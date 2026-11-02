#pragma once
// In-memory stand-in for the Win32 surface install_root_guard.cpp uses, so the real
// translation unit runs on any host. CreateFileW honours FILE_FLAG_OPEN_REPARSE_POINT:
// without it a link resolves to its target, which is what makes no-follow testable.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <map>
#include <string>
#include <vector>
#undef __stdcall
#undef __declspec
#define __stdcall
#define __declspec(x)
typedef unsigned int DWORD,UINT,*LPDWORD; typedef int BOOL; typedef void*HANDLE,*PSID,*PSECURITY_DESCRIPTOR;
typedef unsigned long MSIHANDLE; typedef const wchar_t*LPCWSTR; typedef wchar_t*LPWSTR; typedef unsigned char BYTE;
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
enum { ERROR_SUCCESS=0,ERROR_FILE_NOT_FOUND=2,ERROR_PATH_NOT_FOUND=3,ERROR_ACCESS_DENIED=5,ERROR_NO_MORE_FILES=18,ERROR_MORE_DATA=234,ERROR_INSTALL_FAILURE=1603 };
enum : DWORD { FILE_LIST_DIRECTORY=1,WRITE_DAC=0x40000,READ_CONTROL=0x20000,FILE_READ_ATTRIBUTES=0x80,FILE_SHARE_READ=1,FILE_SHARE_WRITE=2,FILE_SHARE_DELETE=4,OPEN_EXISTING=3,
 FILE_FLAG_OPEN_REPARSE_POINT=0x200000,FILE_FLAG_BACKUP_SEMANTICS=0x2000000,FILE_ATTRIBUTE_DIRECTORY=0x10,FILE_ATTRIBUTE_REPARSE_POINT=0x400,
 DACL_SECURITY_INFORMATION=4,PROTECTED_DACL_SECURITY_INFORMATION=0x80000000,GENERIC_ALL=0x10000000,INHERITED_ACE=0x10,FILE_ALL_ACCESS=0x1f01ff,FILE_GENERIC_READ=0x120089,FILE_GENERIC_EXECUTE=0x1200a0,
 ACL_REVISION=2,OBJECT_INHERIT_ACE=1,CONTAINER_INHERIT_ACE=2,MAXDWORD=0xffffffff,SECURITY_DESCRIPTOR_REVISION=1,SECURITY_MAX_SID_SIZE=68 };
