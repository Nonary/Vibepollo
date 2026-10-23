#include "frame_push_windows_io.h"
#ifdef _WIN32
#include <Windows.h>
#include <map>

namespace platf::frame_push {
  namespace {
    class WindowsIo final: public AsyncIo, public std::enable_shared_from_this<WindowsIo> {
    public:
      struct Operation {
        OVERLAPPED overlapped {};
        std::shared_ptr<PendingTransaction> transaction;
        std::shared_ptr<WindowsIo> owner;
      };
      ~WindowsIo() override {
        // Every native operation holds this object until its completion
        // callback. No outstanding OVERLAPPED access remains at destruction.
        if (device_ != INVALID_HANDLE_VALUE) CloseHandle(device_);
        if (io_) CloseThreadpoolIo(io_);
      }
      bool open(const std::wstring &path) {
        device_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (device_ == INVALID_HANDLE_VALUE) return false;
        io_ = CreateThreadpoolIo(device_, completed, this, nullptr);
        return io_ != nullptr;
      }
      bool begin(std::shared_ptr<PendingTransaction> transaction, std::uint32_t &error) override {
        auto operation = std::make_unique<Operation>();
        operation->transaction = std::move(transaction); operation->owner = shared_from_this();
        auto *raw = operation.get();
        {
          std::lock_guard lock {mutex_};
          if (!operations_.empty()) { error = ERROR_BUSY; return false; }
          operations_.emplace(raw->transaction.get(), raw);
        }
        StartThreadpoolIo(io_);
        const BOOL ok = DeviceIoControl(device_, raw->transaction->code,
          raw->transaction->input.empty() ? nullptr : raw->transaction->input.data(),
          static_cast<DWORD>(raw->transaction->input.size()),
          raw->transaction->output.empty() ? nullptr : raw->transaction->output.data(),
          static_cast<DWORD>(raw->transaction->output.size()), nullptr, &raw->overlapped);
        const auto native_error = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && native_error != ERROR_IO_PENDING) {
          CancelThreadpoolIo(io_);
          { std::lock_guard lock {mutex_}; operations_.erase(raw->transaction.get()); }
          error = native_error; return false;
        }
        // No FILE_SKIP_COMPLETION_PORT_ON_SUCCESS mode is enabled. The callback
        // owns this allocation for both synchronous success and ERROR_IO_PENDING.
        operation.release(); return true;
      }
      void cancel(const std::shared_ptr<PendingTransaction> &transaction) override {
        std::lock_guard lock {mutex_};
        const auto i = operations_.find(transaction.get());
        if (i != operations_.end()) (void) CancelIoEx(device_, &i->second->overlapped);
        // Never wait for a potentially stuck driver here. Eight global pending
        // permits bound retained operations across destroyed/recreated endpoints.
      }
    private:
      static void CALLBACK completed(PTP_CALLBACK_INSTANCE, void *context, void *overlapped,
                                     ULONG result, ULONG_PTR bytes, PTP_IO) {
        auto *owner = static_cast<WindowsIo *>(context);
        std::unique_ptr<Operation> operation;
        {
          std::lock_guard lock {owner->mutex_};
          for (auto i = owner->operations_.begin(); i != owner->operations_.end(); ++i) {
            if (&i->second->overlapped == overlapped) {
              operation.reset(i->second); owner->operations_.erase(i); break;
            }
          }
        }
        if (!operation) return;
        operation->transaction->complete(result, static_cast<std::size_t>(bytes));
      }
      HANDLE device_ {INVALID_HANDLE_VALUE};
      PTP_IO io_ {};
      std::mutex mutex_;
      std::map<PendingTransaction *, Operation *> operations_;
    };
  }
  std::shared_ptr<AsyncIo> open_frame_push_windows_io(const std::wstring &device_path) {
    auto io = std::make_shared<WindowsIo>();
    return io->open(device_path) ? io : nullptr;
  }
}
#endif
