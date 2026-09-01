#include <cerrno>
#include <cstdio>

#include <sys/capability.h>
#include <sys/prctl.h>

#include "../../../../src/platform/linux/capability_sanitizer.h"

#define CHECK(expression) do { \
  if (!(expression)) { \
    std::fprintf(stderr, "FAIL: %s:%d: %s (errno=%d)\n", \
                 __FILE__, __LINE__, #expression, errno); \
    return 1; \
  } \
} while (0)

namespace {
  bool is_retained_capability(cap_value_t capability) {
    return capability == CAP_SYS_ADMIN || capability == CAP_SYS_NICE;
  }
}

int main() {
  cap_t before = cap_get_proc();
  CHECK(before != nullptr);

  bool permitted_before[CAP_LAST_CAP + 1] {};
  for (int capability = 0; capability <= CAP_LAST_CAP; ++capability) {
    cap_flag_value_t value = CAP_CLEAR;
    CHECK(cap_get_flag(before, static_cast<cap_value_t>(capability),
                       CAP_PERMITTED, &value) == 0);
    permitted_before[capability] = value == CAP_SET;
  }
  cap_free(before);

  CHECK(platf::linux_security::sanitize_startup_capabilities());
  CHECK(platf::linux_security::sanitize_startup_capabilities());

  cap_t after = cap_get_proc();
  CHECK(after != nullptr);
  for (int capability = 0; capability <= CAP_LAST_CAP; ++capability) {
    cap_flag_value_t effective = CAP_SET;
    cap_flag_value_t inheritable = CAP_SET;
    cap_flag_value_t permitted = CAP_CLEAR;
    CHECK(cap_get_flag(after, static_cast<cap_value_t>(capability),
                       CAP_EFFECTIVE, &effective) == 0);
    CHECK(cap_get_flag(after, static_cast<cap_value_t>(capability),
                       CAP_INHERITABLE, &inheritable) == 0);
    CHECK(cap_get_flag(after, static_cast<cap_value_t>(capability),
                       CAP_PERMITTED, &permitted) == 0);
    CHECK(effective == CAP_CLEAR);
    CHECK(inheritable == CAP_CLEAR);
    const bool should_remain = permitted_before[capability] &&
                               is_retained_capability(static_cast<cap_value_t>(capability));
    CHECK((permitted == CAP_SET) == should_remain);
  }
  cap_free(after);

  for (int capability = 0; capability <= CAP_LAST_CAP; ++capability) {
    CHECK(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET,
                static_cast<unsigned long>(capability), 0, 0) == 0);
  }

  std::puts("PASS: Linux startup capability sanitization");
  return 0;
}
