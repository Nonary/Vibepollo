// SPDX-License-Identifier: GPL-3.0-or-later
// Hardware-free Windows integration check for the process-local endpoint.
#include "ipc.h"

#include <audioclient.h>
#include <cstdio>
#include <mmdeviceapi.h>
#include <string>

#define CHECK(expr) \
  do { \
    if (!(expr)) { \
      std::fprintf(stderr, "Failed at line %d: %s\n", __LINE__, #expr); \
      return 1; \
    } \
  } while (0)

int wmain(int argc, wchar_t **argv) {
  using namespace dualsense_haptics;
  CHECK(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
  const bool child_mode = argc > 1;
  if (child_mode) {
    CHECK(!IsDebuggerPresent());
  }
  wchar_t name[128] {};
  shared_state *state = nullptr;
  HANDLE mapping = nullptr;
  if (!child_mode) {
    swprintf(name, 128, L"Local\\Vibepollo.Haptics.Test.%lu", GetCurrentProcessId());
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(shared_state), name);
    CHECK(mapping);
    state = static_cast<shared_state *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(shared_state)));
    CHECK(state);
    *state = {};
    state->signature = magic;
    state->alive = 1;
    state->slots[3].active = 1;
    CHECK(SetEnvironmentVariableW(environment_key, name));
    const auto dll = LoadLibraryW(L"vibepollo_dualsense_audio.dll");
    CHECK(dll);
    const auto init = reinterpret_cast<DWORD(WINAPI *)(void *)>(GetProcAddress(dll, "VibepolloHapticsInitialize"));
    CHECK(init && init(nullptr));
  }
  IMMDeviceEnumerator *enumerator = nullptr;
  CHECK(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator))));
  IMMDeviceCollection *collection = nullptr;
  CHECK(SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection)));
  UINT count = 0;
  CHECK(SUCCEEDED(collection->GetCount(&count)) && count > 0);
  IMMDevice *device = nullptr;
  CHECK(SUCCEEDED(enumerator->GetDevice(L"Vibepollo.DualSense.Audio.3", &device)));
  IPropertyStore *properties = nullptr;
  CHECK(SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)));
  PROPERTYKEY key {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
  PROPVARIANT value {};
  CHECK(SUCCEEDED(properties->GetValue(key, &value)) && value.vt == VT_LPWSTR);
  GUID actual {};
  CHECK(SUCCEEDED(CLSIDFromString(value.pwszVal, &actual)) && actual == container_id(3));
  PropVariantClear(&value);
  properties->Release();
  IAudioClient *client = nullptr;
  CHECK(SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void **>(&client))));
  WAVEFORMATEX *format = nullptr;
  CHECK(SUCCEEDED(client->GetMixFormat(&format)) && format->nChannels == 4 && format->wBitsPerSample == 32);
  // Reproduce Wwise changing the mix rate to match its engine.
  format->nSamplesPerSec = 96000;
  format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
  CHECK(SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 200000, 0, format, nullptr)));
  CoTaskMemFree(format);
  IAudioRenderClient *render = nullptr;
  CHECK(SUCCEEDED(client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&render))));
  CHECK(SUCCEEDED(client->Start()));
  BYTE *buffer = nullptr;
  CHECK(SUCCEEDED(render->GetBuffer(480, &buffer)) && buffer);
  for (unsigned i = 0; i < 480; ++i) {
    auto samples = reinterpret_cast<float *>(buffer);
    samples[i * 4] = 1;
    samples[i * 4 + 1] = -1;
    samples[i * 4 + 2] = -0.5;
    samples[i * 4 + 3] = 0.25;
  }
  CHECK(render->GetBuffer(1, &buffer) == AUDCLNT_E_OUT_OF_ORDER);
  CHECK(SUCCEEDED(render->ReleaseBuffer(480, 0)));
  UINT32 padding = 0;
  CHECK(SUCCEEDED(client->GetCurrentPadding(&padding)) && padding <= 480);
  if (!child_mode) {
    auto &slot = state->slots[3];
    CHECK(slot.write == 1);
    CHECK(slot.packets[0].samples[0] == 0 && slot.packets[0].samples[1] == 0xc0);
    CHECK(slot.packets[0].samples[2] == 0 && slot.packets[0].samples[3] == 0x20);
    CHECK(SUCCEEDED(render->GetBuffer(480, &buffer)));
    CHECK(SUCCEEDED(render->ReleaseBuffer(480, AUDCLNT_BUFFERFLAGS_SILENT)));
    CHECK(slot.write == 2);
    for (auto sample : slot.packets[1].samples) {
      CHECK(sample == 0);
    }
    // Check Unicode/ANSI and caller-requested suspension, with an explicit
    // environment that omits the haptics mapping name. No debugger is attached.
    wchar_t path[32768] {};
    CHECK(GetModuleFileNameW(nullptr, path, 32768));
    for (bool ansi : {false, true}) {
      const auto before = slot.write;
      std::wstring command = L"\"" + std::wstring(path) + L"\" --child";
      wchar_t custom_env[] = L"SystemRoot=C:\\windows\0HAPTICS_PROBE=1\0\0";
      STARTUPINFOW startup {sizeof(startup)};
      PROCESS_INFORMATION child {};
      BOOL created = FALSE;
      if (ansi) {
        const auto size = WideCharToMultiByte(CP_ACP, 0, command.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string narrow(size, '\0');
        WideCharToMultiByte(CP_ACP, 0, command.c_str(), -1, narrow.data(), size, nullptr, nullptr);
        STARTUPINFOA startup_a {sizeof(startup_a)};
        created = CreateProcessA(nullptr, narrow.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, custom_env, nullptr, &startup_a, &child);
      } else {
        created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, custom_env, nullptr, &startup, &child);
      }
      CHECK(created);
      Sleep(20);
      CHECK(slot.write == before);
      CHECK(ResumeThread(child.hThread) == 1);
      CHECK(WaitForSingleObject(child.hProcess, 20000) == WAIT_OBJECT_0);
      DWORD code = 1;
      CHECK(GetExitCodeProcess(child.hProcess, &code) && code == 0);
      CHECK(slot.write == before + 1);
      CHECK(slot.packets[before % queue_size].sequence == before);
      CloseHandle(child.hThread);
      CloseHandle(child.hProcess);
    }
    InterlockedIncrement(&slot.generation);
    CHECK(client->GetCurrentPadding(&padding) == AUDCLNT_E_DEVICE_INVALIDATED);
  }
  CHECK(SUCCEEDED(client->Stop()));
  CHECK(SUCCEEDED(client->Reset()));
  render->Release();
  client->Release();
  device->Release();
  collection->Release();
  enumerator->Release();
  if (state) {
    state->alive = 0;
    UnmapViewOfFile(state);
  }
  if (mapping) {
    CloseHandle(mapping);
  }
  CoUninitialize();
  std::puts("DualSense audio COM/PCM/child-startup checks passed");
  return 0;
}
