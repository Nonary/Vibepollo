#pragma once
#include <cstdint>
#include <string>
#include <vector>
// The installer's whole folder policy. Stop an install only for a path Windows
// could never install to. Repair what every account on the PC could change, and
// warn only if that stays wrong: warnings are for serious problems. All else is logged.
namespace install_root_guard {
  enum class outcome_t { ok, unusable, not_a_folder };
  struct node_t { bool exists=false; bool directory=true; bool exposed=false; };
  struct verdict_t { outcome_t outcome; std::wstring path; };

  inline std::wstring without_trailing_separator(std::wstring path) {
    while(path.size()>3 && path.back()==L'\\') path.pop_back();
    return path;
  }
  // A local folder path with a name Windows allows. Every helper command quotes
  // this path, so characters like & and ' are fine, but % expands to a different
  // directory and ; splits the machine PATH entry.
  inline bool usable_path(const std::wstring &path) {
    return path.size()>3 && path[1]==L':' && path[2]==L'\\' &&
      path.find_first_of(L"\"|<>*?:%;\r\n",2)==std::wstring::npos;
  }
  // Rights that let an account add, replace or re-permission what is in a folder.
  inline bool ace_exposes(std::uint32_t mask) {
    constexpr std::uint32_t write_data=0x2,add_subdirectory=0x4,delete_child=0x40,write_dac=0x40000,write_owner=0x80000,generic_all=0x10000000,generic_write=0x40000000;
    return (mask & (write_data|add_subdirectory|delete_child|write_dac|write_owner|generic_all|generic_write))!=0;
  }
  // The same grant with every way to change anything taken away; reading stays.
  inline std::uint32_t read_only_mask(std::uint32_t mask) {
    constexpr std::uint32_t generic_read=0x80000000,generic_execute=0x20000000,generic_all=0x10000000,generic_write=0x40000000;
    constexpr std::uint32_t changes=0x2|0x4|0x10|0x40|0x100|0x10000|0x40000|0x80000|generic_all|generic_write;
    return (mask&~changes)|((mask&generic_all) ? generic_read|generic_execute : 0);
  }
  template<class Backend>
  verdict_t check(Backend &fs, std::wstring root) {
    root=without_trailing_separator(std::move(root));
    if(!usable_path(root)) return {outcome_t::unusable,root};
    // A folder that doesn't exist yet needs a real folder as its nearest existing parent.
    for(auto path=root;;) {
      const auto node=fs.inspect(path);
      if(node.exists) {
        if(!node.directory) return {outcome_t::not_a_folder,path};
        return {outcome_t::ok,path};
      }
      if(path.size()<=3) return {outcome_t::ok,root};
      const auto end=path.find_last_of(L'\\');
      path=path.substr(0,end==2 ? 3 : end);
    }
  }
  // Runs as SYSTEM, so only a confirmed plain folder is entered. The value repair
  // returns holds that folder open for as long as its children are being changed,
  // which stops it being swapped for a link partway through.
  template<class Backend>
  void repair_walk(Backend &fs, const std::wstring &path, bool root, std::size_t depth, std::size_t &count) {
    if(++count>100000 || depth>256) return;
    const auto entry=fs.repair(path,root);
    if(!entry.descend) return;
    for(const auto &child:fs.children(path)) repair_walk(fs,child,false,depth+1,count);
  }
  template<class Backend>
  void repair_tree(Backend &fs, const std::wstring &root) {
    std::size_t count=0;
    repair_walk(fs,root,true,0,count);
  }
}