enum { SE_FILE_OBJECT=1,ACCESS_ALLOWED_ACE_TYPE=0,ACCESS_DENIED_ACE_TYPE=1,INHERIT_ONLY_ACE=8 };
enum { INSTALLMESSAGE_ERROR=0x01000000,INSTALLMESSAGE_INFO=0x04000000 };
enum WELL_KNOWN_SID_TYPE { WinWorldSid,WinAuthenticatedUserSid,WinBuiltinUsersSid,WinInteractiveSid,WinLocalSystemSid,WinBuiltinAdministratorsSid };
typedef unsigned short SECURITY_DESCRIPTOR_CONTROL; enum : unsigned short { SE_DACL_AUTO_INHERITED=0x400,SE_DACL_PROTECTED=0x1000 }; typedef DWORD SECURITY_INFORMATION;
struct ACE_HEADER{BYTE AceType,AceFlags;unsigned short AceSize;};
struct ACCESS_ALLOWED_ACE{ACE_HEADER Header;DWORD Mask;DWORD SidStart;};
struct ACL{DWORD AceCount;std::vector<ACCESS_ALLOWED_ACE>*aces;DWORD AclSize=64;DWORD AclRevision=2;}; typedef ACL*PACL;
struct SECURITY_DESCRIPTOR{PACL dacl=nullptr;SECURITY_DESCRIPTOR_CONTROL control=0;};
struct BY_HANDLE_FILE_INFORMATION{DWORD dwFileAttributes,nNumberOfLinks;};
struct WIN32_FIND_DATAW{wchar_t cFileName[260];};
// A SID's "kind" doubles as its payload.
enum : DWORD { SID_SYSTEM=1,SID_ADMINS=2,SID_INSTALLER=3,SID_CREATOR=4,SID_USER=10,SID_USERS=11,SID_AUTH=12,SID_CAPABILITY=13,SID_EVERYONE=15 };
struct Fake{bool dir=true,reparse=false,denied=false,noacl=false,null_dacl=false,set_fails=false,no_write_dac=false,no_pin=false,becomes_link=false,write_ignored=false,read_fails_after_write=false,protected_dacl=false;DWORD links=1;int writes=0;int pins_at_write=-1;std::wstring target;DWORD owner=SID_INSTALLER;std::vector<ACCESS_ALLOWED_ACE> aces;ACL acl{};};
struct World{std::map<std::wstring,Fake> nodes;std::map<std::wstring,std::wstring> props;std::vector<std::wstring> log,shown;int open_pins=0;};
inline World*world=nullptr; inline DWORD last_error=0;
inline std::wstring fold(std::wstring s){for(auto&c:s)c=(wchar_t)towlower(c);return s;}
inline DWORD GetLastError(){return last_error;}
struct FakeHandle{Fake*node;bool pin;};
inline Fake*node_of(HANDLE h){return ((FakeHandle*)h)->node;}
inline BOOL CloseHandle(HANDLE h){auto*f=(FakeHandle*)h;if(f->pin)world->open_pins--;delete f;return TRUE;} inline void*LocalFree(void*){return nullptr;}
inline HANDLE CreateFileW(LPCWSTR p,DWORD access,DWORD,void*,DWORD,DWORD flags,void*){
 auto it=world->nodes.find(fold(p));
 if(it!=world->nodes.end()&&it->second.reparse&&!(flags&FILE_FLAG_OPEN_REPARSE_POINT))it=world->nodes.find(fold(it->second.target));
 if(it==world->nodes.end()){last_error=ERROR_FILE_NOT_FOUND;return INVALID_HANDLE_VALUE;}
 if(it->second.denied||((access&WRITE_DAC)&&it->second.no_write_dac)){last_error=ERROR_ACCESS_DENIED;return INVALID_HANDLE_VALUE;}
 const bool pin=(access&FILE_LIST_DIRECTORY)!=0;
 if(pin&&it->second.no_pin){last_error=32;return INVALID_HANDLE_VALUE;}
 if(pin)world->open_pins++;
 return new FakeHandle{&it->second,pin};}
