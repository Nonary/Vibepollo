// Runs the real guard exports against the in-memory filesystem in install_root_guard_fake.
// ACLs are copied from field reports. Repair changes permissions as SYSTEM, so its limits
// (no links, no hardlinks, no lockouts, nothing healthy touched) are pinned here.
#include "packaging/windows/install_root_guard.cpp"
#include <iostream>
static int failures=0;
static void check(const char *name,bool ok){std::cout<<(ok?"ok    ":"FAIL  ")<<name<<"\n";failures+=!ok;}
static ACCESS_ALLOWED_ACE ace(DWORD sid,DWORD mask,BYTE flags=0){return {{ACCESS_ALLOWED_ACE_TYPE,flags,(unsigned short)sizeof(ACCESS_ALLOWED_ACE)},mask,sid};}
static Fake default_folder(bool dir=true){Fake f;f.dir=dir;f.owner=SID_ADMINS;f.aces={ace(SID_INSTALLER,0x1f01ff),ace(SID_SYSTEM,0x1f01ff),ace(SID_ADMINS,0x1f01ff),ace(SID_USERS,0x1200a9),ace(SID_USERS,0xa0000000,INHERIT_ONLY_ACE),ace(SID_CREATOR,0x10000000,INHERIT_ONLY_ACE),ace(SID_CAPABILITY,0x1200a9)};return f;}
static const std::wstring ROOT=L"C:\\Program Files\\Sunshine";
static const std::wstring R=fold(ROOT);
static World base(){World w;Fake c=default_folder();c.owner=SID_SYSTEM;c.aces.push_back(ace(SID_AUTH,0xe0010000,INHERIT_ONLY_ACE));c.aces.push_back(ace(SID_AUTH,0x1301bf));w.nodes[L"c:\\"]=c;w.nodes[L"c:\\program files"]=default_folder();w.nodes[R]=default_folder();w.nodes[R+L"\\assets"]=default_folder();w.props[L"INSTALL_ROOT"]=ROOT;return w;}
static bool validate(World &w,UINT rc,const wchar_t *needle=nullptr){world=&w;const UINT a=ValidateInstallRootPreview(1),b=ValidateInstallRoot(1);const auto &e=w.props[L"VIBESHINE_INSTALL_ROOT_ERROR"];
  if(a!=rc||b!=rc||(rc==0)!=e.empty()||!w.props[L"VIBESHINE_INSTALL_ROOT_WARNINGS"].empty())return false;
  if(rc&&(w.shown.empty()||w.shown[0].find(e)==std::wstring::npos))return false;
  return !needle||e.find(needle)!=std::wstring::npos;}
