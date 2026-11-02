"""Run the MSI guard's actual traversal against deterministic unsafe trees."""
from pathlib import Path
import argparse,subprocess,tempfile,xml.etree.ElementTree as ET
p=argparse.ArgumentParser();p.add_argument('--compiler',default='g++');a=p.parse_args()
r=Path(__file__).resolve().parents[2]
ns={'w':'http://schemas.microsoft.com/wix/2006/wi'}
custom=ET.parse(r/'packaging/windows/wix/custom_actions.wxs')
assert custom.find('.//w:Binary[@Id="InstallRootGuard"]',ns).attrib['SourceFile'].endswith('sunshine_install_guard.dll')
guard=custom.find('.//w:CustomAction[@Id="ValidateInstallRoot"]',ns)
assert guard.attrib['Return']=='check' and guard.attrib['Execute']=='immediate'
seq=ET.parse(r/'packaging/windows/wix/patch_custom_actions.wxs')
assert seq.find('.//Custom[@Action="ValidateInstallRootPreview"]').attrib['After']=='CostFinalize'
assert seq.find('.//Custom[@Action="ValidateInstallRootPreview"]').text=='1'
assert seq.find('.//Custom[@Action="ValidateInstallRoot"]').text=='1'
assert seq.find('.//Custom[@Action="SetKillProcsQuietImmediate"]').attrib['After']=='ValidateInstallRootPreview'
assert seq.find('.//Custom[@Action="ValidateInstallRoot"]').attrib['After']=='StopSvcQuietImmediate'
assert seq.find('.//Custom[@Action="RemoveConflictingProducts"]').attrib['After']=='ValidateInstallRoot'
assert custom.find('.//w:CustomAction[@Id="BlockLegacySunshine"]',ns).get('Error')
# Repair must run as SYSTEM, never fail the install, and precede every helper.
repair=custom.find('.//w:CustomAction[@Id="RepairInstallRoot"]',ns)
assert repair.attrib['Execute']=='deferred' and repair.attrib['Impersonate']=='no' and repair.attrib['Return']=='ignore'
assert custom.find('.//w:CustomAction[@Id="SetRepairInstallRoot"]',ns).attrib['Property']=='RepairInstallRoot'
assert seq.find('.//Custom[@Action="RepairInstallRoot"]').attrib['Before']=='SetInstallSudovda'
assert seq.find('.//Custom[@Action="SetInstallSudovda"]').attrib['After']=='InstallFiles'
assert seq.find('.//Custom[@Action="SetRepairInstallRoot"]').attrib['Before']=='RepairInstallRoot'
assert seq.find('.//Custom[@Action="RepairInstallRoot"]').text=='NOT REMOVE'
for old in ('AskUninstallLegacySunshine','WaitForLegacyUninstall'):
 assert custom.find('.//w:CustomAction[@Id="'+old+'"]',ns) is None
 assert seq.find('.//Custom[@Action="'+old+'"]') is None
source=(r/'packaging/windows/install_root_guard.cpp').read_text()
bootstrap=(r/'packaging/windows/bootstrapper/VibeshineInstaller.cs').read_text()
preflight=bootstrap.split('private static InstallerResult UninstallCompetingProducts(',1)[1]
assert preflight.index('product => product.IsPerUser') < preflight.index('TryPreserveKnownMsiSourceBeforeReplacement')

# Checking looks at one folder and follows links. Repair must never follow one, must
# not propagate through the tree, and neither may fail the install.
check_side=source.split('struct filesystem_t {',1)[1].split('struct repairer_t {',1)[0]
repair_side=source.split('struct repairer_t {',1)[1].split('std::wstring normalized(',1)[0]
assert 'FILE_FLAG_OPEN_REPARSE_POINT' not in check_side and 'FindFirstFileW' not in check_side
assert 'FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS' in repair_side
assert 'FILE_ATTRIBUTE_REPARSE_POINT' in repair_side and 'nNumberOfLinks>1' in repair_side
assert 'SetKernelObjectSecurity' in repair_side
# Listing access makes Windows record the share mode, and omitting FILE_SHARE_DELETE is the pin.
assert 'FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,' in repair_side
for propagating in ('SetSecurityInfo(','SetNamedSecurityInfo','TreeSetNamedSecurityInfo'):
 assert propagating not in source,propagating
