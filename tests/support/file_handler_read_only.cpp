// Minimal test adapter for catalog fixtures.  Production file handling keeps
// its richer atomic-write and diagnostics adapter out of these read-only tests.
#include "src/file_handler.h"

#include <fstream>
#include <iterator>

namespace file_handler {
  std::string read_file(const char *path) {
    if (!path) {
      return {};
    }
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char> {input}, std::istreambuf_iterator<char> {}};
  }
}  // namespace file_handler
