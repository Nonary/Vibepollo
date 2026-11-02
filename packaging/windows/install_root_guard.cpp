#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <msiquery.h>
#include <aclapi.h>
#include <shlobj.h>
#include <sddl.h>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include "install_root_guard.h"
namespace {
  struct handle_t {
    HANDLE value;
    explicit handle_t(HANDLE h):value(h) {}
    ~handle_t(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    handle_t(const handle_t&)=delete;
  };
  std::wstring property(MSIHANDLE session,const wchar_t *name) {
    DWORD size=0;wchar_t empty=0;
    UINT result=MsiGetPropertyW(session,name,&empty,&size);
    if(result!=ERROR_MORE_DATA && result!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"msi_property",install_root_guard::role_t::none,L"MsiGetPropertyW",result);
    std::wstring value(size+1,L'\0');++size;
    result=MsiGetPropertyW(session,name,value.data(),&size);
    if(result!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"msi_property",install_root_guard::role_t::none,L"MsiGetPropertyW",result);
    value.resize(size);return value;
  }
  std::wstring lower(std::wstring s){for(auto &ch:s)ch=static_cast<wchar_t>(std::towlower(ch));return s;}
  bool trusted(PSID sid) {
    if(!sid || !IsValidSid(sid))return false;
    if(IsWellKnownSid(sid,WinLocalSystemSid)||IsWellKnownSid(sid,WinBuiltinAdministratorsSid))return true;
    PSID installer=nullptr;
    if(!ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464",&installer))return false;
    const bool ok=EqualSid(sid,installer)!=FALSE;LocalFree(installer);return ok;
  }
  std::wstring hex_value(DWORD value) {
    wchar_t buffer[11]{};
    swprintf(buffer,11,L"0x%08lx",static_cast<unsigned long>(value));
    return buffer;
  }
  std::wstring sid_text(PSID sid) {
    if(!sid || !IsValidSid(sid))return L"missing_or_invalid";
    LPWSTR raw=nullptr;
    if(!ConvertSidToStringSidW(sid,&raw))return L"unavailable(status="+std::to_wstring(GetLastError())+L")";
    auto free_sid=std::unique_ptr<void,decltype(&LocalFree)>(raw,LocalFree);
    return raw;
  }
  std::wstring security_evidence(PSECURITY_DESCRIPTOR descriptor,PSID owner,PACL dacl,bool strict,DWORD ace_index) {
    // Use the descriptor from the rejected, pinned object. Reopening its path
    // after failure could report different ownership/permissions after a race.
    std::wstring result=L" strict="+std::to_wstring(strict)+L" owner_sid="+sid_text(owner)+
      L" owner_trusted="+std::to_wstring(trusted(owner))+L" dacl="+(dacl?L"present":L"null_or_absent");
    BOOL present=FALSE,defaulted=FALSE;PACL raw_dacl=nullptr;
    if(GetSecurityDescriptorDacl(descriptor,&present,&raw_dacl,&defaulted))
      result+=L" dacl_present="+std::to_wstring(present!=FALSE)+L" dacl_null="+std::to_wstring(raw_dacl==nullptr);
    else result+=L" dacl_diagnostic_status="+std::to_wstring(GetLastError());
    SECURITY_DESCRIPTOR_CONTROL control=0;DWORD revision=0;
    if(GetSecurityDescriptorControl(descriptor,&control,&revision))result+=L" sd_control="+hex_value(control);
    else result+=L" sd_control_status="+std::to_wstring(GetLastError());
    if(dacl) {
      result+=L" ace_count="+std::to_wstring(dacl->AceCount);
      if(ace_index<dacl->AceCount) {
        void *raw=nullptr;
        if(GetAce(dacl,ace_index,&raw)) {
          const auto header=static_cast<ACE_HEADER*>(raw);
          result+=L" ace_index="+std::to_wstring(ace_index)+L" ace_type="+hex_value(header->AceType)+
            L" ace_flags="+hex_value(header->AceFlags);
          if(header->AceType==ACCESS_ALLOWED_ACE_TYPE) {
            const auto ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
            result+=L" ace_sid="+sid_text(&ace->SidStart)+L" ace_mask="+hex_value(ace->Mask);
          }
        } else result+=L" ace_diagnostic_status="+std::to_wstring(GetLastError());
      }
    }
    LPWSTR raw=nullptr;
    if(ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor,SDDL_REVISION_1,
        OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&raw,nullptr)) {
      auto free_sddl=std::unique_ptr<void,decltype(&LocalFree)>(raw,LocalFree);
      std::wstring sddl=raw;
      // Keep the exact rejected ACE above even when an unusually large ACL
      // exceeds the diagnostic budget. Never silently omit truncation.
      result+=L" sddl_chars="+std::to_wstring(sddl.size())+L" sddl_truncated="+std::to_wstring(sddl.size()>8192);
      sddl.resize(std::min<std::size_t>(sddl.size(),8192));
      for(auto &ch:sddl)if(ch==L'\r' || ch==L'\n')ch=L' ';
      result+=L" sddl=\""+sddl+L"\"";
    } else result+=L" sddl_status="+std::to_wstring(GetLastError());
    return result;
  }
  struct root_check_t { bool valid; const wchar_t *api; DWORD status; std::wstring evidence={}; };
  root_check_t is_volume_root(HANDLE file,const BY_HANDLE_FILE_INFORMATION &info) {
    // Resolve the object already opened without following its final reparse
    // point. Failure, truncation, an alias to a directory, or an unfamiliar
    // format must all leave the normal ACL rejection in force.
    wchar_t resolved[50]{};
    const DWORD length=GetFinalPathNameByHandleW(file,resolved,50,FILE_NAME_NORMALIZED|VOLUME_NAME_GUID);
    if(!length)return {false,L"GetFinalPathNameByHandleW",GetLastError()};
    auto evidence=L" resolved_chars="+std::to_wstring(length);
    if(length<50)evidence+=L" resolved_path=\""+std::wstring(resolved,length)+L"\"";
    if(length!=49 || !install_root_guard::is_volume_guid_root(std::wstring(resolved,length)))return {false,L"none",0,evidence};
    evidence+=L" pinned_volume="+hex_value(info.dwVolumeSerialNumber)+L" pinned_file_id_high="+hex_value(info.nFileIndexHigh)+
      L" pinned_file_id_low="+hex_value(info.nFileIndexLow);
    // Confirm the canonical root is the same filesystem object as the pinned
    // handle, not merely a name that looks like a root. Unknown IDs fail closed.
    handle_t root(CreateFileW(resolved,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
      nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(root.value==INVALID_HANDLE_VALUE)return {false,L"CreateFileW",GetLastError(),evidence};
    BY_HANDLE_FILE_INFORMATION root_info{};
    if(!GetFileInformationByHandle(root.value,&root_info))return {false,L"GetFileInformationByHandle",GetLastError(),evidence};
    evidence+=L" canonical_attributes="+hex_value(root_info.dwFileAttributes)+L" canonical_volume="+hex_value(root_info.dwVolumeSerialNumber)+
      L" canonical_file_id_high="+hex_value(root_info.nFileIndexHigh)+L" canonical_file_id_low="+hex_value(root_info.nFileIndexLow);
    if(!(root_info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (root_info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return {false,L"none",0,evidence};
    return {install_root_guard::same_file_identity(info.dwVolumeSerialNumber,info.nFileIndexHigh,info.nFileIndexLow,
      root_info.dwVolumeSerialNumber,root_info.nFileIndexHigh,root_info.nFileIndexLow),L"none",0,evidence};
  }
  struct filesystem_t {
    std::vector<std::unique_ptr<handle_t>> pins;
    bool preview;
    std::function<void(const install_root_guard::failure_t&)> diagnostic={};
    void note(const install_root_guard::failure_t &failure) { if(diagnostic)diagnostic(failure); }
    install_root_guard::node_t pin(const std::wstring &path,bool strict,install_root_guard::role_t role) {
      try { return pin_checked(path,strict,role); }
      catch(install_root_guard::failure_t &failure) { failure.path=path;throw; }
    }
    install_root_guard::node_t pin_checked(const std::wstring &path,bool strict,install_root_guard::role_t role) {
      using namespace install_root_guard;
      auto file=std::make_unique<handle_t>(CreateFileW(path.c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | (preview ? FILE_SHARE_WRITE : 0),nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
      if(file->value==INVALID_HANDLE_VALUE){
        const DWORD error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)return {false,false,error};
        throw failure_t(L"open",role,L"CreateFileW",error);
      }
      BY_HANDLE_FILE_INFORMATION info{};
      if(!GetFileInformationByHandle(file->value,&info))throw failure_t(L"attributes",role,L"GetFileInformationByHandle",GetLastError());
      if(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) {
        failure_t failure(L"reparse",role);
        try {
          failure.evidence=L" attributes="+hex_value(info.dwFileAttributes);
          FILE_ATTRIBUTE_TAG_INFO tag{};
          if(GetFileInformationByHandleEx(file->value,FileAttributeTagInfo,&tag,sizeof(tag)))
            failure.evidence+=L" reparse_tag="+hex_value(tag.ReparseTag);
          else failure.evidence+=L" reparse_tag_status="+std::to_wstring(GetLastError());
        } catch(...) {}
        throw failure;
      }
      PSID owner=nullptr;PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
      const DWORD security_status=GetSecurityInfo(file->value,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
          &owner,nullptr,&dacl,nullptr,&descriptor);
      if(security_status!=ERROR_SUCCESS)throw failure_t(L"security",role,L"GetSecurityInfo",security_status);
      auto free_descriptor=std::unique_ptr<void,decltype(&LocalFree)>(descriptor,LocalFree);
      DWORD i=MAXDWORD;
      try {
        if(!trusted(owner))throw failure_t(L"untrusted_owner",role);
        if(!dacl)throw failure_t(L"null_dacl",role);
        for(i=0;i<dacl->AceCount;++i){
          void *raw=nullptr;if(!GetAce(dacl,i,&raw))throw failure_t(L"acl_read",role,L"GetAce",GetLastError());
          const auto header=static_cast<ACE_HEADER*>(raw);
          if(header->AceType==ACCESS_ALLOWED_ACE_TYPE){
            const auto ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
            const bool inherit_only=(header->AceFlags&INHERIT_ONLY_ACE)!=0;
            const bool trusted_sid=trusted(&ace->SidStart);
            const bool creator_owner=IsWellKnownSid(&ace->SidStart,WinCreatorOwnerSid)!=FALSE;
            if(!install_root_guard::allow_ace_safe(ace->Mask,inherit_only,strict,trusted_sid,creator_owner)) {
              // Apply the exception only when DELETE is the sole rejected bit,
              // and only to the already-opened lexical drive-root object after
              // resolving it to an actual volume root.
              root_check_t identity{false,L"none",0};
              const bool identity_attempted=!strict && path.size()==3 &&
                 install_root_guard::allow_ace_safe(ace->Mask,inherit_only,strict,trusted_sid,creator_owner,true);
              if(identity_attempted)
                identity=is_volume_root(file->value,info);
              if(!identity.valid) {
                failure_t failure(identity_attempted?L"identity_unproven":L"untrusted_allow",role,identity.api,identity.status,unsafe_access_bits(ace->Mask,strict));
                failure.evidence=std::move(identity.evidence);
                throw failure;
              }
            }
          }else if(header->AceType!=ACCESS_DENIED_ACE_TYPE && header->AceType!=SYSTEM_AUDIT_ACE_TYPE)
            throw failure_t(L"unsupported_ace",role);
        }
      } catch(failure_t &failure) {
        // Diagnostic failures must never replace the original policy decision.
        try { failure.evidence+=security_evidence(descriptor,owner,dacl,strict,i); } catch(...) {}
        throw;
      }
      pins.push_back(std::move(file));return {true,(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0};
    }
  };
  struct msi_handle_t {
    MSIHANDLE value=0;
    explicit msi_handle_t(MSIHANDLE handle=0):value(handle) {}
    ~msi_handle_t(){if(value)MsiCloseHandle(value);}
    msi_handle_t(const msi_handle_t&)=delete;
  };
  void inventory_status(UINT status,const wchar_t *api) {
    if(status!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"package_inventory",install_root_guard::role_t::none,api,status);
  }
  std::wstring record_text(MSIHANDLE record,UINT field) {
    DWORD length=0;wchar_t empty=0;
    const auto status=MsiRecordGetStringW(record,field,&empty,&length);
    if(status!=ERROR_MORE_DATA)inventory_status(status,L"MsiRecordGetStringW");
    std::wstring text(length+1,L'\0');++length;
    inventory_status(MsiRecordGetStringW(record,field,text.data(),&length),L"MsiRecordGetStringW");
    text.resize(length);return text;
  }
  std::vector<std::vector<std::wstring>> inventory_rows(MSIHANDLE database,const wchar_t *sql,UINT fields) {
    msi_handle_t view;
    inventory_status(MsiDatabaseOpenViewW(database,sql,&view.value),L"MsiDatabaseOpenViewW");
    inventory_status(MsiViewExecute(view.value,0),L"MsiViewExecute");
    std::vector<std::vector<std::wstring>> result;
    for(;;) {
      msi_handle_t record;
      const auto status=MsiViewFetch(view.value,&record.value);
      if(status==ERROR_NO_MORE_ITEMS)return result;
      inventory_status(status,L"MsiViewFetch");
      if(result.size()>=100000)throw install_root_guard::failure_t(L"bounds");
      auto &row=result.emplace_back();
      for(UINT field=1;field<=fields;++field)row.push_back(record_text(record.value,field));
    }
  }
  std::wstring target_name(std::wstring name,bool directory=false,bool short_names=false) {
    // Directory.DefaultDir is target:source; both it and File.FileName may
    // contain the MSI short|long filename pair. Only target names are relevant.
    if(directory)name.resize(name.find(L':')==std::wstring::npos?name.size():name.find(L':'));
    const auto separator=name.find(L'|');
    if(separator!=std::wstring::npos) {
      if(short_names)name.resize(separator);
      else name.erase(0,separator+1);
    }
    return lower(name);
  }
  using diagnostic_fn=std::function<void(const install_root_guard::failure_t&)>;
  using component_resolver_fn=std::function<std::wstring(const std::wstring&)>;
  std::wstring normalized_path(const std::wstring &path) {
    std::vector<wchar_t> result(32768);
    const DWORD length=GetFullPathNameW(path.c_str(),static_cast<DWORD>(result.size()),result.data(),nullptr);
    if(!length || length>=result.size())throw install_root_guard::failure_t(L"package_inventory",install_root_guard::role_t::none,L"GetFullPathNameW",length?0:GetLastError());
    auto normalized=lower(std::wstring(result.data(),length));
    while(normalized.size()>3 && normalized.back()==L'\\')normalized.pop_back();
    return normalized;
  }
  std::wstring relative_to_root(const std::wstring &root,const std::wstring &path,bool incoming) {
    if(path==root)return {};
    if(path.size()>root.size() && path.compare(0,root.size(),root)==0 && path[root.size()]==L'\\')return path.substr(root.size()+1);
    install_root_guard::failure_t failure(incoming?L"outside_anchor":L"package_inventory",install_root_guard::role_t::descendant);
    failure.path=path;failure.evidence=L" install_root=\""+root+L"\"";throw failure;
  }
  std::vector<std::wstring> package_inventory(MSIHANDLE database,MSIHANDLE session=0,
      const std::wstring &installed_root={},const component_resolver_fn &resolve_component={},const diagnostic_fn &diagnostic={}) {
    using namespace install_root_guard;
    // A nonempty SHORTFILENAMES property, including "0", selects short targets.
    const bool short_names=session && !property(session,L"SHORTFILENAMES").empty();
    const auto rows=inventory_rows(database,L"SELECT `Directory`, `Directory_Parent`, `DefaultDir` FROM `Directory`",3);
    std::map<std::wstring,std::pair<std::wstring,std::wstring>> directories;
    for(const auto &row:rows)directories.emplace(row[0],std::make_pair(row[1],row[2]));
    if(!directories.contains(L"INSTALL_ROOT"))throw failure_t(L"package_inventory");
    auto directory_path=[&](std::wstring id)->std::optional<std::wstring> {
      std::vector<std::wstring> parts;std::set<std::wstring> seen;
      while(id!=L"INSTALL_ROOT") {
        if(!seen.insert(id).second)throw failure_t(L"package_inventory");
        const auto found=directories.find(id);
        if(found==directories.end())throw failure_t(L"package_inventory");
        if(found->second.first.empty() || found->second.first==id)return std::nullopt; // external system directory
        const auto name=target_name(found->second.second,true,short_names);
        if(name!=L".") {
          if(!relative_file_path(name) || name.find(L'\\')!=std::wstring::npos)throw failure_t(L"package_inventory");
          parts.push_back(name);
        }
        id=found->second.first;
      }
      std::wstring path;
      for(auto it=parts.rbegin();it!=parts.rend();++it){if(!path.empty())path+=L'\\';path+=*it;}
      return path;
    };
    std::vector<std::wstring> result;
    std::map<std::wstring,std::wstring> costed_directories;
    std::wstring incoming_root;
    if(session)incoming_root=normalized_path(property(session,L"INSTALL_ROOT"));
    const auto registered_root=resolve_component?normalized_path(installed_root):L"";
    const auto files=inventory_rows(database,
      L"SELECT `Component`.`Directory_`, `File`.`FileName`, `File`.`File`, `Component`.`ComponentId`, `Component`.`KeyPath` FROM `Component`, `File` WHERE `Component`.`Component` = `File`.`Component_`",5);
    std::map<std::wstring,std::wstring> file_names,registered_components;
    for(const auto &file:files)file_names.emplace(file[2],file[1]);
    for(const auto &file:files) {
      auto relative=directory_path(file[0]);
      if(!relative)continue;
      if(resolve_component) {
        try {
          auto component=registered_components.find(file[3]);
          if(component==registered_components.end())component=registered_components.emplace(file[3],normalized_path(resolve_component(file[3]))).first;
          const auto &key_path=component->second;
          std::wstring target;
          if(file[2]==file[4])target=key_path; // Registered file key: exact installed short/long name and directory.
          else if(file[4].empty())target=key_path+L"\\"+target_name(file[1]); // Directory key path.
          else {
            const auto key=file_names.find(file[4]);
            if(key==file_names.end())throw failure_t(L"package_inventory"); // Registry/ODBC keys are not filesystem locations.
            const auto actual_name=key_path.substr(key_path.find_last_of(L'\\')+1);
            const bool installed_short=actual_name==target_name(key->second,false,true) && actual_name!=target_name(key->second);
            target=parent(key_path)+L"\\"+target_name(file[1],false,installed_short);
          }
          result.push_back(relative_to_root(registered_root,target,false));
        } catch(failure_t &failure) {
          failure.evidence+=L" file_key="+file[2]+L" component_id="+file[3];
          if(!diagnostic)throw;
          diagnostic(failure);
        }
        continue;
      }
      if(session) {
        // Use the costed incoming paths, including directory-property overrides.
        auto found=costed_directories.find(file[0]);
        if(found==costed_directories.end()) {
          std::vector<wchar_t> path(32768);DWORD length=static_cast<DWORD>(path.size());
          inventory_status(MsiGetTargetPathW(session,file[0].c_str(),path.data(),&length),L"MsiGetTargetPathW");
          auto directory=lower(std::wstring(path.data(),length));
          while(directory.size()>3 && directory.back()==L'\\')directory.pop_back();
          found=costed_directories.emplace(file[0],relative_to_root(incoming_root,directory,true)).first;
        }
        relative=found->second;
      }
      const auto name=target_name(file[1],false,short_names);
      if(!relative_file_path(name) || name.find(L'\\')!=std::wstring::npos)throw failure_t(L"package_inventory");
      result.push_back(relative->empty()?name:*relative+L"\\"+name);
    }
    return result;
  }
  struct scope_t { std::vector<std::wstring> files,directories; };
  bool scheduled_action(MSIHANDLE database,MSIHANDLE session,const wchar_t *action) {
    const auto query=L"SELECT `Condition` FROM `InstallExecuteSequence` WHERE `Action` = '"+std::wstring(action)+L"'";
    const auto rows=inventory_rows(database,query.c_str(),1);
    if(rows.empty())return false;
    const auto condition=MsiEvaluateConditionW(session,rows.front()[0].c_str());
    if(condition==MSICONDITION_ERROR)throw install_root_guard::failure_t(L"package_inventory",install_root_guard::role_t::none,L"MsiEvaluateConditionW",ERROR_INVALID_DATA);
    return condition==MSICONDITION_TRUE || condition==MSICONDITION_NONE;
  }
  scope_t action_scope(MSIHANDLE database,MSIHANDLE session,const diagnostic_fn &diagnostic={}) {
    scope_t scope;
    // Only the active package's scheduled actions apply to its selected root.
    // Never inject these incoming paths into a related product's old root.
    for(const auto &[action,path]:std::initializer_list<std::pair<const wchar_t*,const wchar_t*>>{
        {L"MigrateConfig",L"scripts\\migrate-config.bat"},{L"RunInstallerMigrations",L"scripts\\installer-migrations.ps1"},
        {L"InstallSudovda",L"drivers\\sudovda\\install.ps1"},{L"UninstallSudovda",L"drivers\\sudovda\\install.ps1"},
        {L"InstallVirtualDisplayDriver",L"drivers\\sunshine\\install.ps1"},{L"UninstallVirtualDisplayDriver",L"drivers\\sunshine\\install.ps1"},
        {L"RegisterVulkanHdrLayer",L"drivers\\sunshine\\install.ps1"},{L"UnregisterVulkanHdrLayer",L"drivers\\sunshine\\install.ps1"},
        {L"InstallVirtualGamepadDriver",L"drivers\\vhf-gamepad\\install.ps1"},{L"RemoveVirtualGamepadRoot",L"drivers\\vhf-gamepad\\cleanup.ps1"},
        {L"InstallUsbIpTransport",L"drivers\\usbip-win2\\install.ps1"},{L"RestoreNvPrefsUndo",L"sunshine.exe"},
        {L"FactoryResetAppData",L"scripts\\factory-reset-appdata.ps1"},
        {L"UpdatePathAdd",L"scripts\\update-path.bat"},{L"UpdatePathRemove",L"scripts\\update-path.bat"}}) {
      if(scheduled_action(database,session,action))scope.files.emplace_back(path);
    }
    const bool migrate=scheduled_action(database,session,L"MigrateConfig");
    const bool migrations=scheduled_action(database,session,L"RunInstallerMigrations");
    if(migrate || migrations) {
      scope.directories={L"config",L"config\\credentials"};
      for(const auto name:{L"apps.json",L"sunshine.conf",L"sunshine_state.json",L"sunshine_state.json.bak",L"vibeshine_state.json"}) {
        scope.files.push_back(L"config\\"+std::wstring(name));
        if(migrations || std::wstring(name)!=L"vibeshine_state.json")scope.files.emplace_back(name);
      }
    }
    if(migrate) {
      // The batch migration probes a legacy directory before considering its
      // destination. An unused user covers directory is not an action input.
      const auto root=property(session,L"INSTALL_ROOT");
      auto legacy_directory=[&](const wchar_t *name) {
        const auto path=root+L"\\"+name;
        const auto attributes=GetFileAttributesW(path.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES)return (attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
        const auto status=GetLastError();
        if(status!=ERROR_FILE_NOT_FOUND && status!=ERROR_PATH_NOT_FOUND) {
          install_root_guard::failure_t failure(L"package_inventory",install_root_guard::role_t::descendant,L"GetFileAttributesW",status);
          failure.path=path;
          if(!diagnostic)throw failure;
          diagnostic(failure);
        }
        return false;
      };
      if(legacy_directory(L"credentials"))scope.directories.emplace_back(L"credentials");
      if(legacy_directory(L"covers")) {
        scope.directories.emplace_back(L"covers");scope.directories.emplace_back(L"config\\covers");
      }
    }
    if(scheduled_action(database,session,L"FactoryResetAppData")) {
      scope.directories.push_back(L"config");
      for(const auto base:{L"",L"config\\"}) {
        for(const auto name:{L"credentials",L"covers",L"logs",L"session_history"})scope.directories.push_back(std::wstring(base)+name);
        for(const auto name:{L"apps.json",L"sunshine.conf",L"sunshine.log",L"sunshine_state.json",L"sunshine_state.json.bak",L"vibeshine_state.json",
            L"virtual_display_cache.json",L"nvprefs_undo.json",L"sunshine_playnite.log"})scope.files.push_back(std::wstring(base)+name);
      }
    }
    // Dependencies consumed before their calling driver script can validate
    // them are included even if file-inventory inspection later fails.
    if(scheduled_action(database,session,L"InstallUsbIpTransport"))scope.files.emplace_back(L"drivers\\usbip-win2\\package.ps1");
    if(scheduled_action(database,session,L"UpdatePathAdd") || scheduled_action(database,session,L"UpdatePathRemove"))scope.files.emplace_back(L"scripts\\update-path.ps1");
    if(scheduled_action(database,session,L"InstallSudovda") || scheduled_action(database,session,L"UninstallSudovda"))scope.files.emplace_back(L"drivers\\sudovda\\nefconc.exe");
    if(scheduled_action(database,session,L"InstallVirtualDisplayDriver") || scheduled_action(database,session,L"UninstallVirtualDisplayDriver"))scope.files.emplace_back(L"drivers\\sunshine\\nefconc.exe");
    return scope;
  }
  std::wstring registered_component_path(const std::wstring &code,const std::wstring &component) {
    using component_path_fn=INSTALLSTATE (WINAPI *)(LPCWSTR,LPCWSTR,LPCWSTR,MSIINSTALLCONTEXT,LPWSTR,LPDWORD);
    const auto component_path=reinterpret_cast<component_path_fn>(GetProcAddress(GetModuleHandleW(L"msi.dll"),"MsiGetComponentPathExW"));
    if(!component_path)throw install_root_guard::failure_t(L"package_inventory",install_root_guard::role_t::none,L"GetProcAddress",GetLastError());
    std::vector<wchar_t> path(32768);DWORD length=static_cast<DWORD>(path.size());
    const auto state=component_path(code.c_str(),component.c_str(),nullptr,MSIINSTALLCONTEXT_MACHINE,path.data(),&length);
    if(state!=INSTALLSTATE_LOCAL || !length || length>=path.size())throw install_root_guard::failure_t(L"package_inventory",install_root_guard::role_t::none,L"MsiGetComponentPathExW",static_cast<DWORD>(state));
    return std::wstring(path.data(),length);
  }
  std::vector<std::wstring> registered_inventory(const std::wstring &code,const std::wstring &root,const diagnostic_fn &diagnostic) {
    std::vector<wchar_t> path(32768);DWORD length=static_cast<DWORD>(path.size());
    inventory_status(MsiGetProductInfoExW(code.c_str(),nullptr,MSIINSTALLCONTEXT_MACHINE,L"LocalPackage",path.data(),&length),L"MsiGetProductInfoExW");
    msi_handle_t database;
    const auto status=MsiOpenDatabaseW(path.data(),MSIDBOPEN_READONLY,&database.value);
    if(status!=ERROR_SUCCESS) {
      install_root_guard::failure_t failure(L"package_inventory",install_root_guard::role_t::none,L"MsiOpenDatabaseW",status);
      failure.path=path.data();throw failure;
    }
    return package_inventory(database.value,0,root,[&](const std::wstring &component){return registered_component_path(code,component);},diagnostic);
  }
}
static void validate_one(std::wstring requested,bool removing,bool preview,
    const std::function<void(const install_root_guard::failure_t&)> &diagnostic,const scope_t &scope) {
    while(requested.size()>3 && (requested.back()==L'\\'||requested.back()==L'/'))requested.pop_back();
    if(requested.size()<4 || requested[1]!=L':' || requested[2]!=L'\\' ||
       requested.find_first_of(L"\"'`$%;&|<>^\r\n/")!=std::wstring::npos)
      throw install_root_guard::failure_t(L"invalid_path",install_root_guard::role_t::install_root);
    std::vector<wchar_t> normalized(32768);
    const DWORD length=GetFullPathNameW(requested.c_str(),static_cast<DWORD>(normalized.size()),normalized.data(),nullptr);
    if(!length)throw install_root_guard::failure_t(L"normalize",install_root_guard::role_t::install_root,L"GetFullPathNameW",GetLastError());
    if(length>=normalized.size())throw install_root_guard::failure_t(L"normalize_too_long",install_root_guard::role_t::install_root);
    auto root=lower(std::wstring(normalized.data(),length));
    std::wstring anchor;
    std::wstring folder_evidence;
    install_root_guard::anchor_diagnostic_t folder_diagnostic;
    for(const KNOWNFOLDERID *id : {&FOLDERID_ProgramFiles,&FOLDERID_ProgramFilesX86}) {
      PWSTR raw=nullptr;
      const HRESULT folder_status=SHGetKnownFolderPath(*id,0,nullptr,&raw);
      folder_evidence+=std::wstring(id==&FOLDERID_ProgramFiles?L" program_files_status=":L" program_files_x86_status=")+hex_value(folder_status);
      folder_diagnostic.record(SUCCEEDED(folder_status),static_cast<std::uint32_t>(folder_status));
      if(SUCCEEDED(folder_status)){
        auto candidate=lower(raw);CoTaskMemFree(raw);
        folder_evidence+=std::wstring(id==&FOLDERID_ProgramFiles?L" program_files=\"":L" program_files_x86=\"")+candidate+L"\"";
        if(root.size()>candidate.size() && root.compare(0,candidate.size(),candidate)==0 && root[candidate.size()]==L'\\')anchor=candidate;
      }
    }
    if(const auto reason=folder_diagnostic.rejection(!anchor.empty(),removing)) {
      install_root_guard::failure_t failure(reason,install_root_guard::role_t::install_root,
        folder_diagnostic.failed?L"SHGetKnownFolderPath":L"none",folder_diagnostic.failed?folder_diagnostic.status:0);
      failure.evidence=L" normalized_root=\""+root+L"\""+folder_evidence;
      throw failure;
    }
    if(anchor.empty()) {
      // Retain removal of trusted legacy installations outside Program Files.
      // Ancestors must still deny untrusted rename/delete/control operations.
      anchor=root.substr(0,3);
    }
    filesystem_t fs{{},preview,diagnostic};install_root_guard::validate(fs,root,anchor,!removing || anchor.size()>3,scope.files,scope.directories);

}
static std::wstring registry_string(const std::wstring &key,const wchar_t *name) {
  DWORD bytes=0;
  constexpr DWORD flags=RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY;
  const LSTATUS status=RegGetValueW(HKEY_LOCAL_MACHINE,key.c_str(),name,flags,nullptr,nullptr,&bytes);
  if(status==ERROR_FILE_NOT_FOUND || status==ERROR_PATH_NOT_FOUND)return {};
  if(status!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"registry",install_root_guard::role_t::none,L"RegGetValueW",status);
  if(bytes<sizeof(wchar_t) || bytes>65536)throw install_root_guard::failure_t(L"registry_size");
  std::wstring value(bytes/sizeof(wchar_t),L'\0');
  const LSTATUS read_status=RegGetValueW(HKEY_LOCAL_MACHINE,key.c_str(),name,flags,nullptr,value.data(),&bytes);
  if(read_status!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"registry",install_root_guard::role_t::none,L"RegGetValueW",read_status);
  while(!value.empty() && value.back()==L'\0')value.pop_back();
  return value;
}
static const std::wstring uninstall_key=L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
static std::wstring registered_root(const std::wstring &code) {
  DWORD size=0;wchar_t empty=0;
  auto status=MsiGetProductInfoExW(code.c_str(),nullptr,MSIINSTALLCONTEXT_MACHINE,L"InstallLocation",&empty,&size);
  if(status==ERROR_MORE_DATA || (status==ERROR_SUCCESS && size)) {
    std::wstring value(size+1,L'\0');++size;
    const auto info_status=MsiGetProductInfoExW(code.c_str(),nullptr,MSIINSTALLCONTEXT_MACHINE,L"InstallLocation",value.data(),&size);
    if(info_status!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"product_location",install_root_guard::role_t::none,L"MsiGetProductInfoExW",info_status);
    value.resize(size);if(!value.empty())return value;
  }
  auto location=registry_string(uninstall_key+code,L"InstallLocation");
  if(!location.empty())return location;
  for(const auto name : {L"Sunshine",L"Vibeshine",L"Apollo",L"Vibepollo"}) {
    const auto key=uninstall_key+name;
    if(lower(registry_string(key,L"ProductCode"))==lower(code)) {
      location=registry_string(key,L"InstallLocation");
      if(!location.empty())return location;
    }
  }
  // Historical MSIs did not publish ARPINSTALLLOCATION. Their stable
  // directory-key-path component binds INSTALL_ROOT to this exact machine
  // product; a named ARP entry alone is not an identity proof.
  std::vector<wchar_t> path(32768);DWORD length=static_cast<DWORD>(path.size());
  // Some MinGW SDK headers omit this Windows 7+ API. Resolve its exact
  // signature from the already loaded system MSI module, with no PATH lookup.
  using component_path_fn=INSTALLSTATE (WINAPI *)(LPCWSTR,LPCWSTR,LPCWSTR,MSIINSTALLCONTEXT,LPWSTR,LPDWORD);
  const auto component_path=reinterpret_cast<component_path_fn>(GetProcAddress(GetModuleHandleW(L"msi.dll"),"MsiGetComponentPathExW"));
  if(!component_path)throw install_root_guard::failure_t(L"installed_location_unresolved",install_root_guard::role_t::none,L"GetProcAddress",GetLastError());
  // Keep both Vibepollo and historical Sunshine directory components.
  INSTALLSTATE component_state=INSTALLSTATE_UNKNOWN;
  for(const auto component : {L"{640D87CF-69A4-4F54-AE29-941F78D020B2}",L"{B6D8A6A3-63B7-4C3F-8A2C-2C8F2B2F3B61}"}) {
    length=static_cast<DWORD>(path.size());
    component_state=component_path(code.c_str(),component,nullptr,MSIINSTALLCONTEXT_MACHINE,path.data(),&length);
    if(component_state==INSTALLSTATE_LOCAL && length>3 && length<path.size())
      return std::wstring(path.data(),length);
  }
  throw install_root_guard::failure_t(L"installed_location_unresolved",install_root_guard::role_t::none,L"MsiGetComponentPathExW",static_cast<std::uint32_t>(component_state));
}
static const wchar_t *role_name(install_root_guard::role_t role) {
  using install_root_guard::role_t;
  switch(role) {
    case role_t::drive_ancestor:return L"drive_ancestor";
    case role_t::anchor:return L"anchor";
    case role_t::ancestor:return L"ancestor";
    case role_t::install_root:return L"install_root";
    case role_t::descendant:return L"descendant";
    default:return L"none";
  }
}
static const wchar_t *failure_description(const wchar_t *reason) {
  const std::wstring code=reason;
  if(code==L"open")return L"Could not inspect a busy or inaccessible install path";
  if(code==L"security" || code==L"acl_read")return L"Could not read install path permissions";
  if(code==L"unsupported_ace")return L"An install path has an unrecognized permission entry";
  if(code==L"installed_location_unresolved")return L"Could not resolve a registered product folder";
  if(code==L"package_inventory")return L"Could not fully resolve the package inputs; available inputs and install-root checks continue";
  if(code==L"known_folder")return L"Windows could not provide a Program Files location";
  if(code==L"identity_unproven")return L"The drive root identity could not be confirmed";
  if(code==L"untrusted_allow")return L"Another account can modify an install path";
  if(code==L"untrusted_owner")return L"An install path owner is outside the trusted SYSTEM, Administrators, and TrustedInstaller accounts";
  if(code==L"null_dacl")return L"An install path has a missing or null permissions list";
  if(code==L"reparse")return L"An install path is a junction or symbolic link";
  if(code==L"invalid_path" || code==L"outside_program_files")return L"The install folder is not a supported direct Program Files path";
  if(code==L"bounds")return L"The install directory inspection reached its safety limit";
  if(code==L"missing")return L"An expected install directory is missing";
  if(code==L"tree_changed")return L"The install directory changed during inspection";
  return L"Install path validation reported a problem";
}
static std::wstring failure_detail(bool preview,const wchar_t *category,const install_root_guard::failure_t &failure) {
  std::wstring system;
  wchar_t *raw=nullptr;
  if(failure.status && FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER|FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr,failure.status,0,reinterpret_cast<wchar_t*>(&raw),0,nullptr) && raw) {
    system=raw;LocalFree(raw);
    for(auto &ch:system)if(ch==L'\r' || ch==L'\n')ch=L' ';
  }
  return std::wstring(failure_description(failure.reason))+L"; phase="+(preview?L"preview":L"strict")+
    L" category="+category+L" role="+role_name(failure.role)+L" reason="+failure.reason+
    L" api="+failure.api+L" status="+std::to_wstring(failure.status)+L" access="+std::to_wstring(failure.access)+
    (system.empty()?L"":L" ("+system+L")")+(failure.path.empty()?L"":L" path="+failure.path)+failure.evidence;
}
static void log_failure(MSIHANDLE session,bool preview,const wchar_t *category,const install_root_guard::failure_t &failure) noexcept {
  try {
    const bool fatal=install_root_guard::critical(failure);
    const bool informational=!fatal && failure.role==install_root_guard::role_t::drive_ancestor;
    const auto detail=failure_detail(preview,category,failure);
    const auto message=(fatal?L"INSTALL_ROOT_ERROR: ":informational?L"INSTALL_ROOT_INFO: ":L"INSTALL_ROOT_WARNING: ")+detail;
    // Keep paths in a record field so brackets in a filename cannot be treated
    // as MSI formatting instructions. Diagnostics never change the outcome.
    MSIHANDLE record=MsiCreateRecord(1);
    if(record){MsiRecordSetStringW(record,0,L"Install root guard: [1]");MsiRecordSetStringW(record,1,message.c_str());MsiProcessMessage(session,INSTALLMESSAGE_INFO,record);MsiCloseHandle(record);}
    if(fatal)MsiSetPropertyW(session,L"VIBESHINE_INSTALL_ROOT_ERROR",detail.c_str());
    else {
      const auto name=informational?L"VIBESHINE_INSTALL_ROOT_DIAGNOSTICS":L"VIBESHINE_INSTALL_ROOT_WARNINGS";
      auto warnings=property(session,name);
      if(warnings.size()<16000) {
        if(!warnings.empty())warnings+=L"\n";
        warnings+=detail;
        MsiSetPropertyW(session,name,warnings.c_str());
      }
    }
  } catch(...) {}
}
static void check_root(MSIHANDLE session,std::wstring root,bool removing,bool preview,const wchar_t *category,
    const scope_t &scope) {
  try { validate_one(root,removing,preview,[&](const install_root_guard::failure_t &failure) { log_failure(session,preview,category,failure); },scope); }
  catch(install_root_guard::failure_t &failure) {
    if(failure.path.empty())failure.path=root;
    if(install_root_guard::critical(failure))throw;
    log_failure(session,preview,category,failure);
  }
}
static UINT validate_root(MSIHANDLE session,bool preview) {
 const wchar_t *category=L"none";
 try {
  MsiSetPropertyW(session,L"VIBESHINE_INSTALL_ROOT_ERROR",L"");
  MsiSetPropertyW(session,L"VIBESHINE_INSTALL_ROOT_WARNINGS",L"");
  MsiSetPropertyW(session,L"VIBESHINE_INSTALL_ROOT_DIAGNOSTICS",L"");
  const auto code=property(session,L"ProductCode");DWORD product_size=0;wchar_t product=0;
  const auto status=MsiGetProductInfoExW(code.c_str(),nullptr,MSIINSTALLCONTEXT_MACHINE,L"ProductName",&product,&product_size);
  const bool installed=status==ERROR_SUCCESS || status==ERROR_MORE_DATA;
  const bool removing=installed && lower(property(session,L"REMOVE"))==L"all";
  auto inventory=[&](const std::wstring &product,const std::wstring &root) {
    scope_t scope;
    const diagnostic_fn diagnostic=[&](const install_root_guard::failure_t &failure){log_failure(session,preview,category,failure);};
    auto report=[&](const install_root_guard::failure_t &failure) {
      if(install_root_guard::critical(failure))throw;
      diagnostic(failure);
    };
    if(product.empty()) {
      msi_handle_t database(MsiGetActiveDatabase(session));
      if(!database.value) { diagnostic(install_root_guard::failure_t(L"package_inventory",install_root_guard::role_t::none,L"MsiGetActiveDatabase"));return scope; }
      try { scope=action_scope(database.value,session,diagnostic); } catch(const install_root_guard::failure_t &failure) { report(failure); }
      try {
        auto files=package_inventory(database.value,session);
        scope.files.insert(scope.files.end(),files.begin(),files.end());
      } catch(const install_root_guard::failure_t &failure) { report(failure); }
    } else {
      // Registered products use only their own installed inventory. Conditions
      // and optional inputs of the incoming version do not describe old roots.
      try { scope.files=registered_inventory(product,root,diagnostic); }
      catch(const install_root_guard::failure_t &failure) { report(failure); }
    }
    return scope;
  };
  category=L"selected";
  const auto selected_root=property(session,L"INSTALL_ROOT");
  check_root(session,selected_root,removing,preview,category,inventory(L"",selected_root));
  category=L"installed";
  if(installed) {
    try { const auto root=registered_root(code);check_root(session,root,true,preview,category,inventory(code,root)); }
    catch(const install_root_guard::failure_t &failure) {
      if(install_root_guard::critical(failure))throw;
      log_failure(session,preview,category,failure);
    }
  }
  // MajorUpgrade removes related products using their recorded directories,
  // even when the incoming product is installed in a different directory.
  category=L"related";
  const auto upgrade=property(session,L"UpgradeCode");
  if(upgrade.empty())throw install_root_guard::failure_t(L"missing_upgrade_identity");
  for(DWORD index=0;;++index) {
    wchar_t related[39]={};
    const auto result=MsiEnumRelatedProductsW(upgrade.c_str(),0,index,related);
    if(result==ERROR_NO_MORE_ITEMS)break;
    if(result!=ERROR_SUCCESS)throw install_root_guard::failure_t(L"related_enumeration",install_root_guard::role_t::none,L"MsiEnumRelatedProductsW",result);
    if(lower(related)!=lower(code)) {
      try { const auto root=registered_root(related);check_root(session,root,true,preview,category,inventory(related,root)); }
      catch(const install_root_guard::failure_t &failure) {
        if(install_root_guard::critical(failure))throw;
        log_failure(session,preview,category,failure);
      }
    }
  }
  // Match the pre-transaction competing-product action's machine registration
  // roots; do not touch or validate those products when the caller skips it.
  category=L"conflicting";
  if(!installed && !removing && property(session,L"SKIP_REMOVE_CONFLICTING_PRODUCTS")!=L"1") {
    for(const auto name : {L"Sunshine",L"Vibeshine",L"Apollo",L"Vibepollo"}) {
      auto location=registry_string(uninstall_key+name,L"InstallLocation");
      if(!location.empty()) {
        const auto product=registry_string(uninstall_key+name,L"ProductCode");
        // ProductCode-less legacy registrations name the installed uninstaller;
        // do not apply this package's optional actions to those directories.
        check_root(session,location,true,preview,category,product.empty()?scope_t{{L"uninstall.exe"},{}}:inventory(product,location));
      }
    }
  }
  return ERROR_SUCCESS;
 }catch(const install_root_guard::failure_t &failure) {
   log_failure(session,preview,category,failure);
   if(!install_root_guard::critical(failure))return ERROR_SUCCESS;
 }
  catch(...) {log_failure(session,preview,category,install_root_guard::failure_t(L"unexpected"));}
 MSIHANDLE record=MsiCreateRecord(1);
 if(record){MsiRecordSetStringW(record,0,L"Installation stopped because an install path is unsafe. [1]");MsiRecordSetStringW(record,1,property(session,L"VIBESHINE_INSTALL_ROOT_ERROR").c_str());MsiProcessMessage(session,INSTALLMESSAGE_ERROR,record);MsiCloseHandle(record);}
 return ERROR_INSTALL_FAILURE;
}
extern "C" __declspec(dllexport) UINT __stdcall ValidateInstallRootPreview(MSIHANDLE session) {
 return validate_root(session,true);
}
extern "C" __declspec(dllexport) UINT __stdcall ValidateInstallRoot(MSIHANDLE session) {
 return validate_root(session,false);
}
