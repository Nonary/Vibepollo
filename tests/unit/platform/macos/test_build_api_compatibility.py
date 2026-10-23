"""Compile macOS adapter expressions against real Boost and a typed DNS-SD port.

This is a portable API regression test, not a native macOS build or Bonjour test.
Pass the configured Boost include directories with --include-dir (semicolon
lists are accepted). No Boost or DNS-SD implementation is mocked for the
working-directory initializer; only the DNSServiceRegister effect is replaced.
"""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, marker):
    begin = source.index(marker)
    end = source.index('{', begin) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


def check_process(root, compiler, includes, process_library, directory):
    source = (root / 'src/platform/macos/misc.mm').read_text()
    production = function(source, 'bp::child run_command(')
    cpp = directory / 'command.cpp'
    cpp.write_text('''
#include <boost/program_options/parsers.hpp>
#include "src/boost_process_shim.h"
namespace bp = boost_process_shim;
namespace v2 = boost::process::v2;
''' + production)
    # Check every production launch branch, including grouped/ungrouped launches
    # and an empty working directory, with the actual process and shim types.
    subprocess.run([compiler, '-std=c++23', '-fsyntax-only', '-I', str(root),
                    *includes, str(cpp)], check=True)

    initializers = re.findall(r'auto start = [^;]+;', production)
    assert len(initializers) == 2, 'Expected grouped and ungrouped cwd initializers'
    cases = '\n'.join('''
void case_INDEX(const std::string &directory) {
  v2::filesystem::path working_dir(directory);
  INITIALIZER
  // The temporary path/string and later mutation must not invalidate the cwd.
  working_dir.clear();
  v2::posix::default_launcher launcher;
  const auto error = start.on_exec_setup(launcher, {}, nullptr);
  if (directory.empty() || !std::filesystem::exists(directory)) {
    assert(error);
  } else {
    assert(!error);
    assert(std::filesystem::equivalent(std::filesystem::current_path(), directory));
  }
}
'''.replace('INDEX', str(index)).replace('INITIALIZER', initializer)
                      for index, initializer in enumerate(initializers))
    cpp.write_text('''
#include <boost/process/v2/start_dir.hpp>
#include <boost/version.hpp>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
namespace v2 = boost::process::v2;
''' + cases + '''
int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string valid = argv[1];
  case_0(valid);
  case_1(valid);
  case_0(valid + "/missing");
  case_1(valid + "/missing");
  std::cout << "Both cwd initializers passed with Boost " << BOOST_LIB_VERSION << '\\n';
}
''')
    binary = directory / 'command'
    library_flags = ['-lboost_process']
    if process_library:
        library_flags = [str(process_library), '-Wl,-rpath,' + str(process_library.parent)]
    subprocess.run([compiler, '-std=c++23', '-pthread', *includes, str(cpp),
                    *library_flags, '-o', str(binary)], check=True)
    working_dir = directory / 'working directory with spaces'
    working_dir.mkdir()
    subprocess.run([str(binary), str(working_dir)], check=True)


def check_service(root, compiler, directory):
    common = (root / 'src/platform/common.h').read_text()
    declaration = re.search(r'inline constexpr std::string_view SERVICE_TYPE\s*=\s*[^;]+;', common)
    assert declaration, 'Expected the production service-type declaration'
    publish = (root / 'src/platform/macos/publish.cpp').read_text()
    call = re.search(r'const auto status = DNSServiceRegister\([\s\S]+?\n\s*\);', publish)
    assert call, 'Expected the production registration call'
    # Signature follows Apple's mDNSShared/dns_sd.h. Keep const char *regtype:
    # a permissive generic stub would miss the original string_view error.
    cpp = directory / 'service.cpp'
    cpp.write_text('''
#include <arpa/inet.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string_view>
namespace platf { DECLARATION }
using DNSServiceRef = void *;
using DNSServiceFlags = std::uint32_t;
using DNSServiceErrorType = std::int32_t;
using DNSServiceRegisterReply = void (*)(DNSServiceRef, DNSServiceFlags,
    DNSServiceErrorType, const char *, const char *, const char *, void *);
namespace net { std::uint16_t map_port(std::uint16_t port) { return port; } }
namespace nvhttp { constexpr std::uint16_t PORT_HTTP = 47989; }
void registrationCallback(DNSServiceRef, DNSServiceFlags, DNSServiceErrorType,
                          const char *, const char *, const char *, void *) {}
int calls = 0;
DNSServiceErrorType DNSServiceRegister(DNSServiceRef *ref, DNSServiceFlags flags,
    std::uint32_t index, const char *name, const char *regtype, const char *domain,
    const char *host, std::uint16_t port, std::uint16_t length, const void *record,
    DNSServiceRegisterReply callback, void *context) {
  assert(ref && !flags && !index && !name && !domain && !host && !length && !record && !context);
  assert(callback == registrationCallback && port == htons(nvhttp::PORT_HTTP));
  assert(regtype == platf::SERVICE_TYPE.data());
  assert(std::strlen(regtype) == platf::SERVICE_TYPE.size());
  assert(std::strcmp(regtype, "_nvstream._tcp") == 0);
  ++calls;
  return 0;
}
int main() {
  // data() is safe only because this particular view spans the complete literal.
  static_assert(platf::SERVICE_TYPE.data()[platf::SERVICE_TYPE.size()] == '\\0');
  auto serviceRef = DNSServiceRef{};
  CALL
  assert(status == 0 && calls == 1);
}
'''.replace('DECLARATION', declaration.group()).replace('CALL', call.group()))
    binary = directory / 'service'
    subprocess.run([compiler, '-std=c++23', '-Wall', '-Wextra', '-Werror', str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    print('Production DNSServiceRegister call passed the typed service-name port')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument('--compiler', default='c++')
    parser.add_argument('--include-dir', action='append', default=[])
    parser.add_argument('--process-library', type=Path)
    parser.add_argument('--check', choices=('all', 'process', 'service'), default='all')
    args = parser.parse_args()
    includes = ['-I' + path for item in args.include_dir for path in item.split(';') if path]
    with tempfile.TemporaryDirectory(prefix='macos-api-') as temporary:
        directory = Path(temporary)
        if args.check in ('all', 'process'):
            check_process(args.repo, args.compiler, includes, args.process_library, directory)
        if args.check in ('all', 'service'):
            check_service(args.repo, args.compiler, directory)


if __name__ == '__main__':
    main()
