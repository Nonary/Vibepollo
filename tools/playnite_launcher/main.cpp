#include "tools/playnite_launcher/launcher.h"

#include <string>
#include <vector>
#include <windows.h>
#include <winsock2.h>

int main(int argc, char **argv) {
  return playnite_launcher::launcher_run(argc, argv);
}

std::vector<std::string> convert_wide_args(int &argc, LPWSTR *&wargv) {
  wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::string> utf8;
  utf8.reserve(argc);
  for (int i = 0; i < argc; ++i) {
    int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s;
    s.resize(need > 0 ? static_cast<size_t>(need) - 1 : 0);
    if (need > 0) {
      WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), need, nullptr, nullptr);
    }
    utf8.emplace_back(std::move(s));
  }
  return utf8;
}

std::vector<char *> make_argv(std::vector<std::string> &utf8args) {
  std::vector<char *> argv;
  argv.reserve(utf8args.size());
  for (auto &s : utf8args) {
    argv.push_back(s.data());
  }
  return argv;
}

#ifdef _WIN32
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
  int argc = 0;
  LPWSTR *wargv = nullptr;
  auto utf8args = convert_wide_args(argc, wargv);
  auto argv = make_argv(utf8args);
  int rc = playnite_launcher::launcher_run(static_cast<int>(argv.size()), argv.data());
  if (wargv) {
    LocalFree(wargv);
  }
  return rc;
}
#endif
