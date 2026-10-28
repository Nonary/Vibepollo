/**
 * @file src/nvenc/nvenc_d3d11.cpp
 * @brief Definitions for abstract Direct3D11 NVENC encoder.
 */
#ifdef _WIN32
  // Include the D3D11 platform header before logging can include Windows.h.
  #include "nvenc_d3d11.h"
  #include "nvenc_api.h"
  #include "nvenc_event_wait.h"
  #include "src/logging.h"

namespace nvenc {
  namespace {
    static_assert(sizeof(DWORD) == sizeof(std::uint32_t));
    static_assert(WAIT_OBJECT_0 == event_wait::signaled);
    static_assert(WAIT_TIMEOUT == event_wait::timed_out);
    static_assert(WAIT_FAILED == event_wait::failed);

    class win32_event_wait_t final: public event_wait::api_t {
    public:
      std::uint32_t wait(void *event, std::uint32_t timeout_ms) override {
        const DWORD result = WaitForSingleObject(event, timeout_ms);
        return result;
      }

      std::uint32_t last_error() override {
        return GetLastError();
      }
    };
  }  // namespace

  nvenc_d3d11::nvenc_d3d11(NV_ENC_DEVICE_TYPE device_type):
      nvenc_base(device_type) {
    async_event_handle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  }

  nvenc_d3d11::~nvenc_d3d11() {
    if (dll) {
      FreeLibrary(dll);
      dll = nullptr;
    }
    if (async_event_handle) {
      CloseHandle(async_event_handle);
    }
  }

  bool nvenc_d3d11::init_library(uint32_t api_version) {
    if (dll && nvenc && function_list_api_version == api_version) {
      return true;
    }

  #ifdef _WIN64
    constexpr auto dll_name = "nvEncodeAPI64.dll";
  #else
    constexpr auto dll_name = "nvEncodeAPI.dll";
  #endif

    if (!dll) {
      dll = LoadLibraryEx(dll_name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }

    if (dll) {
      // Query max supported version on first load (per NVIDIA programming guide)
      if (!max_driver_api_version) {
        if (auto get_max_ver = (decltype(NvEncodeAPIGetMaxSupportedVersion) *) GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion")) {
          uint32_t max_ver = 0;
          if (get_max_ver(&max_ver) == NV_ENC_SUCCESS) {
            max_driver_api_version = api::driver_max_to_api_version(max_ver);
            BOOST_LOG(info) << "NvEnc: driver supports up to API "
                            << api::version_string(max_driver_api_version)
                            << ", compiled SDK " << NVENCAPI_MAJOR_VERSION << "." << NVENCAPI_MINOR_VERSION;
          } else {
            BOOST_LOG(warning) << "NvEnc: NvEncodeAPIGetMaxSupportedVersion() failed";
          }
        }
      }

      // Reject API versions the driver can't support
      if (max_driver_api_version && !api::driver_supports_api_version(max_driver_api_version, api_version)) {
        last_nvenc_status = NV_ENC_ERR_INVALID_VERSION;
        last_nvenc_error_string = "driver max API " + api::version_string(max_driver_api_version);
        return false;
      }

      if (auto create_instance = (decltype(NvEncodeAPICreateInstance) *) GetProcAddress(dll, "NvEncodeAPICreateInstance")) {
        auto new_nvenc = std::make_unique<NV_ENCODE_API_FUNCTION_LIST>();
        new_nvenc->version = api::function_list_version(api_version);
        if (nvenc_failed(create_instance(new_nvenc.get()))) {
          if (last_nvenc_status == NV_ENC_ERR_INVALID_VERSION) {
            BOOST_LOG(debug) << "NvEnc: NvEncodeAPICreateInstance() rejected API " << api::version_string(api_version);
          } else {
            BOOST_LOG(error) << "NvEnc: NvEncodeAPICreateInstance() failed: " << last_nvenc_error_string;
          }
        } else {
          nvenc = std::move(new_nvenc);
          function_list_api_version = api_version;
          return true;
        }
      } else {
        BOOST_LOG(error) << "NvEnc: No NvEncodeAPICreateInstance() in " << dll_name;
      }
    } else {
      BOOST_LOG(debug) << "NvEnc: Couldn't load NvEnc library " << dll_name;
    }

    if (dll) {
      FreeLibrary(dll);
      dll = nullptr;
    }
    function_list_api_version = 0;

    return false;
  }

  bool nvenc_d3d11::wait_for_async_event(uint32_t timeout_ms) {
    win32_event_wait_t api;
    const auto result = event_wait::wait(async_event_handle, timeout_ms, api);
    if (!result.succeeded()) {
      BOOST_LOG(error) << "NvEnc: " << event_wait::describe(result, timeout_ms);
    }
    return result.succeeded();
  }

}  // namespace nvenc
#endif
