// Exercise production rejection and formatting with deterministic security
// descriptors. Only GetSecurityInfo is substituted; filesystem opens, SID/ACL
// parsing, trust policy, SDDL conversion, and failure formatting are production.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <msiquery.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

static std::wstring fixture_sddl;
static std::wstring fixture_first_sddl;
static unsigned fixture_calls=0;
static bool fixture_bad_destination=false;
static std::wstring fixture_untrusted_suffix;
static std::map<std::wstring,std::wstring> fixture_properties;
static std::map<std::wstring,std::wstring> fixture_targets;
static std::map<std::wstring,MSICONDITION> fixture_conditions;
static MSICONDITION WINAPI fixture_condition(MSIHANDLE,LPCWSTR condition) {
  if(!condition || !*condition)return MSICONDITION_NONE;
  const auto found=fixture_conditions.find(condition);
  return found==fixture_conditions.end()?MSICONDITION_FALSE:found->second;
}
static UINT WINAPI fixture_target_path(MSIHANDLE session,LPCWSTR name,LPWSTR value,LPDWORD size) {
  if(fixture_targets.empty())return MsiGetTargetPathW(session,name,value,size);
  const auto found=fixture_targets.find(name);
  if(found==fixture_targets.end())return ERROR_DIRECTORY;
  const auto capacity=*size;*size=static_cast<DWORD>(found->second.size());
  if(capacity<=found->second.size())return ERROR_MORE_DATA;
  std::copy(found->second.begin(),found->second.end(),value);value[found->second.size()]=L'\0';return ERROR_SUCCESS;
}
static UINT WINAPI fixture_set_property(MSIHANDLE,LPCWSTR name,LPCWSTR value) {
  fixture_properties[name]=value?value:L"";return ERROR_SUCCESS;
}
static UINT WINAPI fixture_get_property(MSIHANDLE,LPCWSTR name,LPWSTR value,LPDWORD size) {
  const auto &text=fixture_properties[name];
  const auto capacity=*size;*size=static_cast<DWORD>(text.size());
  if(capacity<=text.size())return ERROR_MORE_DATA;
  std::copy(text.begin(),text.end(),value);value[text.size()]=L'\0';return ERROR_SUCCESS;
}
static DWORD fixture_status=ERROR_SUCCESS;
static bool fixture_conversion_failure=false;
static BOOL WINAPI fixture_convert(PSECURITY_DESCRIPTOR descriptor,DWORD revision,
    SECURITY_INFORMATION information,LPWSTR *text,PULONG size) {
  if(fixture_conversion_failure) { SetLastError(ERROR_ACCESS_DENIED);return FALSE; }
  return ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor,revision,information,text,size);
}
static DWORD WINAPI fixture_security(HANDLE file,SE_OBJECT_TYPE,SECURITY_INFORMATION,
    PSID *owner,PSID*,PACL *dacl,PACL*,PSECURITY_DESCRIPTOR *descriptor) {
  if(fixture_status)return fixture_status;
  auto sddl=fixture_sddl;
  if(!fixture_first_sddl.empty() && fixture_calls++==0)sddl=fixture_first_sddl;
  if(fixture_bad_destination || !fixture_untrusted_suffix.empty()) {
    wchar_t path[32768]{};
    const auto length=GetFinalPathNameByHandleW(file,path,32768,FILE_NAME_NORMALIZED);
    if(fixture_bad_destination && length && length<32768 && std::wstring(path,length).ends_with(L"\\host"))sddl=L"O:SYD:(A;;0x40;;;BU)";
    if(length && length<32768 && !fixture_untrusted_suffix.empty() && std::wstring(path,length).ends_with(fixture_untrusted_suffix))
      sddl=L"O:S-1-5-21-1-2-3-1001D:(A;;FA;;;SY)(A;;FA;;;S-1-5-21-1-2-3-1001)";
  }
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,descriptor,nullptr))return GetLastError();
  BOOL defaulted=FALSE,present=FALSE;
  GetSecurityDescriptorOwner(*descriptor,owner,&defaulted);
  GetSecurityDescriptorDacl(*descriptor,&present,dacl,&defaulted);
  return ERROR_SUCCESS;
}
#define GetSecurityInfo fixture_security
#define ConvertSecurityDescriptorToStringSecurityDescriptorW fixture_convert
#define MsiSetPropertyW fixture_set_property
#define MsiGetPropertyW fixture_get_property
#define MsiGetTargetPathW fixture_target_path
#define MsiEvaluateConditionW fixture_condition
#include "../../packaging/windows/install_root_guard.cpp"
#undef GetSecurityInfo
#undef ConvertSecurityDescriptorToStringSecurityDescriptorW
#undef MsiSetPropertyW
#undef MsiGetPropertyW
#undef MsiGetTargetPathW
#undef MsiEvaluateConditionW

