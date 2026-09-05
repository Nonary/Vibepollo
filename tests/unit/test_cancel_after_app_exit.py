#!/usr/bin/env python3
"""Compile the production cancel admission path with isolated transport stubs."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[2]
source = (root / "src/nvhttp.cpp").read_text()
start = source.index("  void cancel(resp_https_t response, req_https_t request) {")
end = source.index("#ifdef _WIN32\n    const bool preserve_deferred_launch", start)
body = source[start:end]
# The response serialization guard is unrelated to admission and needs the real
# HTTP server types. Keep the production permission/identity/state branches.
a = body.index("    auto g = util::fail_guard(")
b = body.index("    });", a) + len("    });")
body = body[:a] + body[b:]
body = body.replace("resp_https_t response", "resp_https_t /*response*/")
body += "    ++authorized_teardowns;\n  }\n"
downstream = "has_client_perm(verified_client" in body
policy = ""
if not downstream:
    remote = (root / "src/remote_session.cpp").read_text()
    a = remote.index("  bool allows_normal_game_cancel(")
    b = remote.index("\n  }", a) + 4
    policy = "namespace remote_session {" + remote[a:b] + "}"
program = r'''
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
using namespace std::string_view_literals;
struct req_https_t {};
struct resp_https_t {};
struct SunshineHTTPS {};
template<class T> void print_req(req_https_t) {}
int result, status, authorized_teardowns, owner_reads;
bool running, paired, permitted, remote_active;
std::string requesting_uuid, owner_uuid;
namespace pt {
struct ptree {
  void put(const char *key, int value) {
    if (std::string(key) == "root.cancel") result = value;
    if (std::string(key) == "root.<xmlattr>.status_code") status = value;
  }
  void put(const char *, const char *) {}
  void put(const char *, const std::string &) {}
};
}
struct identity_t { std::string uuid; };
identity_t resolve_client_identity_from_request(req_https_t) { return {paired ? requesting_uuid : ""}; }
int get_verified_cert(req_https_t) { return paired; }
namespace PERM { constexpr int launch = 1; }
bool has_client_perm(int cert, int) { return cert && permitted; }
void log_permission_denied(std::string_view, std::string_view, int) {}
std::string permission_denied_status_message(int, std::string_view) { return "denied"; }
identity_t resolve_client_identity(req_https_t, int cert) { return {cert ? requesting_uuid : ""}; }
namespace proc {
struct guard_t { std::string client_uuid; };
struct proc_t {
  int running() { return ::running ? 1 : 0; }
  guard_t active_session_guard() { ++owner_reads; return {owner_uuid}; }
} proc;
}
std::uint64_t active_session_generation(proc::guard_t) { return 1; }
struct snapshot_t { bool active; };
snapshot_t remote_role_gate_snapshot_for_client(const std::string &) { return {remote_active}; }
namespace remote_session {
struct caller_t { std::string uuid; bool paired; bool may_terminate; };
struct game_t { bool running; std::string owner_uuid; std::uint64_t generation; };
bool owns_game(const caller_t &caller, const game_t &game) { return caller.uuid == game.owner_uuid; }
void clear_app_replacement_confirmation(const std::string &) {}
}
''' + policy + body + r'''
void check(bool app, bool authenticated, bool permission, bool other, bool remote, int expected_status, bool teardown) {
  running = app; paired = authenticated; permitted = permission; remote_active = remote;
  requesting_uuid = "caller"; owner_uuid = other ? "peer" : "caller";
  result = -1; status = -1; authorized_teardowns = 0; owner_reads = 0;
  cancel({}, {});
  assert(status == expected_status);
  assert(authorized_teardowns == static_cast<int>(teardown));
  if (status == 200) assert(result == 1);
  if (!app) assert(owner_reads == 0);
}
int main() {
  // An exited app has already discarded its owner. Repeated cancellation is
  // successful but must never enter shared teardown or consult the old owner.
  for (bool other : {false, true}) for (bool remote : {false, true}) {
    check(false, true, true, other, remote, 200, false);
    check(false, false, true, other, remote, 403, false);
  }
  check(true, true, true, false, false, 200, true);
  check(true, true, true, false, true, 200, true);
  check(true, false, true, false, false, 403, false);
'''
if downstream:
    program += "check(false, true, false, false, false, 403, false);\ncheck(true, true, false, false, false, 403, false);\ncheck(true, true, true, true, false, 403, false);\ncheck(true, true, true, true, true, 403, false);\n"
else:
    program += "check(true, true, true, true, false, 200, true);\ncheck(true, true, true, true, true, 403, false);\n"
program += "}\n"
with tempfile.TemporaryDirectory(prefix="cancel-after-exit-") as directory:
    src = pathlib.Path(directory) / "test.cpp"
    binary = pathlib.Path(directory) / "test"
    src.write_text(program)
    subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(src), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print("Production cancel admission: exited-app, repeated cancel, authentication, ownership and teardown checks passed")