assert 'L"REMOVE"' in source and 'ERROR_SUCCESS;' in source.split('catch(...)',1)[1].split('}',1)[0]
assert source.split('UINT __stdcall RepairInstallRoot',1)[1].rstrip().endswith('return ERROR_SUCCESS;\n}')
for group in ('WinWorldSid','WinAuthenticatedUserSid','WinBuiltinUsersSid','WinInteractiveSid'):
 assert group in source,group
code=r'''
#include "packaging/windows/install_root_guard.h"
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>
#include <iostream>
using namespace install_root_guard;
void require(bool b){if(!b)throw std::runtime_error("guard regression");}
struct FS{std::map<std::wstring,node_t> nodes;std::vector<std::wstring> seen;
 node_t inspect(const std::wstring&p){seen.push_back(p);auto it=nodes.find(p);return it==nodes.end()?node_t{}:it->second;}};
FS tree(){FS f;f.nodes[L"C:\\"]=node_t{true};f.nodes[L"C:\\Program Files"]=node_t{true};return f;}
outcome_t run(FS&f,const std::wstring&root){return check(f,root).outcome;}
struct Tree{
 struct Held{bool descend;int*open;Held(bool d,int*o):descend(d),open(o){++*open;}~Held(){--*open;}Held(const Held&)=delete;};
 std::map<std::wstring,std::vector<std::wstring>> listing;std::map<std::wstring,bool> folder;std::vector<std::pair<std::wstring,bool>> repaired;std::map<std::wstring,int> held_at;int open=0;
 Held repair(const std::wstring&p,bool root){repaired.push_back({p,root});held_at[p]=open;return Held(folder[p],&open);}
 std::vector<std::wstring> children(const std::wstring&p){return listing[p];}};
int main(){
 const std::wstring host=L"C:\\Program Files\\Sunshine";
 // Masks taken from field reports' real ACLs.
 for(auto mask : {0x1200a9u,0xa0000000u,0x001000a1u,0x00010000u})require(!ace_exposes(mask));
 for(auto mask : {0x1301bfu,0x1f01ffu,0xe0010000u,0x00100004u,0x10000000u,0x40000000u,0x2u,0x4u,0x40u,0x40000u,0x80000u})require(ace_exposes(mask));

 for(auto ok : {L"C:\\Program Files\\Sunshine",L"D:\\Games\\Sunshine",L"C:\\Host",L"C:\\Program Files (x86)\\A b-c_d.e",L"C:\\R&D",L"C:\\a'b",L"C:\\$x",L"C:\\a^b"})require(usable_path(ok));
 for(auto bad : {L"",L"C:\\",L"C:",L"Sunshine",L"\\\\server\\share\\x",L"C:\\a\"b",L"C:\\a|b",L"C:\\a<b",L"C:\\a*b",L"C:\\a?b",L"C:\\a:b",L"C:\\100%",L"C:\\a;b"})require(!usable_path(bad));

 auto f=tree();f.nodes[host]=node_t{true};require(run(f,host)==outcome_t::ok);
 require(f.seen==std::vector<std::wstring>{host}); // nothing above an existing folder is examined
 require(run(f,host+L"\\")==outcome_t::ok);

 // C:\ with any ACL is never looked at for a Program Files install.
 f=tree();f.nodes[L"C:\\"].exposed=true;f.nodes[host]=node_t{true};require(run(f,host)==outcome_t::ok);
 f=tree();f.nodes[L"C:\\"].exposed=true;require(run(f,host)==outcome_t::ok);
 require((f.seen==std::vector<std::wstring>{host,L"C:\\Program Files"}));

 // Permissions are repaired, never a reason to stop or warn before installing.
 f=tree();f.nodes[host]=node_t{true,true,true};require(run(f,host)==outcome_t::ok);
 f=tree();f.nodes[L"D:\\"]=node_t{true,true,true};require(run(f,L"D:\\Games\\Sunshine")==outcome_t::ok);

 // Repair turns any write grant into exactly read and execute.
 require(read_only_mask(0x1301bf)==0x1200a9);
 require(read_only_mask(0x1f01ff)==0x1200a9);
 require(read_only_mask(0x1200a9)==0x1200a9);
 require(read_only_mask(0xe0010000u)==0xa0000000u);
 require(read_only_mask(0x10000000)==0xa0000000u);
 require(read_only_mask(0x40000000)==0);
 for(auto mask : {0x1301bfu,0x1f01ffu,0xe0010000u,0x10000000u,0x40000000u,0x2u,0x4u,0x40u,0x40000u,0x80000u,0x00100004u})require(!ace_exposes(read_only_mask(mask)));

 // Repair visits the whole tree, marks only the top as the root, and enters only what the backend confirms.
 Tree t;const std::wstring tools=host+L"\\tools",exe=tools+L"\\a.exe",link=host+L"\\link",beyond=link+L"\\beyond";
 t.folder[host]=t.folder[tools]=true;t.listing[host]={tools,link};t.listing[tools]={exe};t.listing[link]={beyond};
 repair_tree(t,host);
 require(t.repaired.size()==4);
 for(const auto&[path,is_root]:t.repaired){require(is_root==(path==host));require(path!=beyond);}
 // Every ancestor is still held when a child is changed, and all are released at the end.
 require(t.held_at[host]==0 && t.held_at[tools]==1 && t.held_at[exe]==2 && t.held_at[link]==1 && t.open==0);
 Tree deep;std::wstring level=host;for(int i=0;i<400;++i){const auto next=level+L"\\d";deep.folder[level]=true;deep.listing[level]={next};level=next;}
 repair_tree(deep,host);require(deep.repaired.size()==257); // bounded depth, because the walk recurses
 Tree huge;huge.folder[host]=true;for(int i=0;i<200000;++i)huge.listing[host].push_back(host+L"\\"+std::to_wstring(i));
 repair_tree(huge,host);require(huge.repaired.size()==100000); // stops at the bound without failing

 // Only unusable paths fail.
 f=tree();require(run(f,L"C:\\a|b")==outcome_t::unusable);require(f.seen.empty());
 f=tree();f.nodes[host]=node_t{true,false};auto v=check(f,host);require(v.outcome==outcome_t::not_a_folder && v.path==host);
 f=tree();f.nodes[L"C:\\Program Files\\Vendor"]=node_t{true,false};require(run(f,L"C:\\Program Files\\Vendor\\Host")==outcome_t::not_a_folder);
 // A missing drive is not the guard's business; Windows Installer reports it.
 f=FS{};require(run(f,L"Z:\\Host\\Deep")==outcome_t::ok);
 std::cout<<"MSI guard: usable paths, repair masks, no-follow walk, bounds and field ACLs passed\n";
}
'''
with tempfile.TemporaryDirectory() as tmp:
 cpp=Path(tmp)/'guard.cpp';exe=Path(tmp)/'guard';cpp.write_text(code)
 subprocess.run([a.compiler,'-std=c++23','-O2','-DNDEBUG','-Wall','-Wextra','-Werror','-I',str(r),str(cpp),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True,timeout=10)
 # The real guard exports against an in-memory filesystem; warnings come from the fake's headers.
 scenarios=Path(tmp)/'scenarios'
 subprocess.run([a.compiler,'-std=c++23','-O1','-w','-I',str(r/'tests/unit/install_root_guard_fake'),'-I',str(r),str(r/'tests/unit/install_root_guard_scenarios.cpp'),'-o',str(scenarios)],check=True)
 subprocess.run([str(scenarios)],check=True,timeout=30)
