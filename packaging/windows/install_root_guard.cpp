#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <msiquery.h>
#include <aclapi.h>
#include <cwctype>
#include <vector>
#include "install_root_guard.h"
namespace {
  std::wstring property(MSIHANDLE session,const wchar_t *name) {
    DWORD size=0;wchar_t empty=0;
    const UINT result=MsiGetPropertyW(session,name,&empty,&size);
    if(result!=ERROR_MORE_DATA && result!=ERROR_SUCCESS)return {};
    std::wstring value(size+1,L'\0');++size;
    if(MsiGetPropertyW(session,name,value.data(),&size)!=ERROR_SUCCESS)return {};
    value.resize(size);return value;
  }
  void log_line(MSIHANDLE session,const std::wstring &message) {
    // Keep paths in a record field so brackets in a filename cannot be treated
    // as MSI formatting instructions.
    const MSIHANDLE record=MsiCreateRecord(1);
    if(!record)return;
    MsiRecordSetStringW(record,0,L"Install root guard: [1]");
    MsiRecordSetStringW(record,1,message.c_str());
    MsiProcessMessage(session,INSTALLMESSAGE_INFO,record);
    MsiCloseHandle(record);
  }
  void inconclusive(MSIHANDLE session,const std::wstring &path,const wchar_t *call,DWORD status) {
    log_line(session,L"INSTALL_ROOT_DIAGNOSTIC: "+std::wstring(call)+L" returned "+std::to_wstring(status)+L" for "+path);
  }
  bool everyone_on_this_pc(PSID sid) {
    if(!sid || !IsValidSid(sid))return false;
    for(const auto type : {WinWorldSid,WinAuthenticatedUserSid,WinBuiltinUsersSid,WinInteractiveSid})
      if(IsWellKnownSid(sid,type))return true;
    return false;
  }
  bool acl_exposes(PACL dacl) {
    if(!dacl)return true; // a null DACL grants everyone full control
    for(DWORD i=0;i<dacl->AceCount;++i) {
      void *raw=nullptr;
      if(!GetAce(dacl,i,&raw))break;
      if(static_cast<ACE_HEADER*>(raw)->AceType!=ACCESS_ALLOWED_ACE_TYPE)continue;
      const auto ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
      if(everyone_on_this_pc(&ace->SidStart) && install_root_guard::ace_exposes(ace->Mask))return true;
    }
    return false;
  }
  struct filesystem_t {
    MSIHANDLE session;
    // Follows links on purpose: what matters is the folder the files land in.
    install_root_guard::node_t inspect(const std::wstring &path) {
      install_root_guard::node_t node;
      const HANDLE file=CreateFileW(path.c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
      if(file==INVALID_HANDLE_VALUE) {
        const DWORD error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)return node;
        inconclusive(session,path,L"CreateFileW",error);
        node.exists=true;return node;
      }
      node.exists=true;
      BY_HANDLE_FILE_INFORMATION info{};
      if(GetFileInformationByHandle(file,&info))node.directory=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
      else inconclusive(session,path,L"GetFileInformationByHandle",GetLastError());
      PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
      const DWORD status=GetSecurityInfo(file,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&dacl,nullptr,&descriptor);
      if(status!=ERROR_SUCCESS)inconclusive(session,path,L"GetSecurityInfo",status);
      else {
        node.exposed=acl_exposes(dacl);
        LocalFree(descriptor);
      }
      CloseHandle(file);
      return node;
    }
  };
  struct pinned_entry_t {
    bool descend=false;
    HANDLE pin=INVALID_HANDLE_VALUE;
    pinned_entry_t()=default;
    pinned_entry_t(const pinned_entry_t &)=delete;
    pinned_entry_t(pinned_entry_t &&other) noexcept:descend(other.descend),pin(other.pin) { other.pin=INVALID_HANDLE_VALUE; }
    ~pinned_entry_t() { if(pin!=INVALID_HANDLE_VALUE)CloseHandle(pin); }
  };
  // Runs as SYSTEM over a tree other accounts may have been able to write, so it
  // works only through no-follow handles and never changes what a link points to.
  struct repairer_t {
    MSIHANDLE session;
    unsigned repaired=0;
    std::wstring first_failure;
    // Whether sid keeps full control here and, for a folder, on what is created inside it.
    bool keeps_full_control(PACL acl,PSID sid,bool directory) {
      if(!acl)return false;
      bool here=false,inside=!directory;
      for(DWORD i=0;i<acl->AceCount;++i) {
        void *raw=nullptr;
        if(!GetAce(acl,i,&raw))return false;
        const auto ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
        // Inherited grants don't count: reducing a broad explicit grant can expose an
        // inherited deny that Windows reaches first, which would lock this account out.
        if(ace->Header.AceType!=ACCESS_ALLOWED_ACE_TYPE || (ace->Header.AceFlags&INHERITED_ACE) || !EqualSid(&ace->SidStart,sid))continue;
        const bool full=(ace->Mask&FILE_ALL_ACCESS)==FILE_ALL_ACCESS;
        if(full && !(ace->Header.AceFlags&INHERIT_ONLY_ACE))here=true;
        if((full || (ace->Mask&GENERIC_ALL)) && (ace->Header.AceFlags&OBJECT_INHERIT_ACE) && (ace->Header.AceFlags&CONTAINER_INHERIT_ACE))inside=true;
      }
      return here && inside;
    }
    // Strip the write rights of the everyone-style groups and leave every other
    // entry alone, including anything granted to a person's own account.
    bool rewrite(HANDLE file,PACL old_acl,PSECURITY_DESCRIPTOR old_descriptor,bool directory,bool root) {
      std::vector<DWORD> buffer(((old_acl ? old_acl->AclSize : sizeof(ACL))+3*(sizeof(ACCESS_ALLOWED_ACE)+SECURITY_MAX_SID_SIZE))/sizeof(DWORD)+1);
      const auto new_acl=reinterpret_cast<PACL>(buffer.data());
      const DWORD revision=old_acl ? old_acl->AclRevision : ACL_REVISION;
      if(!InitializeAcl(new_acl,static_cast<DWORD>(buffer.size()*sizeof(DWORD)),revision))return false;
      // The service and administrators must keep working if everyone was all they had.
      const DWORD inherit=directory ? OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE : 0;
      const auto add_grants=[&]() {
        for(const auto type : {WinLocalSystemSid,WinBuiltinAdministratorsSid}) {
          BYTE sid[SECURITY_MAX_SID_SIZE];DWORD size=sizeof(sid);
          if(!CreateWellKnownSid(type,nullptr,sid,&size))return false;
          if(!keeps_full_control(old_acl,sid,directory) && !AddAccessAllowedAceEx(new_acl,revision,inherit,FILE_ALL_ACCESS,sid))return false;
        }
        return true;
      };
      // Windows stops at the first match, so explicit grants go after explicit
      // denies and ahead of everything inherited.
      bool granted=false;
      if(old_acl) for(DWORD i=0;i<old_acl->AceCount;++i) {
        void *raw=nullptr;
        if(!GetAce(old_acl,i,&raw))return false;
        const auto header=static_cast<ACE_HEADER*>(raw);
        if(!granted && !(header->AceType==ACCESS_DENIED_ACE_TYPE && !(header->AceFlags&INHERITED_ACE))) {
          granted=true;
          if(!add_grants())return false;
        }
        std::vector<BYTE> copy(static_cast<BYTE*>(raw),static_cast<BYTE*>(raw)+header->AceSize);
        if(header->AceType==ACCESS_ALLOWED_ACE_TYPE) {
          const auto ace=reinterpret_cast<ACCESS_ALLOWED_ACE*>(copy.data());
          if(everyone_on_this_pc(&ace->SidStart) && install_root_guard::ace_exposes(ace->Mask))
            ace->Mask=install_root_guard::read_only_mask(ace->Mask);
        }
        if(!AddAce(new_acl,revision,MAXDWORD,copy.data(),header->AceSize))return false;
      }
      if(!granted && !add_grants())return false;
      if(!old_acl) {
        BYTE sid[SECURITY_MAX_SID_SIZE];DWORD size=sizeof(sid);
        if(!CreateWellKnownSid(WinBuiltinUsersSid,nullptr,sid,&size))return false;
        if(!AddAccessAllowedAceEx(new_acl,revision,inherit,FILE_GENERIC_READ|FILE_GENERIC_EXECUTE,sid))return false;
      }
      SECURITY_DESCRIPTOR descriptor;
      if(!InitializeSecurityDescriptor(&descriptor,SECURITY_DESCRIPTOR_REVISION))return false;
      if(!SetSecurityDescriptorDacl(&descriptor,TRUE,new_acl,FALSE))return false;
      SECURITY_INFORMATION information=DACL_SECURITY_INFORMATION;
      SECURITY_DESCRIPTOR_CONTROL control=0;DWORD control_revision=0;
      GetSecurityDescriptorControl(old_descriptor,&control,&control_revision);
      // A weak drive must not hand its permissions back to the install folder later.
      if(root || (control&SE_DACL_PROTECTED)) {
        if(!SetSecurityDescriptorControl(&descriptor,SE_DACL_PROTECTED,SE_DACL_PROTECTED))return false;
        information|=PROTECTED_DACL_SECURITY_INFORMATION;
      }
      if(control&SE_DACL_AUTO_INHERITED)SetSecurityDescriptorControl(&descriptor,SE_DACL_AUTO_INHERITED,SE_DACL_AUTO_INHERITED);
      // Unlike SetSecurityInfo, this changes only this handle's object and never
      // propagates through the tree, where it could reach beyond a link.
      return SetKernelObjectSecurity(file,information,&descriptor)!=FALSE;
    }
    // A successful call is not proof: only what the file system kept counts. Being
    // unable to read it back is not evidence of exposure, so that alone never warns.
    bool still_safe(HANDLE file) {
      PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
      if(GetSecurityInfo(file,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&dacl,nullptr,&descriptor)!=ERROR_SUCCESS)return true;
      const bool safe=!acl_exposes(dacl);
      LocalFree(descriptor);
      return safe;
    }
    // Listing access is what makes Windows enforce the sharing mode here: without
    // FILE_SHARE_DELETE, nobody can rename or replace this folder while it is held.
    void pin_folder(const std::wstring &path,pinned_entry_t &entry) {
      const HANDLE pin=CreateFileW(path.c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
      if(pin==INVALID_HANDLE_VALUE) {
        inconclusive(session,path,L"CreateFileW",GetLastError());
        return;
      }
      entry.pin=pin;
      BY_HANDLE_FILE_INFORMATION info{};
      entry.descend=GetFileInformationByHandle(pin,&info) && (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT);
    }
    pinned_entry_t repair(const std::wstring &path,bool root) {
      pinned_entry_t entry;
      const HANDLE file=CreateFileW(path.c_str(),READ_CONTROL|WRITE_DAC|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
      if(file==INVALID_HANDLE_VALUE) {
        const DWORD error=GetLastError();
        if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)inconclusive(session,path,L"CreateFileW",error);
        return entry;
      }
      BY_HANDLE_FILE_INFORMATION info{};
      if(!GetFileInformationByHandle(file,&info)) {
        inconclusive(session,path,L"GetFileInformationByHandle",GetLastError());
        CloseHandle(file);return entry;
      }
      const bool directory=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
      // A link's target and a hardlink's other names may live outside this tree.
      if((info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) || (!directory && info.nNumberOfLinks>1)) {
        log_line(session,L"INSTALL_ROOT_DIAGNOSTIC: left link unchanged: "+path);
        CloseHandle(file);return entry;
      }
      if(directory)pin_folder(path,entry);
      PACL dacl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
      const DWORD status=GetSecurityInfo(file,SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&dacl,nullptr,&descriptor);
      if(status!=ERROR_SUCCESS) {
        inconclusive(session,path,L"GetSecurityInfo",status);
        CloseHandle(file);return entry;
      }
      if(acl_exposes(dacl)) {
        if(rewrite(file,dacl,descriptor,directory,root) && still_safe(file))++repaired;
        else {
          inconclusive(session,path,L"repair",GetLastError());
          if(first_failure.empty())first_failure=path;
        }
      }
      LocalFree(descriptor);
      CloseHandle(file);
      return entry;
    }
    std::vector<std::wstring> children(const std::wstring &path) {
      std::vector<std::wstring> result;WIN32_FIND_DATAW data{};
      const HANDLE search=FindFirstFileW((path+L"\\*").c_str(),&data);
      if(search==INVALID_HANDLE_VALUE)return result;
      do{const std::wstring name=data.cFileName;if(name!=L"."&&name!=L"..")result.push_back(path+L"\\"+name);}while(FindNextFileW(search,&data));
      FindClose(search);
      return result;
    }
  };
  std::wstring normalized(const std::wstring &path) {
    std::wstring full(32768,L'\0');
    const DWORD length=GetFullPathNameW(path.c_str(),static_cast<DWORD>(full.size()),full.data(),nullptr);
    if(!length || length>=full.size())return path;
    full.resize(length);return full;
  }
  std::wstring lower(std::wstring s){for(auto &ch:s)ch=static_cast<wchar_t>(std::towlower(ch));return s;}
  UINT validate_root(MSIHANDLE session) {
    using install_root_guard::outcome_t;
    try {
      MsiSetPropertyW(session,L"VIBESHINE_INSTALL_ROOT_ERROR",L"");
      MsiSetPropertyW(session,L"VIBESHINE_INSTALL_ROOT_WARNINGS",L"");
      // Nothing here may ever stop someone removing the product.
      if(lower(property(session,L"REMOVE"))==L"all")return ERROR_SUCCESS;
      const auto root=property(session,L"INSTALL_ROOT");
      if(root.empty())return ERROR_SUCCESS;
      filesystem_t fs{session};
      const auto verdict=install_root_guard::check(fs,normalized(root));
      if(verdict.outcome==outcome_t::ok)return ERROR_SUCCESS;
      const auto error=verdict.outcome==outcome_t::unusable
        ? verdict.path+L" cannot be used as the install folder. Choose a folder on a local drive whose name has no % or ; character."
        : verdict.path+L" exists but is not a folder. Remove it or choose a different install folder.";
      log_line(session,L"INSTALL_ROOT_ERROR: "+error);
      MsiSetPropertyW(session,L"VIBESHINE_INSTALL_ROOT_ERROR",error.c_str());
      const MSIHANDLE record=MsiCreateRecord(1);
      if(record){MsiRecordSetStringW(record,0,L"Installation stopped. [1]");MsiRecordSetStringW(record,1,error.c_str());MsiProcessMessage(session,INSTALLMESSAGE_ERROR,record);MsiCloseHandle(record);}
      return ERROR_INSTALL_FAILURE;
    } catch(...) {
      // A defect in the guard must never stop an install.
      return ERROR_SUCCESS;
    }
  }
}
extern "C" __declspec(dllexport) UINT __stdcall ValidateInstallRootPreview(MSIHANDLE session) {
  return validate_root(session);
}
extern "C" __declspec(dllexport) UINT __stdcall ValidateInstallRoot(MSIHANDLE session) {
  return validate_root(session);
}
// Deferred and SYSTEM, right after the files land and before any helper runs from them.
extern "C" __declspec(dllexport) UINT __stdcall RepairInstallRoot(MSIHANDLE session) {
  try {
    const auto root=install_root_guard::without_trailing_separator(normalized(property(session,L"CustomActionData")));
    if(!install_root_guard::usable_path(root))return ERROR_SUCCESS;
    repairer_t repairer{session};
    install_root_guard::repair_tree(repairer,root);
    log_line(session,L"INSTALL_ROOT_DIAGNOSTIC: repaired "+std::to_wstring(repairer.repaired)+L" under "+root);
    // Inspecting the root also covers an install folder that is a link to somewhere exposed.
    filesystem_t fs{session};
    const auto &exposed=!repairer.first_failure.empty() ? repairer.first_failure : fs.inspect(root).exposed ? root : std::wstring();
    if(!exposed.empty())
      log_line(session,L"INSTALL_ROOT_WARNING: Anyone with an account on this PC can modify "+exposed+L", and the installer could not change that. The service runs with full system privileges from there, so any of those accounts could take control of this PC.");
  } catch(...) {}
  return ERROR_SUCCESS;
}