static UINT repair(World &w){world=&w;w.props[L"CustomActionData"]=w.props[L"INSTALL_ROOT"]+L"\\";w.log.clear();return RepairInstallRoot(1);}
static bool warned(const World &w){for(const auto &l:w.log)if(l.find(L"INSTALL_ROOT_WARNING")!=std::wstring::npos)return true;return false;}
static bool can_write(const Fake &f,DWORD sid){for(const auto &e:f.aces)if(e.SidStart==sid&&install_root_guard::ace_exposes(e.Mask))return true;return false;}
static bool full_control(const Fake &f,DWORD sid){for(const auto &e:f.aces)if(e.SidStart==sid&&(e.Mask&0x1f01ff)==0x1f01ff&&!(e.Header.AceFlags&INHERIT_ONLY_ACE))return true;return false;}
int main(){
  { World w=base();w.nodes.erase(R);w.nodes.erase(R+L"\\assets");check("validate: fresh default install",validate(w,0)); }
  { World w=base();check("validate: upgrade in place",validate(w,0)); }
  { World w=base();Fake &c=w.nodes[L"c:\\"];c.owner=SID_USER;c.aces.push_back(ace(SID_USERS,0x1f01ff));c.aces.push_back(ace(SID_CAPABILITY,0x1f01ff));check("validate: field C:\\ ACLs are ignored",validate(w,0)); }
  { World w=base();Fake &r=w.nodes[R];r.owner=SID_USER;r.aces.push_back(ace(SID_USERS,0x1f01ff));check("validate: weak folder is repaired, not reported",validate(w,0)); }
  { World w=base();w.nodes[R].denied=true;check("validate: unopenable folder does not fail",validate(w,0)); }
  { World w=base();w.nodes[R].noacl=true;check("validate: unreadable permissions do not fail",validate(w,0)); }
  { World w=base();w.props[L"INSTALL_ROOT"]=L"D:\\Games\\Sunshine";check("validate: another drive is allowed",validate(w,0)); }
  { World w=base();w.props[L"INSTALL_ROOT"]=L"C:\\Program Files\\R&D's $tuff";check("validate: characters Windows allows do not stop or warn",validate(w,0)); }
  { World w=base();w.props[L"INSTALL_ROOT"]=L"C:\\Program Files\\100%";check("validate: a percent sign stops, because it expands to another directory",validate(w,ERROR_INSTALL_FAILURE,L"% or ;")); }
  { World w=base();w.props[L"INSTALL_ROOT"]=L"C:\\Program Files\\a;b";check("validate: a semicolon stops, because it splits the machine PATH",validate(w,ERROR_INSTALL_FAILURE,L"% or ;")); }
  { World w=base();w.props[L"INSTALL_ROOT"]=L"C:\\Program Files\\a|b";check("validate: a name Windows forbids stops",validate(w,ERROR_INSTALL_FAILURE,L"local drive")); }
  { World w=base();w.props[L"INSTALL_ROOT"]=L"\\\\server\\share\\host";check("validate: network path fails",validate(w,ERROR_INSTALL_FAILURE)); }
  { World w=base();w.nodes[R]=default_folder(false);check("validate: a file where the folder belongs fails",validate(w,ERROR_INSTALL_FAILURE,L"not a folder")); }
  { World w=base();w.props[L"REMOVE"]=L"ALL";w.props[L"INSTALL_ROOT"]=L"C:\\a|b";check("validate: uninstall is never stopped",validate(w,0)); }
  { World w=base();w.props[L"INSTALL_ROOT"]=L"";check("validate: no resolved folder does not fail",validate(w,0)); }

  { World w=base();const auto before=w.nodes[R].aces.size();const UINT rc=repair(w);check("repair: a healthy install is not written at all",rc==0&&w.nodes[R].writes==0&&w.nodes[R+L"\\assets"].writes==0&&w.nodes[R].aces.size()==before&&!warned(w)); }
  { World w=base();w.nodes[R].aces.push_back(ace(SID_AUTH,0x1301bf));w.nodes[R].aces.push_back(ace(SID_AUTH,0xe0010000,INHERIT_ONLY_ACE));
    Fake t=default_folder();t.aces.push_back(ace(SID_AUTH,0x1301bf));w.nodes[R+L"\\tools"]=t;Fake e=default_folder(false);e.aces.push_back(ace(SID_AUTH,0x1301bf));w.nodes[R+L"\\tools\\a.exe"]=e;
    repair(w);const auto &root=w.nodes[R];
    check("repair: weak folder, subfolder and file are all fixed",!can_write(root,SID_AUTH)&&!can_write(w.nodes[R+L"\\tools"],SID_AUTH)&&!can_write(w.nodes[R+L"\\tools\\a.exe"],SID_AUTH)&&!warned(w));
    check("repair: only the top folder stops inheriting",root.protected_dacl&&!w.nodes[R+L"\\tools"].protected_dacl);
    bool read=false,inherit=false;for(const auto &a:root.aces){read|=a.SidStart==SID_AUTH&&a.Mask==0x1200a9;inherit|=a.SidStart==SID_AUTH&&a.Mask==0xa0000000&&(a.Header.AceFlags&INHERIT_ONLY_ACE);}
    check("repair: those accounts keep read access",read&&inherit); }
  { World w=base();Fake &p=w.nodes[R+L"\\assets\\windows.png"];p=default_folder(false);p.owner=SID_USER;p.aces.push_back(ace(SID_USER,0x1f01ff));Fake &q=w.nodes[R+L"\\assets\\web.js"];q=default_folder(false);q.aces.push_back(ace(SID_USERS,0x1301bf));repair(w);
    check("repair: walker reaches assets",q.writes==1&&!can_write(q,SID_USERS));
    check("repair: a person's own file and access are left alone",p.writes==0&&can_write(p,SID_USER)&&p.owner==SID_USER); }
  { World w=base();w.nodes[R].aces.push_back(ace(SID_USER,0x1f01ff));w.nodes[R].aces.push_back(ace(SID_USERS,0x1301bf));repair(w);check("repair: a personal grant survives when Users is stripped",can_write(w.nodes[R],SID_USER)&&!can_write(w.nodes[R],SID_USERS)); }
  { World w=base();Fake &sys=w.nodes[L"c:\\windows\\system32"];sys=default_folder();sys.aces.push_back(ace(SID_USERS,0x1301bf));Fake &dll=w.nodes[L"c:\\windows\\system32\\kernel32.dll"];dll=default_folder(false);dll.aces.push_back(ace(SID_USERS,0x1301bf));
    Fake &l=w.nodes[R+L"\\link"];l=default_folder();l.reparse=true;l.target=L"c:\\windows\\system32";l.aces.push_back(ace(SID_USERS,0x1301bf));Fake &beyond=w.nodes[R+L"\\link\\kernel32.dll"];beyond=dll;
    repair(w);check("repair: a junction and everything behind it are never touched",l.writes==0&&sys.writes==0&&dll.writes==0&&beyond.writes==0); }
  { World w=base();Fake &h=w.nodes[R+L"\\assets\\hard.dll"];h=default_folder(false);h.links=2;h.aces.push_back(ace(SID_USERS,0x1301bf));repair(w);check("repair: a hardlinked file is never touched",h.writes==0); }
  { World w=base();Fake &real=w.nodes[L"d:\\real"];real=default_folder();real.aces.push_back(ace(SID_AUTH,0x1301bf));Fake &l=w.nodes[R];l=default_folder();l.reparse=true;l.target=L"d:\\real";repair(w);check("repair: a linked install folder is unchanged and its exposure is reported",real.writes==0&&l.writes==0&&warned(w)); }
  { World w=base();Fake &r=w.nodes[R];r=Fake{};r.aces={ace(SID_EVERYONE,0x1f01ff)};repair(w);check("repair: an Everyone-only folder keeps SYSTEM and Administrators working",full_control(r,SID_SYSTEM)&&full_control(r,SID_ADMINS)&&!can_write(r,SID_EVERYONE)&&!warned(w)); }
  { World w=base();Fake &r=w.nodes[R];r=Fake{};r.null_dacl=true;repair(w);bool users=false;for(const auto &a:r.aces)users|=a.SidStart==SID_USERS&&!install_root_guard::ace_exposes(a.Mask);check("repair: a null DACL becomes SYSTEM, Administrators and read-only Users",!r.null_dacl&&full_control(r,SID_SYSTEM)&&full_control(r,SID_ADMINS)&&users); }
  { World w=base();w.nodes[R].aces.push_back(ace(SID_USERS,0x1301bf));w.nodes[R].set_fails=true;const UINT rc=repair(w);check("repair: a failed write warns and still succeeds",rc==0&&warned(w)); }
  { World w=base();w.nodes[R].aces.push_back(ace(SID_USERS,0x1301bf));w.nodes[R].no_write_dac=true;const UINT rc=repair(w);check("repair: an unopenable folder warns and still succeeds",rc==0&&warned(w)); }
  { World w=base();Fake &locked=w.nodes[R+L"\\locked"];locked=default_folder();locked.no_write_dac=true;Fake &in=w.nodes[R+L"\\locked\\x.dll"];in=default_folder(false);in.aces.push_back(ace(SID_USERS,0x1301bf));repair(w);check("repair: nothing beneath an unopenable folder is changed",in.writes==0&&!warned(w)); }
  { World w=base();w.nodes[R].aces.push_back(ace(SID_USERS,0x1301bf));w.props[L"INSTALL_ROOT"]=L"C:\\a|b";const UINT rc=repair(w);check("repair: an unusable path is a no-op",rc==0&&w.nodes[R].writes==0); }
  { World w=base();Fake &d=w.nodes[L"d:\\"];d=default_folder();d.aces.push_back(ace(SID_AUTH,0xe0010000,INHERIT_ONLY_ACE));Fake &r=w.nodes[L"d:\\sunshine"];r=default_folder();r.aces.push_back(ace(SID_AUTH,0x1301bf));r.aces.push_back(ace(SID_AUTH,0xe0010000,INHERIT_ONLY_ACE));w.props[L"INSTALL_ROOT"]=L"D:\\Sunshine";repair(w);check("repair: a new D:\\ install is locked down and D:\\ itself is untouched",!can_write(r,SID_AUTH)&&r.protected_dacl&&d.writes==0&&!warned(w)); }
  { World w=base();Fake &r=w.nodes[R];r=Fake{};r.aces={ace(SID_SYSTEM,0x1f01ff),ace(SID_ADMINS,0x1f01ff),ace(SID_EVERYONE,0x1f01ff,OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE)};repair(w);
    bool system_inside=false,admins_inside=false;for(const auto &a:r.aces){const bool flows=(a.Header.AceFlags&OBJECT_INHERIT_ACE)&&(a.Header.AceFlags&CONTAINER_INHERIT_ACE)&&(a.Mask&0x1f01ff)==0x1f01ff;system_inside|=flows&&a.SidStart==SID_SYSTEM;admins_inside|=flows&&a.SidStart==SID_ADMINS;}
    check("repair: SYSTEM and Administrators also keep access to files created later",system_inside&&admins_inside); }
  { World w=base();Fake &r=w.nodes[R];r=Fake{};ACCESS_ALLOWED_ACE deny=ace(SID_USER,0x1f01ff);deny.Header.AceType=ACCESS_DENIED_ACE_TYPE;ACCESS_ALLOWED_ACE inherited_deny=deny;inherited_deny.Header.AceFlags=INHERITED_ACE;r.aces={deny,ace(SID_EVERYONE,0x1f01ff),inherited_deny,ace(SID_USERS,0x1301bf,INHERITED_ACE)};repair(w);
    size_t first_grant=r.aces.size(),inherited=r.aces.size(),explicit_deny=r.aces.size();for(size_t i=r.aces.size();i-->0;){const auto &a=r.aces[i];if(a.SidStart==SID_SYSTEM)first_grant=i;if(a.Header.AceFlags&INHERITED_ACE)inherited=i;if(a.Header.AceType==ACCESS_DENIED_ACE_TYPE&&!(a.Header.AceFlags&INHERITED_ACE))explicit_deny=i;}
    check("repair: added grants sit after explicit denies and before inherited entries",explicit_deny<first_grant&&first_grant<inherited);
    size_t denies=0;for(const auto &a:r.aces)denies+=a.Header.AceType==ACCESS_DENIED_ACE_TYPE;check("repair: deny entries are kept",denies==2); }
  { World w=base();Fake &c=w.nodes[R+L"\\assets\\own"];c=default_folder();c.protected_dacl=true;c.aces.push_back(ace(SID_USERS,0x1301bf));Fake &o=w.nodes[R+L"\\assets\\open"];o=default_folder();o.aces.push_back(ace(SID_USERS,0x1301bf));repair(w);check("repair: a folder that was already protected stays protected, others do not change",c.writes==1&&c.protected_dacl&&o.writes==1&&!o.protected_dacl); }
  { World w=base();Fake &d=w.nodes[R+L"\\assets\\stuck.dll"];d=default_folder(false);d.aces.push_back(ace(SID_USERS,0x1301bf));d.set_fails=true;const UINT rc=repair(w);bool names_file=false;for(const auto &l:w.log)names_file|=l.find(L"INSTALL_ROOT_WARNING")!=std::wstring::npos&&l.find(L"stuck.dll")!=std::wstring::npos;check("repair: a file that could not be fixed under a healthy folder is reported",rc==0&&names_file); }
  { World w=base();Fake &t=w.nodes[R+L"\\tools"];t=default_folder();t.aces.push_back(ace(SID_USERS,0x1301bf));Fake &a=w.nodes[R+L"\\tools\\a.exe"];a=default_folder(false);a.aces.push_back(ace(SID_USERS,0x1301bf));repair(w);
    check("repair: every parent folder is held open while its children change",t.pins_at_write==2&&a.pins_at_write==2);
    check("repair: nothing stays held afterwards",w.open_pins==0); }
  { World w=base();Fake &locked=w.nodes[R+L"\\busy"];locked=default_folder();locked.no_pin=true;locked.aces.push_back(ace(SID_USERS,0x1301bf));Fake &in=w.nodes[R+L"\\busy\\x.dll"];in=default_folder(false);in.aces.push_back(ace(SID_USERS,0x1301bf));const UINT rc=repair(w);check("repair: a folder that cannot be held is fixed but not entered",rc==0&&locked.writes==1&&in.writes==0&&w.open_pins==0); }
  { World w=base();Fake &swap=w.nodes[R+L"\\swap"];swap=default_folder();swap.becomes_link=true;Fake &in=w.nodes[R+L"\\swap\\x.dll"];in=default_folder(false);in.aces.push_back(ace(SID_USERS,0x1301bf));repair(w);check("repair: a folder swapped for a link before it is held is not entered",in.writes==0&&w.open_pins==0); }
  { World w=base();Fake &d=w.nodes[R+L"\\assets\\kept.dll"];d=default_folder(false);d.aces.push_back(ace(SID_USERS,0x1301bf));d.write_ignored=true;const UINT rc=repair(w);bool names_file=false;for(const auto &l:w.log)names_file|=l.find(L"INSTALL_ROOT_WARNING")!=std::wstring::npos&&l.find(L"kept.dll")!=std::wstring::npos;check("repair: a change the file system did not keep is reported, not counted",rc==0&&names_file); }
  { World w=base();Fake &r=w.nodes[R];r=Fake{};ACCESS_ALLOWED_ACE inherited_deny=ace(SID_EVERYONE,0x2);inherited_deny.Header.AceType=ACCESS_DENIED_ACE_TYPE;inherited_deny.Header.AceFlags=INHERITED_ACE;r.aces={ace(SID_EVERYONE,0x1f01ff),inherited_deny,ace(SID_SYSTEM,0x1f01ff,INHERITED_ACE|OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE),ace(SID_ADMINS,0x1f01ff,INHERITED_ACE|OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE)};repair(w);
    size_t explicit_system=r.aces.size(),deny=r.aces.size();for(size_t i=r.aces.size();i-->0;){const auto &a=r.aces[i];if(a.SidStart==SID_SYSTEM&&!(a.Header.AceFlags&INHERITED_ACE))explicit_system=i;if(a.Header.AceType==ACCESS_DENIED_ACE_TYPE)deny=i;}
    check("repair: inherited grants behind an inherited deny do not count as keeping access",explicit_system<deny&&deny<r.aces.size()); }
  { World w=base();w.nodes[R].aces.push_back(ace(SID_USERS,0x1301bf));w.nodes[R].read_fails_after_write=true;const UINT rc=repair(w);check("repair: being unable to read a fix back does not alarm anyone",rc==0&&w.nodes[R].writes==1&&!warned(w)); }
  std::cout<<(failures?"FAILURES: ":"all guard scenarios passed: ")<<failures<<"\n";
  return failures!=0;
}
