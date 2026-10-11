/**
 * @file src/aurora_link/mic.cpp
 * @brief Receive the Pi's Opus/RTP voice stream and render it into the virtual cable.
 *
 * WHY THIS REPLACES A PYTHON SCRIPT
 * --------------------------------
 * The mic chain was `mic_receiver.py` + ffmpeg + sounddevice + numpy. That chain is the
 * ONLY reason the host stack is not a single executable: everything else already ships
 * inside the host. It also drags a per-machine Python install and a borrowed ffmpeg.exe
 * into a fork that is supposed to install with no machine-specific setup.
 *
 * The host already links Opus and already talks WASAPI, so the whole chain collapses to
 * one verb in this binary: UDP -> RTP depacketise -> opus_decode -> WASAPI render.
 *
 * WIRE FORMAT, fixed by the Pi's sender (xlink_mic_send.sh):
 *   ffmpeg ... -ac 1 -ar 48000 -c:a libopus -b:a 24k -application lowdelay
 *              -frame_duration 10 -payload_type 111 -f rtp rtp://<host>:48210
 * so: RTP payload type 111, Opus, 48 kHz, MONO, 10 ms frames (480 samples).
 *
 * LATENCY IS THE POINT ([[latency-minimization]]): this is voice for a game session, so
 * every buffer here is sized for delay, not for safety. We render each packet as it
 * arrives into a small shared-mode buffer rather than pre-buffering for smoothness.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <mmreg.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

#include <opus/opus.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aurora_link.h"

namespace aurora_link {

  namespace {

    // Spelled out rather than __uuidof: GCC/MinGW's support is limited, and a silently
    // wrong IID yields a pointer that faults on first use rather than failing cleanly.
    const CLSID CLSID_MMDeviceEnumerator_ = {
      0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
    const IID IID_IMMDeviceEnumerator_ = {
      0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
    const IID IID_IAudioClient_ = {
      0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
    const IID IID_IAudioRenderClient_ = {
      0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};

    // {00000003-0000-0010-8000-00AA00389B71}. Spelled out because the SDK's symbol
    // needs INITGUID or an extra import lib, and the link failure it causes
    // (undefined reference to KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) says nothing about that.
    const GUID kSubtypeFloat = {
      0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

    constexpr int kRate = 48000;
    constexpr int kFrame = 480;        // 10 ms, matching the sender's -frame_duration 10
    constexpr int kMaxFrame = 5760;    // 120 ms, the largest Opus can hand back
    constexpr unsigned char kPayloadType = 111;

    /**
     * Endpoint whose friendly name contains any of `wants`, in priority order.
     *
     * A LIST, not one name, because the cable's endpoints get renamed at install time
     * (`aurora-link rename-audio`) and a host that has not been through that still has
     * the stock VB-CABLE names. Matching either way means the mic does not depend on
     * which of the two states a given machine is in -- the same reason mic_receiver.py
     * takes a list.
     */
    IMMDevice *find_render_device(IMMDeviceEnumerator *en, const std::vector<std::wstring> &wants) {
      IMMDeviceCollection *coll = nullptr;
      if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) || coll == nullptr) {
        return nullptr;
      }
      UINT count = 0;
      coll->GetCount(&count);
      IMMDevice *best = nullptr;
      size_t best_rank = wants.size();
      size_t best_len = SIZE_MAX;
      for (UINT i = 0; i < count; ++i) {
        IMMDevice *dev = nullptr;
        if (FAILED(coll->Item(i, &dev)) || dev == nullptr) {
          continue;
        }
        IPropertyStore *props = nullptr;
        std::wstring name;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props != nullptr) {
          PROPVARIANT v;
          PropVariantInit(&v);
          if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
            name = v.pwszVal;
          }
          PropVariantClear(&v);
          props->Release();
        }
        for (size_t r = 0; r < wants.size(); ++r) {
          if (name.find(wants[r]) == std::wstring::npos) {
            continue;
          }
          // Prefer the earlier candidate, then the SHORTEST name: each stereo bridge
          // label is a prefix of its 16-channel counterpart, and the 16-channel
          // endpoint accepts audio and is simply never heard.
          if (r < best_rank || (r == best_rank && name.size() < best_len)) {
            if (best != nullptr) best->Release();
            best = dev;
            dev = nullptr;
            best_rank = r;
            best_len = name.size();
            wprintf(L"aurora-link: mic -> [%ls]\n", name.c_str());
          }
          break;
        }
        if (dev != nullptr) dev->Release();
      }
      coll->Release();
      return best;
    }

    /** Write mono int16 samples into whatever format the endpoint actually wants. */
    void convert(const opus_int16 *src, int frames, const WAVEFORMATEX *fmt, BYTE *dst) {
      const int ch = fmt->nChannels;
      const bool is_float = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                            (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                             IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt)->SubFormat,
                                         kSubtypeFloat));
      for (int i = 0; i < frames; ++i) {
        for (int c = 0; c < ch; ++c) {   // mono source fans out to every channel
          if (is_float) {
            reinterpret_cast<float *>(dst)[i * ch + c] = src[i] / 32768.0f;
          } else {
            reinterpret_cast<opus_int16 *>(dst)[i * ch + c] = src[i];
          }
        }
      }
    }

  }  // namespace

  int run_mic_host() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
      fprintf(stderr, "aurora-link: WSAStartup failed\n");
      return 1;
    }
    const bool com_here = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    IMMDeviceEnumerator *en = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL,
                                IID_IMMDeviceEnumerator_, reinterpret_cast<void **>(&en)))) {
      fprintf(stderr, "aurora-link: no device enumerator\n");
      return 1;
    }
    // WAIT for the endpoint rather than exiting if it is not there yet. The session
    // hook ENABLES the cable moments before starting this host (cable-session.cmd runs
    // first, elevated), and a PnP device does not appear in the endpoint enumeration
    // instantly. Exiting on the first miss would lose the mic for the whole session,
    // silently -- the failure looks identical to "no one spoke".
    IMMDevice *dev = nullptr;
    for (int waited = 0; waited < 20 && dev == nullptr && !helper_stopping(); ++waited) {
      // Eclipse is the current visible identity. Aurora remains a migration fallback
      // until an Eclipse-aware rollback build has been preserved and the elevated
      // cable path can safely rename the persistent Windows endpoint property.
      dev = find_render_device(en, {L"Eclipse Mic Bridge", L"Aurora Mic Bridge", L"CABLE Input"});
      if (dev == nullptr) {
        if (waited == 0) {
          printf("aurora-link: waiting for the virtual cable endpoint...\n");
          fflush(stdout);
        }
        Sleep(1000);
      }
    }
    if (dev == nullptr) {
      // Genuinely absent: the cable is hidden between sessions, so this is a normal
      // state to report, not a crash.
      fprintf(stderr, "aurora-link: no virtual cable endpoint after 20s (is VB-CABLE "
                      "installed and shown? `aurora-link cable on`)\n");
      en->Release();
      return 1;
    }

    IAudioClient *client = nullptr;
    if (FAILED(dev->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr,
                             reinterpret_cast<void **>(&client)))) {
      fprintf(stderr, "aurora-link: could not activate audio client\n");
      return 1;
    }
    WAVEFORMATEX *fmt = nullptr;
    client->GetMixFormat(&fmt);
    // Shared mode at the endpoint's own mix format: exclusive mode would fight anything
    // else using the cable and buys nothing here, since the cable is ours already.
    // 40 ms of buffer -- enough to absorb network jitter, small enough not to be heard.
    const REFERENCE_TIME kBuffer = 400000;  // 100-ns units
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBuffer, 0, fmt, nullptr))) {
      fprintf(stderr, "aurora-link: audio client init failed\n");
      return 1;
    }
    UINT32 buffer_frames = 0;
    client->GetBufferSize(&buffer_frames);
    IAudioRenderClient *render = nullptr;
    client->GetService(IID_IAudioRenderClient_, reinterpret_cast<void **>(&render));
    client->Start();

    int err = 0;
    OpusDecoder *dec = opus_decoder_create(kRate, 1, &err);
    if (dec == nullptr || err != OPUS_OK) {
      fprintf(stderr, "aurora-link: opus_decoder_create failed (%d)\n", err);
      return 1;
    }

    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(mic_port);
    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
      fprintf(stderr, "aurora-link: cannot bind UDP :%u (already running?)\n", mic_port);
      return 1;
    }
    DWORD receive_timeout = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&receive_timeout), sizeof(receive_timeout));
    printf("aurora-link: mic host listening on UDP :%u (opus %d Hz mono)\n", mic_port, kRate);
    fflush(stdout);

    std::vector<opus_int16> pcm(kMaxFrame);
    unsigned char pkt[2048];
    unsigned long long rendered = 0, lost = 0;
    int last_seq = -1;

    while (!helper_stopping()) {
      int n = recv(s, reinterpret_cast<char *>(pkt), sizeof(pkt), 0);
      if (n < 12) {
        continue;  // shorter than an RTP header: not ours
      }
      // Minimal RTP parse. We only need the payload; the sender is fixed and local, so
      // there is no profile negotiation to honour.
      const int version = pkt[0] >> 6;
      const int has_ext = (pkt[0] >> 4) & 1;
      const int csrc = pkt[0] & 0x0F;
      const unsigned char pt = pkt[1] & 0x7F;
      if (version != 2 || pt != kPayloadType) {
        continue;
      }
      int off = 12 + 4 * csrc;
      if (has_ext && n >= off + 4) {
        off += 4 + 4 * ((pkt[off + 2] << 8) | pkt[off + 3]);
      }
      if (off >= n) {
        continue;
      }
      const int seq = (pkt[2] << 8) | pkt[3];

      // A GAP MEANS LOST PACKETS: ask Opus to conceal them rather than just skipping,
      // otherwise the decoder's state jumps and the next real frame arrives with an
      // audible click. One PLC frame per missing packet, bounded so a long dropout
      // cannot stall the loop.
      if (last_seq >= 0) {
        int gap = (seq - last_seq - 1) & 0xFFFF;
        for (int i = 0; i < gap && i < 5; ++i) {
          int got = opus_decode(dec, nullptr, 0, pcm.data(), kFrame, 0);
          if (got > 0) {
            ++lost;
          }
        }
      }
      last_seq = seq;

      int frames = opus_decode(dec, pkt + off, n - off, pcm.data(), kMaxFrame, 0);
      if (frames <= 0) {
        continue;
      }

      UINT32 padding = 0;
      client->GetCurrentPadding(&padding);
      const UINT32 space = buffer_frames > padding ? buffer_frames - padding : 0;
      if (space == 0) {
        // Endpoint is behind: drop rather than block. For voice, a dropped 10 ms is
        // better than growing latency that never recovers.
        continue;
      }
      const UINT32 want = static_cast<UINT32>(frames) < space ? static_cast<UINT32>(frames) : space;
      BYTE *out = nullptr;
      if (SUCCEEDED(render->GetBuffer(want, &out)) && out != nullptr) {
        convert(pcm.data(), static_cast<int>(want), fmt, out);
        render->ReleaseBuffer(want, 0);
        rendered += want;
        if ((rendered % (kRate * 30)) < want) {   // ~ every 30 s of audio
          printf("aurora-link: mic ok (%llu s rendered, %llu concealed)\n",
                 rendered / kRate, lost);
          fflush(stdout);
        }
      }
    }

    client->Stop();
    render->Release();
    CoTaskMemFree(fmt);
    client->Release();
    dev->Release();
    en->Release();
    opus_decoder_destroy(dec);
    closesocket(s);
    WSACleanup();
    if (com_here) CoUninitialize();
    return 0;
  }

}  // namespace aurora_link
