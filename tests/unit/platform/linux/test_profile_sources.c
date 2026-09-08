#define main profile_import_main
#include "packaging/linux/vibepollo-profile-import.c"
#undef main
#include <assert.h>

int main(void) {
  char fixture[] = "/tmp/vibepollo-profile-sources.XXXXXX";
  assert(mkdtemp(fixture));
  int home = open(fixture, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  assert(home >= 0);
  assert(mkdirat(home, ".config", 0700) == 0);
  assert(open_source_profile(home, "auto") == -1 && errno == ENOENT);
  assert(mkdirat(home, ".config/vibeshine", 0700) == 0);
  int source = open_source_profile(home, "auto");
  assert(source >= 0); close(source);
  assert(mkdirat(home, ".config/sunshine", 0700) == 0);
  assert(open_source_profile(home, "auto") == -1 && errno == EEXIST);
  source = open_source_profile(home, "sunshine");
  assert(source >= 0); close(source);
  assert(unlinkat(home, ".config/sunshine", AT_REMOVEDIR) == 0);
  assert(symlinkat("/tmp", home, ".config/sunshine") == 0);
  assert(open_source_profile(home, "auto") == -1 && (errno == ELOOP || errno == ENOTDIR));
  assert(unlinkat(home, ".config/sunshine", 0) == 0);
  assert(unlinkat(home, ".config/vibeshine", AT_REMOVEDIR) == 0);
  assert(unlinkat(home, ".config", AT_REMOVEDIR) == 0);
  close(home);
  assert(rmdir(fixture) == 0);
  puts("Legacy profile selection, ambiguity and symlink refusal passed.");
}