inline BOOL GetFileInformationByHandle(HANDLE h,BY_HANDLE_FILE_INFORMATION*i){auto*n=node_of(h);const bool swapped=n->becomes_link&&((FakeHandle*)h)->pin;i->dwFileAttributes=(n->dir?(DWORD)FILE_ATTRIBUTE_DIRECTORY:0u)|(n->reparse||swapped?(DWORD)FILE_ATTRIBUTE_REPARSE_POINT:0u);i->nNumberOfLinks=n->links;return TRUE;}
inline DWORD GetSecurityInfo(HANDLE h,int,DWORD,PSID*,PSID*,PACL*d,PACL*,PSECURITY_DESCRIPTOR*s){auto*n=node_of(h);if(n->noacl||(n->read_fails_after_write&&n->writes>0))return ERROR_ACCESS_DENIED;*s=n;if(n->null_dacl){*d=nullptr;return 0;}n->acl.AceCount=(DWORD)n->aces.size();n->acl.aces=&n->aces;*d=&n->acl;return 0;}
inline BOOL GetAce(PACL a,DWORD i,void**r){*r=&(*a->aces)[i];return TRUE;}
inline BOOL IsValidSid(PSID s){return s!=nullptr;}
inline BOOL IsWellKnownSid(PSID s,WELL_KNOWN_SID_TYPE t){DWORD k=*(DWORD*)s;return (t==WinAuthenticatedUserSid&&k==SID_AUTH)||(t==WinBuiltinUsersSid&&k==SID_USERS)||(t==WinWorldSid&&k==SID_EVERYONE);}
inline BOOL EqualSid(PSID a,PSID b){return *(DWORD*)a==*(DWORD*)b;}
inline BOOL CreateWellKnownSid(WELL_KNOWN_SID_TYPE t,void*,void*b,DWORD*){*(DWORD*)b=t==WinLocalSystemSid?SID_SYSTEM:t==WinBuiltinAdministratorsSid?SID_ADMINS:SID_USERS;return TRUE;}
inline BOOL InitializeAcl(PACL a,DWORD,DWORD){a->AceCount=0;a->aces=new std::vector<ACCESS_ALLOWED_ACE>;return TRUE;}
inline BOOL AddAce(PACL a,DWORD,DWORD,void*ace,DWORD){a->aces->push_back(*(ACCESS_ALLOWED_ACE*)ace);a->AceCount++;return TRUE;}
inline BOOL AddAccessAllowedAceEx(PACL a,DWORD,DWORD flags,DWORD mask,void*sid){a->aces->push_back({{ACCESS_ALLOWED_ACE_TYPE,(BYTE)flags,(unsigned short)sizeof(ACCESS_ALLOWED_ACE)},mask,*(DWORD*)sid});a->AceCount++;return TRUE;}
inline BOOL InitializeSecurityDescriptor(SECURITY_DESCRIPTOR*d,DWORD){*d={};return TRUE;}
inline BOOL SetSecurityDescriptorDacl(SECURITY_DESCRIPTOR*d,BOOL,PACL a,BOOL){d->dacl=a;return TRUE;}
inline BOOL SetSecurityDescriptorControl(SECURITY_DESCRIPTOR*d,SECURITY_DESCRIPTOR_CONTROL,SECURITY_DESCRIPTOR_CONTROL v){d->control|=v;return TRUE;}
inline BOOL GetSecurityDescriptorControl(void*d,SECURITY_DESCRIPTOR_CONTROL*c,DWORD*){auto*n=(Fake*)d;*c=(n->protected_dacl?SE_DACL_PROTECTED:0)|SE_DACL_AUTO_INHERITED;return TRUE;}
inline BOOL SetKernelObjectSecurity(HANDLE h,SECURITY_INFORMATION,SECURITY_DESCRIPTOR*d){auto*n=node_of(h);if(n->set_fails){last_error=ERROR_ACCESS_DENIED;return FALSE;}n->pins_at_write=world->open_pins;if(n->write_ignored)return TRUE;n->aces=*d->dacl->aces;n->null_dacl=false;n->protected_dacl=(d->control&SE_DACL_PROTECTED)!=0;n->writes++;return TRUE;}
struct FindState{std::vector<std::wstring> names;size_t next=0;};
inline BOOL FindNextFileW(HANDLE h,WIN32_FIND_DATAW*d){auto*f=(FindState*)h;if(f->next>=f->names.size()){last_error=ERROR_NO_MORE_FILES;return FALSE;}std::wcsncpy(d->cFileName,f->names[f->next++].c_str(),259);return TRUE;}
inline HANDLE FindFirstFileW(LPCWSTR q,WIN32_FIND_DATAW*d){std::wstring dir=fold(q);dir.resize(dir.size()-2);auto*f=new FindState;f->names={L".",L".."};for(auto&entry:world->nodes){const auto&p=entry.first;if(p.size()>dir.size()+1&&p.compare(0,dir.size(),dir)==0&&p[dir.size()]==L'\\'&&p.find(L'\\',dir.size()+1)==std::wstring::npos)f->names.push_back(p.substr(dir.size()+1));}FindNextFileW(f,d);return f;}
inline BOOL FindClose(HANDLE h){delete (FindState*)h;return TRUE;}
inline DWORD GetFullPathNameW(LPCWSTR p,DWORD n,LPWSTR b,void*){std::wcsncpy(b,p,n);return (DWORD)std::wcslen(p);}