static void require(bool value,const char *message) {
  if(!value)throw std::runtime_error(message);
}
static void contains(const std::wstring &text,const std::wstring &value) {
  if(text.find(value)==std::wstring::npos) {
    std::wcerr<<L"Missing "<<value<<L" in "<<text<<L"\n";
    throw std::runtime_error("missing rejection evidence");
  }
}
int main() {
  wchar_t temporary[MAX_PATH]{},path[MAX_PATH]{};
  require(GetTempPathW(MAX_PATH,temporary)!=0,"temporary directory");
  require(GetTempFileNameW(temporary,L"vrd",0,path)!=0,"fixture name");
  require(DeleteFileW(path)!=FALSE && CreateDirectoryW(path,nullptr)!=FALSE,"fixture directory");
  try {
    using namespace install_root_guard;
    auto rejected=[&](const wchar_t *sddl,const wchar_t *reason,bool preview=true) {
      fixture_sddl=sddl;
      filesystem_t fs{{},preview};
      try { fs.pin(path,false,role_t::drive_ancestor); }
      catch(const failure_t &failure) {
        require(std::wstring(failure.reason)==reason,"exact rejection reason");
        require(failure.path==path,"rejected path retained");
        const auto detail=failure_detail(preview,L"selected",failure);
        contains(detail,L"role=drive_ancestor");
        contains(detail,preview?L"phase=preview":L"phase=strict");
        contains(detail,L"path="+std::wstring(path));
        return detail;
      }
      throw std::runtime_error("unsafe fixture accepted");
    };
    auto detail=rejected(L"O:S-1-5-21-1-2-3-1001D:(A;;FA;;;SY)",L"untrusted_owner");
    contains(detail,L"owner_sid=S-1-5-21-1-2-3-1001");
    contains(detail,L"owner_trusted=0 dacl=present");
    contains(detail,L"sddl=\"O:S-1-5-21-1-2-3-1001D:");
    detail=rejected(L"O:SY",L"null_dacl");
    contains(detail,L"owner_sid=S-1-5-18 owner_trusted=1 dacl=null_or_absent");
    contains(detail,L"dacl_present=0 dacl_null=1");
    contains(detail,L"sd_control=");
    detail=rejected(L"O:SYD:(A;;0x40;;;BU)",L"untrusted_allow",false);
    contains(detail,L"ace_index=0 ace_type=0x00000000 ace_flags=0x00000000");
    contains(detail,L"ace_sid=S-1-5-32-545 ace_mask=0x00000040");
    contains(detail,L"access=64");
    contains(detail,L"sddl_truncated=0");
    detail=rejected(L"O:SYD:(A;CI;WD;;;BU)",L"untrusted_allow");
    contains(detail,L"ace_flags=0x00000002");
    contains(detail,L"ace_mask=0x00040000");
    // A large descriptor still retains the exact entry that caused rejection.
    std::wstring large=L"O:SYD:";
    for(int i=0;i<700;++i)large+=L"(A;;FR;;;SY)";
    large+=L"(A;;0x40;;;BU)";
    detail=rejected(large.c_str(),L"untrusted_allow");
    contains(detail,L"ace_index=700");
    contains(detail,L"ace_sid=S-1-5-32-545 ace_mask=0x00000040");
    contains(detail,L"sddl_truncated=1");
    require(detail.size()<10000,"bounded rejection evidence");
    fixture_conversion_failure=true;
    detail=rejected(L"O:S-1-5-21-1-2-3-1001D:",L"untrusted_owner");
    contains(detail,L"owner_sid=S-1-5-21-1-2-3-1001");
    contains(detail,L"sddl_status=5");
    fixture_conversion_failure=false;
    // An empty ACL is distinct from a missing ACL and remains accepted.
    fixture_sddl=L"O:SYD:";
    { filesystem_t fs{{},true};require(fs.pin(path,false,role_t::drive_ancestor).exists,"empty DACL accepted"); }
    // If inspection itself fails, preserve the API/status rather than inventing
    // an unsafe owner or permissions list.
    fixture_status=ERROR_ACCESS_DENIED;
    detail=rejected(L"O:SYD:",L"security");
    contains(detail,L"api=GetSecurityInfo status=5");
    require(detail.find(L"owner_sid=")==std::wstring::npos,"no fabricated owner");
    fixture_status=ERROR_SUCCESS;
    const std::wstring destination=std::wstring(path)+L"\\host";
    for(const auto first : {L"O:S-1-5-21-1-2-3-1001D:(A;;FA;;;SY)",L"O:SY",L"O:SYD:(A;;0x40;;;BU)"}) {
      fixture_first_sddl=first;fixture_sddl=L"O:SYD:(A;;FA;;;SY)";fixture_calls=0;
      std::vector<failure_t> warnings;
      auto warning=[&](const failure_t &failure){warnings.push_back(failure);};
      { filesystem_t fs{{},true,warning};validate(fs,destination,path,true,{L"sunshine.exe"}); }
      require(warnings.size()==1 && !critical(warnings[0]),"drive root metadata is logged and fresh install proceeds");
      contains(failure_detail(true,L"selected",warnings[0]),L"owner_sid=");
      fixture_properties.clear();log_failure(0,true,L"selected",warnings[0]);
      contains(fixture_properties[L"VIBESHINE_INSTALL_ROOT_DIAGNOSTICS"],L"owner_sid=");
      require(fixture_properties[L"VIBESHINE_INSTALL_ROOT_WARNINGS"].empty(),"drive root findings do not create UI warnings");
      require(fixture_properties[L"VIBESHINE_INSTALL_ROOT_ERROR"].empty(),"drive root findings do not create UI errors");
      require(fixture_calls>1,"destination inspected after root warning");
      require(CreateDirectoryW(destination.c_str(),nullptr)!=FALSE,"destination fixture");
      fixture_calls=0;fixture_bad_destination=true;warnings.clear();
      try {
        filesystem_t fs{{},false,warning};validate(fs,destination,path,false,{L"sunshine.exe"});
        throw std::runtime_error("unsafe destination accepted after root warning");
      } catch(const failure_t &failure) {
        require(failure.role==role_t::install_root && critical(failure),"destination remains critical");
        require(warnings.size()==1,"root warning retained before destination rejection");
        contains(failure.evidence,L"ace_sid=S-1-5-32-545");
      }
      fixture_bad_destination=false;
      require(RemoveDirectoryW(destination.c_str())!=FALSE,"destination cleanup");
    }
    fixture_first_sddl.clear();fixture_sddl=L"O:SYD:(A;;FA;;;SY)";
    const auto assets=std::wstring(path)+L"\\assets",artwork=assets+L"\\windows.png";
    require(CreateDirectoryW(assets.c_str(),nullptr)!=FALSE,"assets fixture");
    { const auto file=CreateFileW(artwork.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
      require(file!=INVALID_HANDLE_VALUE,"custom image fixture");CloseHandle(file); }
    fixture_untrusted_suffix=L"\\windows.png";
    { filesystem_t fs{{},true};validate(fs,path,parent(path),false,{L"assets\\box.png"}); }
    try {
      filesystem_t fs{{},false};validate(fs,path,parent(path),false,{L"assets\\windows.png"});
      throw std::runtime_error("MSI-managed image was exempted by extension");
    } catch(const failure_t &failure) {
      require(failure.path==artwork && std::wstring(failure.reason)==L"untrusted_owner","only the planned image is protected");
      contains(failure.evidence,L"owner_sid=S-1-5-21-1-2-3-1001");
    }
    fixture_untrusted_suffix.clear();
    require(DeleteFileW(artwork.c_str())!=FALSE && RemoveDirectoryW(assets.c_str())!=FALSE,"artwork fixture cleanup");
    const auto config=std::wstring(path)+L"\\config",credentials=config+L"\\credentials";
    require(CreateDirectoryW(config.c_str(),nullptr)!=FALSE && CreateDirectoryW(credentials.c_str(),nullptr)!=FALSE,"migration directory fixture");
    { filesystem_t fs{{},true};validate(fs,path,parent(path),false,{}, {L"config\\credentials"}); }
    fixture_untrusted_suffix=L"\\credentials";
    try {
      filesystem_t fs{{},false};validate(fs,path,parent(path),false,{}, {L"config\\credentials"});
      throw std::runtime_error("unsafe action directory accepted");
    } catch(const failure_t &failure) { require(failure.path==credentials && critical(failure),"action directory rejection names exact path"); }
    fixture_untrusted_suffix.clear();
    require(RemoveDirectoryW(credentials.c_str())!=FALSE && RemoveDirectoryW(config.c_str())!=FALSE,"migration directory cleanup");

    const auto package=std::wstring(path)+L"\\inventory.msi";
    {
      msi_handle_t database;
      require(MsiOpenDatabaseW(package.c_str(),MSIDBOPEN_CREATE,&database.value)==ERROR_SUCCESS,"inventory database");
      auto sql=[&](const wchar_t *query) {
        msi_handle_t view;
        auto status=MsiDatabaseOpenViewW(database.value,query,&view.value);
        if(status!=ERROR_SUCCESS)std::wcerr<<L"MSI SQL view failed ("<<status<<L"): "<<query<<L"\n";
        require(status==ERROR_SUCCESS,"fixture SQL view");
        status=MsiViewExecute(view.value,0);
        if(status!=ERROR_SUCCESS)std::wcerr<<L"MSI SQL execution failed ("<<status<<L"): "<<query<<L"\n";
        require(status==ERROR_SUCCESS,"fixture SQL execution");
      };
      sql(L"CREATE TABLE `Directory` (`Directory` CHAR(72) NOT NULL, `Directory_Parent` CHAR(72), `DefaultDir` CHAR(255) NOT NULL PRIMARY KEY `Directory`)");
      sql(L"CREATE TABLE `Component` (`Component` CHAR(72) NOT NULL, `Directory_` CHAR(72) NOT NULL, `ComponentId` CHAR(72), `KeyPath` CHAR(72) PRIMARY KEY `Component`)");
      sql(L"CREATE TABLE `File` (`File` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL, `FileName` CHAR(255) NOT NULL PRIMARY KEY `File`)");
      sql(L"CREATE TABLE `InstallExecuteSequence` (`Action` CHAR(72) NOT NULL, `Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)");
      sql(L"INSERT INTO `Directory` (`Directory`, `DefaultDir`) VALUES ('TARGETDIR', 'SourceDir')");
      sql(L"INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('INSTALL_ROOT', 'TARGETDIR', 'Sunshine')");
      sql(L"INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('Assets', 'INSTALL_ROOT', 'assets')");
      sql(L"INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('Scripts', 'INSTALL_ROOT', 'SCRIPT~1|scripts:source')");
      sql(L"INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('Alias', 'Scripts', '.:alias-source')");
      sql(L"INSERT INTO `Directory` (`Directory`, `Directory_Parent`, `DefaultDir`) VALUES ('WindowsFolder', 'TARGETDIR', '.')");
      sql(L"INSERT INTO `Component` (`Component`, `Directory_`, `ComponentId`, `KeyPath`) VALUES ('Main', 'INSTALL_ROOT', 'MainID', 'MainFile')");
      sql(L"INSERT INTO `Component` (`Component`, `Directory_`, `ComponentId`, `KeyPath`) VALUES ('Image', 'Assets', 'ImageID', 'ImageFile')");
      sql(L"INSERT INTO `Component` (`Component`, `Directory_`, `ComponentId`, `KeyPath`) VALUES ('Script', 'Alias', 'ScriptID', 'ScriptFile')");
      sql(L"INSERT INTO `Component` (`Component`, `Directory_`, `ComponentId`, `KeyPath`) VALUES ('External', 'WindowsFolder', 'ExternalID', 'ExternalFile')");
      sql(L"INSERT INTO `File` (`File`, `Component_`, `FileName`) VALUES ('MainFile', 'Main', 'SUNSHI~1.EXE|sunshine.exe')");
      sql(L"INSERT INTO `File` (`File`, `Component_`, `FileName`) VALUES ('ImageFile', 'Image', 'box.png')");
      sql(L"INSERT INTO `File` (`File`, `Component_`, `FileName`) VALUES ('ScriptFile', 'Script', 'HELPER~1.PS1|helper-script.ps1')");
      sql(L"INSERT INTO `File` (`File`, `Component_`, `FileName`) VALUES ('ExternalFile', 'External', 'outside.dll')");
      require(MsiDatabaseCommit(database.value)==ERROR_SUCCESS,"inventory fixture commit");
      const auto inventory=package_inventory(database.value);
      const std::set<std::wstring> expected{L"sunshine.exe",L"assets\\box.png",L"scripts\\helper-script.ps1"};
      require(std::set<std::wstring>(inventory.begin(),inventory.end())==expected,"cached MSI payload inventory and target/source names");
      fixture_properties[L"INSTALL_ROOT"]=L"C:\\Program Files\\Sunshine\\.";
      fixture_targets={{L"INSTALL_ROOT",L"C:\\Program Files\\Sunshine\\"},
        {L"Assets",L"C:\\Program Files\\Sunshine\\assets\\"},
        {L"Alias",L"C:\\Program Files\\Sunshine\\updated-scripts\\"}};
      const auto incoming=package_inventory(database.value,1);
      require(std::find(incoming.begin(),incoming.end(),L"updated-scripts\\helper-script.ps1")!=incoming.end(),"incoming inventory follows costed directory overrides");
      for(const auto setting:{L"1",L"0"}) {
        fixture_properties[L"SHORTFILENAMES"]=setting;
        const auto short_files=package_inventory(database.value,1);
        require(std::find(short_files.begin(),short_files.end(),L"sunshi~1.exe")!=short_files.end(),"short target member selected when property is set");
        require(std::find(short_files.begin(),short_files.end(),L"sunshine.exe")==short_files.end(),"unused long filename excluded");
        require(std::find(short_files.begin(),short_files.end(),L"updated-scripts\\helper~1.ps1")!=short_files.end(),"costed directories and short filenames combined");
      }
      fixture_properties.erase(L"SHORTFILENAMES");
      const std::map<std::wstring,std::wstring> installed{{L"MainID",L"C:\\Installed\\sunshi~1.exe"},{L"ImageID",L"C:\\Installed\\assets\\box.png"},
        {L"ScriptID",L"C:\\Installed\\relocated\\helper~1.ps1"}};
      const auto old_files=package_inventory(database.value,0,L"C:\\Installed",[&](const std::wstring &id){return installed.at(id);});
      const std::set<std::wstring> installed_expected{L"sunshi~1.exe",L"assets\\box.png",L"relocated\\helper~1.ps1"};
      require(std::set<std::wstring>(old_files.begin(),old_files.end())==installed_expected,"registered component paths preserve actual names and relocations");
      require(std::none_of(old_files.begin(),old_files.end(),[](const auto &file){return file.find(L"usbip")!=std::wstring::npos;}),"old inventory contains no incoming optional paths");
      sql(L"INSERT INTO `InstallExecuteSequence` (`Action`, `Condition`, `Sequence`) VALUES ('MigrateConfig', 'MIGRATING', 100)");
      sql(L"INSERT INTO `InstallExecuteSequence` (`Action`, `Condition`, `Sequence`) VALUES ('InstallUsbIpTransport', 'USB_ENABLED', 101)");
      sql(L"INSERT INTO `InstallExecuteSequence` (`Action`, `Condition`, `Sequence`) VALUES ('RunInstallerMigrations', 'MIGRATIONS', 102)");
      sql(L"INSERT INTO `InstallExecuteSequence` (`Action`, `Condition`, `Sequence`) VALUES ('UpdatePathAdd', 'PATH_ENABLED', 103)");
      fixture_conditions[L"MIGRATING"]=MSICONDITION_TRUE;
      auto selected=action_scope(database.value,1);
      require(std::find(selected.directories.begin(),selected.directories.end(),L"config\\credentials")!=selected.directories.end(),"scheduled migration protects runtime credentials directory");
      require(std::none_of(selected.files.begin(),selected.files.end(),[](const auto &file){return file.find(L"usbip")!=std::wstring::npos;}),"unselected action path is not protected");
      require(std::find(selected.files.begin(),selected.files.end(),L"scripts\\update-path.bat")==selected.files.end(),"unselected PATH action excluded");
      require(std::find(selected.files.begin(),selected.files.end(),L"scripts\\update-path.ps1")==selected.files.end(),"unselected PATH dependency excluded");
      const auto incoming_root=fixture_properties[L"INSTALL_ROOT"];
      fixture_properties[L"INSTALL_ROOT"]=path;
      const auto legacy_covers=std::wstring(path)+L"\\covers",destination_covers=config+L"\\covers";
      require(CreateDirectoryW(config.c_str(),nullptr)!=FALSE && CreateDirectoryW(destination_covers.c_str(),nullptr)!=FALSE,"unused covers fixture");
      fixture_untrusted_suffix=L"\\config\\covers";
      selected=action_scope(database.value,1);
      require(std::find(selected.directories.begin(),selected.directories.end(),L"config\\covers")==selected.directories.end(),"unused covers destination excluded");
      { filesystem_t fs{{},true};validate(fs,path,parent(path),false,selected.files,selected.directories); }
      // A regular file with the legacy directory's name is not migrated.
      { const auto file=CreateFileW(legacy_covers.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(file!=INVALID_HANDLE_VALUE,"non-directory covers fixture");CloseHandle(file); }
      selected=action_scope(database.value,1);
      require(std::find(selected.directories.begin(),selected.directories.end(),L"covers")==selected.directories.end(),"non-directory migration source excluded");
      require(DeleteFileW(legacy_covers.c_str())!=FALSE && CreateDirectoryW(legacy_covers.c_str(),nullptr)!=FALSE,"legacy covers directory fixture");
      selected=action_scope(database.value,1);
      require(std::find(selected.directories.begin(),selected.directories.end(),L"config\\covers")!=selected.directories.end(),"actual covers migration destination protected");
      try {
        filesystem_t fs{{},true};validate(fs,path,parent(path),false,selected.files,selected.directories);
        throw std::runtime_error("unsafe actual migration destination accepted");
      } catch(const failure_t &failure) { require(critical(failure) && failure.path==destination_covers,"migration destination rejection names actual input"); }
      fixture_untrusted_suffix.clear();fixture_properties[L"INSTALL_ROOT"]=incoming_root;
      require(RemoveDirectoryW(legacy_covers.c_str())!=FALSE && RemoveDirectoryW(destination_covers.c_str())!=FALSE && RemoveDirectoryW(config.c_str())!=FALSE,"covers fixtures cleanup");
      fixture_conditions[L"MIGRATING"]=MSICONDITION_FALSE;fixture_conditions[L"MIGRATIONS"]=MSICONDITION_TRUE;
      selected=action_scope(database.value,1);
      for(const auto file:{L"sunshine.conf",L"apps.json",L"sunshine_state.json",L"sunshine_state.json.bak",L"vibeshine_state.json"})
        require(std::find(selected.files.begin(),selected.files.end(),file)!=selected.files.end(),"PowerShell migration protects its root-level config and JSON candidates");
      fixture_conditions[L"MIGRATIONS"]=MSICONDITION_FALSE;
      fixture_conditions[L"PATH_ENABLED"]=MSICONDITION_TRUE;
      fixture_conditions[L"MIGRATING"]=MSICONDITION_FALSE;fixture_conditions[L"USB_ENABLED"]=MSICONDITION_TRUE;
      selected=action_scope(database.value,1);
      require(selected.directories.empty(),"unscheduled migration does not inspect data directories");
      require(std::find(selected.files.begin(),selected.files.end(),L"drivers\\usbip-win2\\package.ps1")!=selected.files.end(),"scheduled action includes dot-sourced dependency");
      require(std::find(selected.files.begin(),selected.files.end(),L"scripts\\update-path.bat")!=selected.files.end(),"scheduled Vibepollo PATH action protected");
      require(std::find(selected.files.begin(),selected.files.end(),L"scripts\\update-path.ps1")!=selected.files.end(),"scheduled Vibepollo PATH dependency protected");
      fixture_targets[L"Assets"]=L"C:\\outside\\";
      try { package_inventory(database.value,1);throw std::runtime_error("outside directory override accepted"); }
      catch(const failure_t &failure) { require(critical(failure) && failure.path==L"c:\\outside","outside target evidence"); }
      fixture_targets.clear();
      sql(L"DROP TABLE `File`");
      try { package_inventory(database.value);throw std::runtime_error("missing inventory not diagnosed"); }
      catch(const failure_t &failure) {
        require(!critical(failure) && std::wstring(failure.reason)==L"package_inventory" && failure.status!=0,"unavailable inventory is a diagnosed compatibility warning");
      }
    }
    require(DeleteFileW(package.c_str())!=FALSE,"inventory fixture cleanup");
    std::cout<<"Install guard: rejection evidence, log-only root findings, scoped payload validation, custom artwork, and MSI inventory parsing passed\n";
  } catch(...) { RemoveDirectoryW((std::wstring(path)+L"\\host").c_str());RemoveDirectoryW(path);throw; }
  require(RemoveDirectoryW(path)!=FALSE,"fixture cleanup");
}
