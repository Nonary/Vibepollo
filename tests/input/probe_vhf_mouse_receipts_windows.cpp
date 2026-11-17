// Manual qualification only. Requires administrator and an installed UMDF
// keyboard/mouse producer. Never register this input-injecting probe in CTest.
#include "src/platform/windows/vhf_mouse_receipt.h"
#include <libvirtualgamepad/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace platf::vhf_keyboard_mouse;

template<class Request>
bool submit(lvg::client &client, mouse_receipt &receipt, const Request &request,
            mouse_receipt::expectation expected) {
  if (!receipt.arm(expected)) { std::puts("Receipt barrier failed"); return false; }
  DWORD status = ERROR_BUSY;
  for (int attempt = 0; attempt < 20 && status == ERROR_BUSY; ++attempt) {
    status = client.submit_keyboard_mouse(request);
    if (status == ERROR_BUSY) Sleep(1);
  }
  if (status != ERROR_SUCCESS) std::printf("Submission failed: %lu\n", status);
  return status == ERROR_SUCCESS;
}

template<class Send>
bool benchmark(const char *name, mouse_receipt &receipt, Send send) {
  std::vector<long long> timings;
  const auto started = std::chrono::steady_clock::now();
  const auto before = receipt.stats();
  for (int i = 0; i < 512; ++i) {
    const auto begin = std::chrono::steady_clock::now();
    if (!send(i)) { std::printf("%s failed at report %d\n", name, i); return false; }
    timings.push_back(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
  }
  if (!receipt.wait()) { std::printf("%s final receipt drain failed\n", name); return false; }
  const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count();
  const auto after = receipt.stats();
  std::sort(timings.begin(), timings.end());
  std::printf("PASS %s events=512 elapsed_us=%lld admission_p50_us=%lld admission_p99_us=%lld barriers=%llu blocked_us=%llu coalesced=%llu peak=%zu\n",
    name, static_cast<long long>(elapsed), timings[256], timings[506],
    static_cast<unsigned long long>(after.barriers - before.barriers),
    static_cast<unsigned long long>(after.blocked_microseconds - before.blocked_microseconds),
    static_cast<unsigned long long>(after.coalesced - before.coalesced), after.peak_pending);
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2 || std::strcmp(argv[1], "--run") != 0) {
    std::puts("Manual probe: run elevated with --run. Sends small alternating mouse movements, then tests absolute/Windows input with the cursor clipped in place. Restores cursor/clipping; no clicks, keyboard input, driver installation or trust changes.");
    return 0;
  }
  lvg::client client;
  DWORD status = client.connect();
  if (status != ERROR_SUCCESS) { std::printf("Connect failed: %lu\n", status); return 1; }
  lvg::km::query_response info {};
  status = client.query_keyboard_mouse(&info);
  std::printf("query=%lu capabilities=%lu generation=%lu\n", status,
    static_cast<unsigned long>(info.capabilities), static_cast<unsigned long>(info.absolute_target_generation));
  if (status != ERROR_SUCCESS || info.absolute_target_generation != 1 ||
      (info.capabilities & lvg::km::primary_absolute_mouse) == 0) return 2;
  status = client.open_keyboard_mouse();
  if (status != ERROR_SUCCESS) { std::printf("Open failed: %lu\n", status); return 3; }
  struct teardown {
    lvg::client &client;
    POINT point {};
    RECT clip {};
    bool restore {}, closed {};
    DWORD result {};
    DWORD finish() {
      if (closed) return result;
      closed = true;
      result = client.abort_keyboard_mouse();
      std::printf("abort=%lu\n", result);
      // Delete queued input before releasing the temporary clip, including
      // failure exits. Otherwise an unacknowledged position could land later.
      if (restore && (!ClipCursor(&clip) || !SetCursorPos(point.x, point.y))) {
        std::puts("Cursor restoration failed");
        if (result == ERROR_SUCCESS) result = ERROR_GEN_FAILURE;
      }
      return result;
    }
    ~teardown() { finish(); }
  } owner {client};
  mouse_receipt receipt;
  if (!receipt.start() || !receipt.bind()) { std::puts("Owned-device receipt observer unavailable"); return 4; }
  POINT original {};
  RECT previous_clip {};
  if (!GetCursorPos(&original) || !GetClipCursor(&previous_clip)) return 5;
  owner.point = original;
  owner.clip = previous_clip;
  owner.restore = true;

  const auto relative = [&](int x) {
    auto request = lvg::km::request<lvg::km::relative_request>();
    request.x = static_cast<std::int16_t>(x);
    mouse_receipt::expectation expected {};
    expected.motion = true;
    expected.x = x;
    return submit(client, receipt, request, expected);
  };
  const auto absolute = [&](int i) {
    auto request = lvg::km::request<lvg::km::absolute_request>();
    request.x = static_cast<std::uint16_t>((12345 + i * 777) % 65536);
    request.y = static_cast<std::uint16_t>((54321 + i * 555) % 65536);
    request.target_generation = info.absolute_target_generation;
    mouse_receipt::expectation expected {};
    expected.absolute = expected.motion = true;
    expected.x = static_cast<LONG>((std::uint64_t(request.x >> 1) * 65535) / 32767);
    expected.y = static_cast<LONG>((std::uint64_t(request.y >> 1) * 65535) / 32767);
    return submit(client, receipt, request, expected);
  };
  if (!benchmark("relative", receipt, [&](int i) { return relative(i % 2 ? -1 : 1); })) return 6;
  const RECT clipped {original.x, original.y, original.x + 1, original.y + 1};
  if (!ClipCursor(&clipped)) return 7;
  if (!benchmark("absolute-with-clipped-cursor", receipt, absolute)) return 8;
  if (!benchmark("absolute-identical-position", receipt, [&](int) { return absolute(9); })) return 8;
  if (!benchmark("absolute-reversing-position", receipt, [&](int i) { return absolute(i % 2); })) return 8;

  const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (vw <= 1 || vh <= 1) return 9;
  const auto x = static_cast<std::uint16_t>(std::clamp<long long>((static_cast<long long>(original.x) - vx) * 65535 / (vw - 1), 0, 65535));
  const auto y = static_cast<std::uint16_t>(std::clamp<long long>((static_cast<long long>(original.y) - vy) * 65535 / (vh - 1), 0, 65535));
  if (!benchmark("windows-absolute-with-clipped-cursor", receipt,
                 [&](int) { return receipt.inject_absolute(x, y); })) return 10;
  const auto started = std::chrono::steady_clock::now();
  for (int i = 0; i < 5; ++i) {
    if (!relative(1) || !absolute(i) || !relative(-1) || !receipt.inject_absolute(x, y)) return 10;
  }
  if (!receipt.wait()) return 11;
  std::printf("PASS clipped relative/absolute/Windows-input crossings=20 elapsed_us=%lld\n",
    static_cast<long long>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()));
  return owner.finish() == ERROR_SUCCESS ? 0 : 12;
}
