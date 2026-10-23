// SPDX-License-Identifier: GPL-3.0-or-later

// winsock2 must precede every header that includes windows.h.
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
// clang-format on
#include "dualsense_usbip_transport.h"

#include "usbip_win2_abi.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <audioclient.h>
#include <cfgmgr32.h>
#include <cstring>
#include <cwchar>
#include <limits>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace platf::dualsense_usbip {
  namespace {
    using namespace std::chrono_literals;

    // The published detach API addresses a reusable port number. Serialize
    // our attach/removal and socket-close paths to prevent our controllers
    // from reusing a port between its identity query and its detach.
    std::mutex driver_lifecycle_mutex;

    struct handle {
      HANDLE value = INVALID_HANDLE_VALUE;

      explicit handle(HANDLE v = INVALID_HANDLE_VALUE):
          value(v) {}

      ~handle() {
        if (value != INVALID_HANDLE_VALUE && value) {
          CloseHandle(value);
        }
      }

      handle(const handle &) = delete;
      handle &operator=(const handle &) = delete;

      explicit operator bool() const {
        return value && value != INVALID_HANDLE_VALUE;
      }
    };

    template<class T>
    struct release_com {
      void operator()(T *p) const {
        if (p) {
          p->Release();
        }
      }
    };

    template<class T>
    using com_ptr = std::unique_ptr<T, release_com<T>>;

    struct com_scope {
      HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

      ~com_scope() {
        if (SUCCEEDED(result)) {
          CoUninitialize();
        }
      }
    };

    // CM's list can change while it is being sized. Retry only a bounded
    // number of times, and open just the public released VHCI interface.
    constexpr DEVPROPKEY instance_devkey {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};

    HANDLE open_driver(DEVINST *node = nullptr) {
      auto guid = driver_abi::interface_guid;
      for (unsigned attempt = 0; attempt < 4; ++attempt) {
        ULONG count = 0;
        if (CM_Get_Device_Interface_List_SizeW(&count, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || count < 2 || count > 65536) {
          return INVALID_HANDLE_VALUE;
        }
        std::vector<wchar_t> paths(count);
        const auto result = CM_Get_Device_Interface_ListW(&guid, nullptr, paths.data(), count, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result == CR_BUFFER_SMALL) {
          continue;
        }
        if (result != CR_SUCCESS) {
          return INVALID_HANDLE_VALUE;
        }
        for (std::size_t offset = 0; offset + 1 < paths.size() && paths[offset];) {
          const auto end = std::find(paths.begin() + offset, paths.end(), L'\0');
          if (end == paths.end()) {
            return INVALID_HANDLE_VALUE;
          }
          const auto device = CreateFileW(paths.data() + offset, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
          if (device != INVALID_HANDLE_VALUE) {
            if (node) {
              std::array<wchar_t, MAX_DEVICE_ID_LEN> instance {};
              DEVPROPTYPE type = 0;
              ULONG size = sizeof(instance);
              if (CM_Get_Device_Interface_PropertyW(paths.data() + offset, &instance_devkey, &type, reinterpret_cast<BYTE *>(instance.data()), &size, 0) != CR_SUCCESS || type != DEVPROP_TYPE_STRING || CM_Locate_DevNodeW(node, instance.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
                CloseHandle(device);
                offset = static_cast<std::size_t>(end - paths.begin()) + 1;
                continue;
              }
            }
            return device;
          }
          offset = static_cast<std::size_t>(end - paths.begin()) + 1;
        }
        return INVALID_HANDLE_VALUE;
      }
      return INVALID_HANDLE_VALUE;
    }

    struct ioctl_request {
      handle device;
      handle event {CreateEventW(nullptr, TRUE, FALSE, nullptr)};
      OVERLAPPED overlap {};
      std::vector<std::uint8_t> input;
      std::vector<std::uint8_t> output;
      std::function<void(bool, const std::vector<std::uint8_t> &, DWORD, DWORD)> late_completion;

      ioctl_request() {
        overlap.hEvent = event.value;
      }
    };

    // An overlapped IOCTL lets attach time out without blocking stream
    // control indefinitely. Cancellation buffers must stay alive until the
    // kernel has completed the request, even if cancellation is delayed.
    bool device_io(HANDLE device, DWORD code, std::span<const std::uint8_t> input, std::span<std::uint8_t> output, DWORD &returned, std::chrono::milliseconds timeout, std::function<void(bool, const std::vector<std::uint8_t> &, DWORD, DWORD)> late_completion = {}, bool cancel_on_timeout = true) {
      auto request = std::make_shared<ioctl_request>();
      request->late_completion = std::move(late_completion);
      if (!request->event || !DuplicateHandle(GetCurrentProcess(), device, GetCurrentProcess(), &request->device.value, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return false;
      }
      request->input.assign(input.begin(), input.end());
      request->output.resize(output.size());
      returned = 0;
      const auto immediate = DeviceIoControl(request->device.value, code, request->input.data(), static_cast<DWORD>(request->input.size()), request->output.data(), static_cast<DWORD>(request->output.size()), &returned, &request->overlap);
      if (!immediate) {
        if (GetLastError() != ERROR_IO_PENDING) {
          return false;
        }
        if (WaitForSingleObject(request->event.value, static_cast<DWORD>(timeout.count())) != WAIT_OBJECT_0) {
          if (cancel_on_timeout) {
            CancelIoEx(request->device.value, &request->overlap);
          }
          if (!cancel_on_timeout || WaitForSingleObject(request->event.value, 2000) != WAIT_OBJECT_0) {
            std::thread([request] {
              WaitForSingleObject(request->event.value, INFINITE);
              DWORD completed = 0;
              const auto succeeded = GetOverlappedResult(request->device.value, &request->overlap, &completed, FALSE);
              const auto error = succeeded ? ERROR_SUCCESS : GetLastError();
              if (request->late_completion) {
                request->late_completion(succeeded, request->output, completed, error);
              }
            }).detach();
          } else if (request->late_completion) {
            DWORD completed = 0;
            const auto succeeded = GetOverlappedResult(request->device.value, &request->overlap, &completed, FALSE);
            const auto error = succeeded ? ERROR_SUCCESS : GetLastError();
            request->late_completion(succeeded, request->output, completed, error);
          }
          SetLastError(ERROR_TIMEOUT);
          return false;
        }
        if (!GetOverlappedResult(request->device.value, &request->overlap, &returned, FALSE)) {
          return false;
        }
      }
      if (returned > output.size()) {
        SetLastError(ERROR_INVALID_DATA);
        return false;
      }
      std::copy_n(request->output.begin(), returned, output.begin());
      return true;
    }

    template<class T>
    auto read_bytes(const T &v) {
      return std::span<const std::uint8_t> {reinterpret_cast<const std::uint8_t *>(&v), sizeof(v)};
    }

    template<class T>
    auto write_bytes(T &v) {
      return std::span<std::uint8_t> {reinterpret_cast<std::uint8_t *>(&v), sizeof(v)};
    }

    void stop_exact_attach_attempts(HANDLE device, const std::string &service, const std::string &busid) {
      driver_abi::stop_attach_attempts request {};
      request.size = sizeof(request);
      constexpr std::string_view host = "127.0.0.1";
      std::copy(host.begin(), host.end(), request.host);
      std::copy(service.begin(), service.end(), request.service);
      std::copy(busid.begin(), busid.end(), request.busid);
      DWORD returned = 0;
      device_io(device, driver_abi::stop_attach_attempts_ioctl, read_bytes(request), write_bytes(request), returned, 1s);
    }

    bool compatible_driver(HANDLE device) {
      driver_abi::get_imported_devices query {};
      query.size = sizeof(query);
      // The released driver rejects the size header before interpreting its
      // fields. GET_IMPORTED_DEVICES is a read-only probe of this exact ABI.
      std::vector<std::uint8_t> devices(driver_abi::imported_devices_offset + 255 * sizeof(driver_abi::imported_device));
      DWORD returned = 0;
      return device_io(device, driver_abi::get_imported_devices_ioctl, read_bytes(query.size), devices, returned, 1s) &&
             returned >= driver_abi::imported_devices_offset && (returned - driver_abi::imported_devices_offset) % sizeof(driver_abi::imported_device) == 0;
    }

    template<std::size_t N>
    void copy_string(char (&target)[N], const std::string &source) {
      // All callers supply short generated ASCII strings, never user input.
      std::copy_n(source.data(), std::min(source.size(), N - 1), target);
    }

    template<std::size_t N>
    bool equals(const char (&value)[N], const std::string &expected) {
      return expected.size() < N && std::equal(expected.begin(), expected.end(), value) && value[expected.size()] == '\0';
    }

    std::string make_serial(std::uint8_t slot) {
      constexpr char digits[] = "0123456789ABCDEF";
      std::random_device random;
      const auto identity = (std::uint64_t {random()} << 32) | random();
      std::string serial = "VP";
      for (unsigned i = 0; i < 12; ++i) {
        serial.push_back(digits[(identity >> (4 * i)) & 0xF]);
      }
      serial.push_back(digits[slot]);
      return serial;
    }

    // The released UDE driver can expose a port-generated instance ID even
    // when its attach request includes a serial. Find our port under the exact
    // host controller we opened; matching VID/PID alone could select a physical
    // DualSense or another session's controller. USB/IP's returned port is the
    // Usb20PortNumber/Usb30PortNumber supplied to UdecxUsbDevicePlugIn.
    bool controller_root(DEVINST host, int port, DEVINST &root) {
      if (port <= 0) {
        return false;
      }
      DEVINST hub = 0;
      if (CM_Get_Child(&hub, host, 0) != CR_SUCCESS) {
        return false;
      }
      for (unsigned hubs = 0; hubs < 32; ++hubs) {
        DEVINST device = 0;
        if (CM_Get_Child(&device, hub, 0) == CR_SUCCESS) {
          for (unsigned devices = 0; devices < 256; ++devices) {
            std::array<wchar_t, MAX_DEVICE_ID_LEN> instance {};
            ULONG address = 0;
            ULONG type = 0;
            ULONG size = sizeof(address);
            constexpr std::wstring_view prefix = L"USB\\VID_054C&PID_0CE6\\";
            if (CM_Get_Device_IDW(device, instance.data(), instance.size(), 0) == CR_SUCCESS && _wcsnicmp(instance.data(), prefix.data(), prefix.size()) == 0 && CM_Get_DevNode_Registry_PropertyW(device, CM_DRP_ADDRESS, &type, &address, &size, 0) == CR_SUCCESS && type == REG_DWORD && size == sizeof(address) && address == static_cast<ULONG>(port)) {
              root = device;
              return true;
            }
            if (CM_Get_Sibling(&device, device, 0) != CR_SUCCESS) {
              break;
            }
          }
        }
        if (CM_Get_Sibling(&hub, hub, 0) != CR_SUCCESS) {
          break;
        }
      }
      return false;
    }

    // A present and started HID child proves hidusb/hidclass finished loading.
    bool hid_started(DEVINST root) {
      std::vector<std::pair<DEVINST, unsigned>> todo {{root, 0}};
      for (unsigned inspected = 0; !todo.empty() && inspected < 64; ++inspected) {
        const auto [node, depth] = todo.back();
        todo.pop_back();
        std::array<wchar_t, 64> class_name {};
        ULONG type = 0;
        ULONG size = sizeof(class_name);
        ULONG status = 0;
        ULONG problem = 0;
        if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_CLASS, &type, class_name.data(), &size, 0) == CR_SUCCESS && _wcsicmp(class_name.data(), L"HIDClass") == 0 && CM_Get_DevNode_Status(&status, &problem, node, 0) == CR_SUCCESS && (status & DN_STARTED) && !problem) {
          return true;
        }
        DEVINST child = 0;
        if (depth < 8 && CM_Get_Child(&child, node, 0) == CR_SUCCESS) {
          for (unsigned siblings = 0; siblings < 32; ++siblings) {
            todo.emplace_back(child, depth + 1);
            if (CM_Get_Sibling(&child, child, 0) != CR_SUCCESS) {
              break;
            }
          }
        }
      }
      return false;
    }

    // Audio endpoints use software devnodes, so match their physical device
    // container GUID rather than relying on an endpoint's parent devnode.
    constexpr GUID container_key_guid {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}};
    constexpr DEVPROPKEY container_devkey {container_key_guid, 2};
    constexpr PROPERTYKEY container_property {container_key_guid, 2};

    bool render_endpoint_ready(IMMDeviceEnumerator *enumerator, const GUID &container) {
      IMMDeviceCollection *raw_collection = nullptr;
      if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &raw_collection))) {
        return false;
      }
      com_ptr<IMMDeviceCollection> collection(raw_collection);
      UINT count = 0;
      if (FAILED(collection->GetCount(&count))) {
        return false;
      }
      for (UINT index = 0; index < std::min(count, UINT {1024}); ++index) {
        IMMDevice *raw_device = nullptr;
        if (FAILED(collection->Item(index, &raw_device))) {
          continue;
        }
        com_ptr<IMMDevice> device(raw_device);
        IPropertyStore *raw_properties = nullptr;
        if (FAILED(device->OpenPropertyStore(STGM_READ, &raw_properties))) {
          continue;
        }
        com_ptr<IPropertyStore> properties(raw_properties);
        PROPVARIANT value {};
        const auto property_ok = properties->GetValue(container_property, &value);
        const auto matches = SUCCEEDED(property_ok) && value.vt == VT_CLSID && value.puuid && IsEqualGUID(*value.puuid, container);
        PropVariantClear(&value);
        if (!matches) {
          continue;
        }
        IAudioClient *raw_client = nullptr;
        if (FAILED(device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&raw_client)))) {
          continue;
        }
        com_ptr<IAudioClient> client(raw_client);
        WAVEFORMATEX *format = nullptr;
        const auto format_ok = client->GetMixFormat(&format);
        const auto ready = SUCCEEDED(format_ok) && format && format->nChannels == 4 && format->nSamplesPerSec == 48000;
        CoTaskMemFree(format);
        if (ready) {
          return true;
        }
      }
      return false;
    }
  }  // namespace

  struct controller::impl: std::enable_shared_from_this<controller::impl> {
    session protocol;
    const std::string busid;
    const std::string serial;
    std::string service;
    DEVINST host_node = 0;
    handle driver;
    SOCKET listener = INVALID_SOCKET;
    std::thread worker;
    std::atomic_bool stop {false};
    std::atomic_bool imported {false};
    bool winsock = false;
    std::atomic_int imported_port {0};

    impl(std::uint8_t slot, callbacks handlers):
        protocol(slot, std::move(handlers)),
        busid(session::bus_id(slot)),
        serial(make_serial(slot)),
        driver(open_driver(&host_node)) {}

    ~impl() {
      {
        const std::lock_guard lifecycle(driver_lifecycle_mutex);
        stop_attach_attempts();
        detach_owned();
      }
      stop = true;
      if (worker.joinable()) {
        worker.join();
      }
      if (listener != INVALID_SOCKET) {
        closesocket(listener);
      }
      // Socket loss schedules retries even for PLUGIN_HARDWARE_ONCE.
      // Cancel just this controller's exact location after socket closure.
      {
        const std::lock_guard lifecycle(driver_lifecycle_mutex);
        detach_owned();
        detach_barrier();
        stop_attach_attempts();
      }
      if (winsock) {
        WSACleanup();
      }
    }

    void detach_barrier() {
      if (!driver || service.empty()) {
        return;
      }
      // Detach uses the sequential queue; retry cancellation uses a parallel
      // queue. An impossible positive port is rejected without touching any
      // device after earlier queued socket-loss detaches have finished. Never
      // use zero or a negative port: those request removal of all devices.
      driver_abi::plugout_hardware request {};
      request.size = sizeof(request);
      request.port = std::numeric_limits<int>::max();
      auto cleanup_device = std::make_shared<handle>();
      if (!DuplicateHandle(GetCurrentProcess(), driver.value, GetCurrentProcess(), &cleanup_device->value, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return;
      }
      auto stop_after_barrier = [cleanup_device, service = service, busid = busid](bool, const std::vector<std::uint8_t> &, DWORD, DWORD error) {
        if (error == ERROR_INVALID_PARAMETER) {
          stop_exact_attach_attempts(cleanup_device->value, service, busid);
        }
      };
      DWORD returned = 0;
      // Keep this harmless request pending if foreground teardown times out.
      // Cancellation while queued would not establish a barrier. Its retained
      // completion context runs exact-location STOP after actual dispatch.
      device_io(driver.value, driver_abi::plugout_hardware_ioctl, read_bytes(request), {}, returned, 2s, std::move(stop_after_barrier), false);
    }

    void stop_attach_attempts() {
      if (!driver || service.empty()) {
        return;
      }
      stop_exact_attach_attempts(driver.value, service, busid);
    }

    void detach_owned() {
      if (!driver || service.empty()) {
        return;
      }
      // A port number is reused after removal. Confirm this exact loopback
      // location still belongs to us before sending a port-based detach.
      driver_abi::get_imported_devices query {};
      query.size = sizeof(query);
      for (unsigned capacity = 4; capacity <= 1024; capacity *= 2) {
        std::vector<std::uint8_t> devices(driver_abi::imported_devices_offset + capacity * sizeof(driver_abi::imported_device));
        DWORD returned = 0;
        if (!device_io(driver.value, driver_abi::get_imported_devices_ioctl, read_bytes(query.size), devices, returned, 1s)) {
          if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
            continue;
          }
          return;
        }
        if (returned < driver_abi::imported_devices_offset || (returned - driver_abi::imported_devices_offset) % sizeof(driver_abi::imported_device)) {
          return;
        }
        for (unsigned offset = driver_abi::imported_devices_offset; offset + sizeof(driver_abi::imported_device) <= returned; offset += sizeof(driver_abi::imported_device)) {
          driver_abi::imported_device device {};
          std::memcpy(&device, devices.data() + offset, sizeof(device));
          if (device.port > 0 && equals(device.host, "127.0.0.1") && equals(device.service, service) && equals(device.busid, busid) && equals(device.serial, serial)) {
            driver_abi::plugout_hardware request {};
            request.size = sizeof(request);
            request.port = device.port;
            device_io(driver.value, driver_abi::plugout_hardware_ioctl, read_bytes(request), {}, returned, 2s);
            return;
          }
        }
        return;
      }
    }

    bool start() {
      if (!driver || !compatible_driver(driver.value)) {
        return false;
      }
      WSADATA data {};
      if (WSAStartup(MAKEWORD(2, 2), &data)) {
        return false;
      }
      winsock = true;
      listener = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
      if (listener == INVALID_SOCKET) {
        return false;
      }
      const BOOL exclusive = TRUE;
      if (setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive))) {
        return false;
      }
      sockaddr_in address {};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      address.sin_port = 0;
      if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) || listen(listener, 1)) {
        return false;
      }
      int size = sizeof(address);
      if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &size)) {
        return false;
      }
      service = std::to_string(ntohs(address.sin_port));
      worker = std::thread([this] {
        try {
          serve();
        } catch (...) {
          // A consumer callback cannot unwind out of a worker thread.
        }
        imported = false;
      });

      driver_abi::plugin_hardware request {};
      request.size = sizeof(request);
      copy_string(request.host, "127.0.0.1");
      copy_string(request.service, service);
      copy_string(request.busid, busid);
      copy_string(request.serial, serial);
      request.wsk_events = true;
      DWORD returned = 0;
      const std::lock_guard lifecycle(driver_lifecycle_mutex);
      // Keep the server and its exact identity alive if cancellation races a
      // successful attach. Its completion owner records the late port before
      // the last shared reference runs normal, owned-device teardown.
      auto late_attach = [owned = shared_from_this()](bool succeeded, const std::vector<std::uint8_t> &response, DWORD actual, DWORD) {
        if (succeeded && actual == driver_abi::attach_reply_size && response.size() >= actual) {
          int port = 0;
          std::memcpy(&port, response.data() + sizeof(ULONG), sizeof(port));
          if (port > 0) {
            owned->imported_port = port;
          }
        }
      };
      if (!device_io(driver.value, driver_abi::plugin_hardware_once, read_bytes(request), write_bytes(request).first(driver_abi::attach_reply_size), returned, 10s, std::move(late_attach)) || returned != driver_abi::attach_reply_size || request.port <= 0) {
        return false;
      }
      imported_port = request.port;
      return imported;
    }

    void serve() {
      SOCKET connection = INVALID_SOCKET;
      while (!stop && connection == INVALID_SOCKET) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        timeval timeout {0, 1000};
        const auto ready = select(0, &readable, nullptr, nullptr, &timeout);
        if (ready == SOCKET_ERROR) {
          return;
        }
        if (!ready) {
          continue;
        }
        sockaddr_in peer {};
        int size = sizeof(peer);
        connection = accept(listener, reinterpret_cast<sockaddr *>(&peer), &size);
        if (connection != INVALID_SOCKET && (peer.sin_family != AF_INET || peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK))) {
          closesocket(connection);
          connection = INVALID_SOCKET;
        }
      }
      if (connection == INVALID_SOCKET) {
        return;
      }

      // Keep socket closure guaranteed if a callback throws.
      struct close_socket {
        SOCKET value;
        std::atomic_bool &imported;

        ~close_socket() {
          const std::lock_guard lifecycle(driver_lifecycle_mutex);
          imported = false;
          shutdown(value, SD_BOTH);
          closesocket(value);
        }
      } close {connection, imported};

      u_long nonblocking = 1;
      const BOOL nodelay = TRUE;
      if (ioctlsocket(connection, FIONBIO, &nonblocking) || setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&nodelay), sizeof(nodelay))) {
        return;
      }
      const auto handshake_deadline = std::chrono::steady_clock::now() + 10s;
      std::vector<std::uint8_t> outgoing;
      std::size_t sent = 0;
      std::array<std::uint8_t, 8192> incoming {};
      while (!stop) {
        protocol.poll();
        auto generated = protocol.take_output();
        if (outgoing.size() - sent + generated.size() > 4 * 1024 * 1024) {
          return;
        }
        if (!generated.empty()) {
          outgoing.erase(outgoing.begin(), outgoing.begin() + sent);
          sent = 0;
          outgoing.insert(outgoing.end(), generated.begin(), generated.end());
        }
        if ((protocol.finished() || protocol.failed()) && sent == outgoing.size()) {
          return;
        }
        imported = protocol.imported() && !protocol.failed();
        if (!imported && std::chrono::steady_clock::now() >= handshake_deadline) {
          return;
        }
        fd_set readable;
        fd_set writable;
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        FD_SET(connection, &readable);
        if (sent < outgoing.size()) {
          FD_SET(connection, &writable);
        }
        timeval timeout {0, 1000};
        if (select(0, &readable, &writable, nullptr, &timeout) == SOCKET_ERROR) {
          return;
        }
        if (FD_ISSET(connection, &writable)) {
          const auto written = send(connection, reinterpret_cast<const char *>(outgoing.data() + sent), static_cast<int>(outgoing.size() - sent), 0);
          if (written == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
              return;
            }
          } else if (!written) {
            return;
          } else {
            sent += static_cast<std::size_t>(written);
          }
        }
        if (FD_ISSET(connection, &readable)) {
          // Bound work per iteration so one socket cannot prevent shutdown
          // or starve the poll that completes scheduled interrupt/iso URBs.
          for (unsigned reads = 0; reads < 8; ++reads) {
            const auto incarnation = protocol.capture_receive_incarnation();
            const auto received = recv(connection, reinterpret_cast<char *>(incoming.data()), static_cast<int>(incoming.size()), 0);
            if (received == SOCKET_ERROR) {
              if (WSAGetLastError() != WSAEWOULDBLOCK) {
                return;
              }
              break;
            }
            if (!received) {
              return;
            }
            if (!protocol.feed(std::span(incoming).first(received), incarnation)) {
              break;
            }
          }
        }
      }
    }
  };

  bool available() {
    const handle driver(open_driver());
    return driver && compatible_driver(driver.value);
  }

  controller::controller(std::shared_ptr<impl> state):
      state_(std::move(state)) {}

  controller::~controller() = default;

  bool controller::set_input_report(std::span<const std::uint8_t> report) {
    return state_->protocol.set_input_report(report);
  }

  bool controller::connected() const noexcept {
    return state_->imported;
  }

  bool controller::wait_until_ready(std::chrono::milliseconds timeout) {
    const com_scope com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) {
      return false;
    }
    IMMDeviceEnumerator *raw_enumerator = nullptr;
    if (FAILED(CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, reinterpret_cast<void **>(&raw_enumerator)))) {
      return false;
    }
    com_ptr<IMMDeviceEnumerator> enumerator(raw_enumerator);
    const auto deadline = std::chrono::steady_clock::now() + std::max(timeout, 0ms);
    do {
      if (!connected()) {
        return false;
      }
      DEVINST root = 0;
      GUID container {};
      DEVPROPTYPE type = 0;
      ULONG size = sizeof(container);
      if (controller_root(state_->host_node, state_->imported_port.load(), root) && hid_started(root) && CM_Get_DevNode_PropertyW(root, &container_devkey, &type, reinterpret_cast<BYTE *>(&container), &size, 0) == CR_SUCCESS && type == DEVPROP_TYPE_GUID && size == sizeof(container) && render_endpoint_ready(enumerator.get(), container)) {
        return true;
      }
      std::this_thread::sleep_for(25ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  std::unique_ptr<controller> create(std::uint8_t slot, callbacks handlers) {
    if (slot >= 16) {
      return {};
    }
    auto state = std::make_shared<controller::impl>(slot, std::move(handlers));
    if (!state->start()) {
      return {};
    }
    return std::unique_ptr<controller>(new controller(std::move(state)));
  }
}  // namespace platf::dualsense_usbip
