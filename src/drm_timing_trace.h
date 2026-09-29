/**
 * @file src/drm_timing_trace.h
 * @brief Buffered, tmpfs-backed diagnostic trace for Linux DRM video timing.
 */
#pragma once

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <vector>

namespace drm_timing_trace {
  inline constexpr auto TRACE_PATH = "/run/vibeshine/host/drm-timing.trace";
  inline constexpr auto PREVIOUS_TRACE_PATH = "/run/vibeshine/host/drm-timing.trace.previous";
  inline constexpr std::size_t MAX_TRACE_BYTES = 128U * 1024U * 1024U;

  class writer_t {
  public:
    writer_t():
        buffer_(1024 * 1024),
        stream_(&file_) {
      file_.pubsetbuf(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
      file_.open(TRACE_PATH, std::ios::out | std::ios::trunc);
      last_flush_ = std::chrono::steady_clock::now();
    }

    ~writer_t() {
      std::lock_guard lock(mutex_);
      stream_.flush();
    }

    [[nodiscard]] bool available() const {
      return file_.is_open();
    }

    void write(std::string_view record) {
      std::lock_guard lock(mutex_);
      if (!file_.is_open()) {
        return;
      }

      if (bytes_written_ + record.size() + 1 > MAX_TRACE_BYTES) {
        rotate();
        if (!file_.is_open()) {
          return;
        }
      }
      stream_ << record << '\n';
      bytes_written_ += record.size() + 1;
      ++records_since_flush_;
      const auto now = std::chrono::steady_clock::now();
      if (records_since_flush_ >= 1024 || now - last_flush_ >= std::chrono::seconds {1}) {
        stream_.flush();
        records_since_flush_ = 0;
        last_flush_ = now;
      }
    }

  private:
    void rotate() {
      stream_.flush();
      file_.close();
      std::remove(PREVIOUS_TRACE_PATH);
      std::rename(TRACE_PATH, PREVIOUS_TRACE_PATH);
      stream_.clear();
      file_.open(TRACE_PATH, std::ios::out | std::ios::trunc);
      bytes_written_ = 0;
      records_since_flush_ = 0;
      last_flush_ = std::chrono::steady_clock::now();
    }

    std::vector<char> buffer_;
    std::filebuf file_;
    std::ostream stream_;
    mutable std::mutex mutex_;
    std::size_t records_since_flush_ {};
    std::size_t bytes_written_ {};
    std::chrono::steady_clock::time_point last_flush_ {};
  };

  inline writer_t &writer() {
    static writer_t instance;
    return instance;
  }

  template<typename Render>
  void write(Render &&render) {
    std::ostringstream record;
    render(record);
    writer().write(record.str());
  }
}  // namespace drm_timing_trace
