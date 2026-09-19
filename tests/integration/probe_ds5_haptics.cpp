// SPDX-License-Identifier: GPL-3.0-or-later
// Manual: creates one temporary controller on slot 3 and plays only to its
// virtual USB audio endpoint. No physical controller or default audio is used.
#include <inputtino/input.hpp>
#include <alsa/asoundlib.h>
#include <atomic>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <cstdio>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

static std::set<std::string> cards() {
  std::set<std::string> result;
  for (const auto &entry : fs::directory_iterator("/sys/class/sound")) {
    const auto name = entry.path().filename().string();
    if (name.rfind("card", 0) == 0) result.insert(name);
  }
  return result;
}

int main() {
  const auto before = cards();
  std::atomic<unsigned> matched {0}, malformed {0}, discontinuities {0};
  int next_frame = -1;
  // Counters outlive the joypad; destruction joins its callback thread.
  auto pad = inputtino::PS5Joypad::create({.name = "Vibeshine DS5 PCM validation",
    .vendor_id = 0x054c, .product_id = 0x0ce6, .version = 0x0100,
    .device_uniq = "02:76:05:09:21:03"});
  if (!pad) return 1;
  (*pad).set_on_haptics([&](const unsigned char *pcm, std::size_t bytes) {
    if (bytes % 8) { ++malformed; return; }
    for (std::size_t offset = 0; offset < bytes; offset += 8) {
      const auto left = uint16_t(pcm[offset + 4] | (pcm[offset + 5] << 8));
      const auto right = uint16_t(pcm[offset + 6] | (pcm[offset + 7] << 8));
      if (right == uint16_t(left ^ 0x55aa) && int16_t(left) >= -240 && int16_t(left) < 240) {
        const auto frame = int16_t(left) + 240;
        if (next_frame >= 0 && frame != next_frame) ++discontinuities;
        next_frame = (frame + 1) % 480;
        ++matched;
      }
    }
  });
  std::string selected;
  for (int attempt = 0; attempt < 60 && selected.empty(); ++attempt) {
    for (const auto &card : cards()) {
      if (before.count(card)) continue;
      auto node = fs::canonical(fs::path("/sys/class/sound") / card / "device");
      if (node.string().find("vibeshine_ds5_hcd.3/") == std::string::npos) continue;
      for (int depth = 0; depth < 5 && !node.empty(); ++depth, node = node.parent_path()) {
        std::ifstream vidFile(node / "idVendor"), pidFile(node / "idProduct");
        std::string vid, pid;
        if (vidFile >> vid && pidFile >> pid && vid == "054c" && pid == "0ce6") selected = card;
      }
    }
    if (selected.empty()) std::this_thread::sleep_for(50ms);
  }
  if (selected.empty()) { std::fprintf(stderr, "No new composite DS5 audio endpoint (slot 3 must be free)\n"); return 2; }
  snd_pcm_t *pcm = nullptr;
  const auto name = "hw:" + selected.substr(4) + ",0";
  // The ALSA sysfs card appears before snd-usb-audio finishes registering PCM
  // and before udev creates its node/access ACL.
  const auto device = fs::path("/proc/asound") / selected / "id";
  for (int attempt = 0; attempt < 60 && !fs::exists(device); ++attempt) std::this_thread::sleep_for(50ms);
  const auto control = "/dev/snd/controlC" + selected.substr(4);
  for (int attempt = 0; attempt < 60 && access(control.c_str(), R_OK | W_OK) != 0; ++attempt) std::this_thread::sleep_for(50ms);
  int err = snd_pcm_open(&pcm, name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  if (err < 0) { std::fprintf(stderr, "open %s: %s\n", name.c_str(), snd_strerror(err)); return 3; }
  err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 4, 48000, 0, 20000);
  if (err < 0) { std::fprintf(stderr, "params: %s\n", snd_strerror(err)); snd_pcm_close(pcm); return 4; }
  std::array<uint8_t, 480 * 8> samples {};
  for (unsigned frame = 0; frame < 480; ++frame) {
    const auto left = uint16_t(int(frame) - 240);
    const auto right = uint16_t(left ^ 0x55aa);
    samples[frame * 8 + 4] = left & 255; samples[frame * 8 + 5] = left >> 8;
    samples[frame * 8 + 6] = right & 255; samples[frame * 8 + 7] = right >> 8;
  }
  const auto started = std::chrono::steady_clock::now();
  const auto end = started + 500ms;
  unsigned offset = 0;
  while (std::chrono::steady_clock::now() < end) {
    auto written = snd_pcm_writei(pcm, samples.data() + offset * 8, 480 - offset);
    if (written == -EAGAIN) { std::this_thread::sleep_for(1ms); continue; }
    if (written < 0 && snd_pcm_recover(pcm, int(written), 1) < 0) break;
    if (written > 0) offset = (offset + unsigned(written)) % 480;
  }
  std::this_thread::sleep_for(40ms);
  snd_pcm_drop(pcm); snd_pcm_close(pcm);
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  const auto rate = matched.load() / seconds;
  std::printf("USB audio -> gadget event -> Inputtino: %u signed stereo frames, %.0f frames/s, %u malformed blocks, %u discontinuities\n",
    matched.load(), rate, malformed.load(), discontinuities.load());
  return rate >= 36000 && rate <= 60000 && malformed == 0 && discontinuities == 0 ? 0 : 5;
}
