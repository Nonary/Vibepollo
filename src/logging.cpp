/**
 * @file src/logging.cpp
 * @brief Definitions for logging related functions.
 */
#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <string_view>
#include <vector>

// lib includes
#include <boost/core/null_deleter.hpp>
#include <boost/format.hpp>
#include <boost/log/attributes/clock.hpp>
#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/sources/severity_logger.hpp>

// local includes
#include "logging.h"

// conditional includes
#ifdef __ANDROID__
  #include <android/log.h>
#else
  // Include libdisplaydevice logging when enabled for main target or tests
  #if defined(SUNSHINE_USE_DISPLAYDEVICE_LOGGING) || defined(SETUP_LIBDISPLAYDEVICE_LOGGING)
    #include <display_device/logging.h>
  #endif
#endif

#ifdef SETUP_AV_LOGGING
extern "C" {
  #include <libavutil/log.h>
}
#endif

using namespace std::literals;

namespace logging {
  namespace detail {
    constexpr std::size_t k_max_session_count = 10;
    constexpr std::size_t k_max_session_rollovers = 10;
    constexpr std::uintmax_t k_rollover_size_bytes = 2ull * 1024ull * 1024ull;
    inline std::filesystem::path log_root;
    inline std::filesystem::path current_log_file;
    inline std::string log_basename;
    inline std::string session_label;
    inline bool session_mode_enabled = false;
  }  // namespace detail
}  // namespace logging

namespace bl = boost::log;

namespace {
  enum class init_mode {
    session,
    single_file
  };

  constexpr std::string_view k_session_separator = "-";
  constexpr std::string_view k_log_suffix = ".log";

  std::filesystem::path resolve_log_root(const std::filesystem::path &configured_path) {
    namespace fs = std::filesystem;
    auto default_root_base = []() {
      std::error_code ec;
      auto cwd = std::filesystem::current_path(ec);
      if (!ec) {
        return cwd;
      }
      auto temp = std::filesystem::temp_directory_path(ec);
      if (!ec) {
        return temp;
      }
      return fs::path();
    };

    if (configured_path.empty()) {
      return default_root_base() / "logs";
    }
    std::error_code ec;
    if (fs::exists(configured_path, ec) && fs::is_directory(configured_path, ec)) {
      return configured_path;
    }
    if (configured_path.has_extension()) {
      auto parent = configured_path.parent_path();
      if (parent.empty()) {
        parent = default_root_base();
      }
      return parent / "logs";
    }
    return configured_path;
  }

  std::string derive_base_name(const std::filesystem::path &configured_path) {
    if (!configured_path.filename().empty()) {
      if (configured_path.has_extension()) {
        auto stem = configured_path.stem().string();
        if (!stem.empty()) {
          return stem;
        }
      } else {
        auto filename = configured_path.filename().string();
        if (!filename.empty()) {
          return filename;
        }
      }
    }
    return "sunshine";
  }

  std::string make_session_label(const std::string &base_name) {
    auto now = std::chrono::system_clock::now();
    auto tt = std::chrono::system_clock::to_time_t(now);
    auto local_tm = *std::localtime(&tt);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::ostringstream oss;
    oss << base_name << k_session_separator << std::put_time(&local_tm, "%Y%m%d-%H%M%S") << k_session_separator
        << std::setw(3) << std::setfill('0') << ms.count();
    return oss.str();
  }

  struct session_file_entry {
    std::string label;
    std::filesystem::path path;
  };

  std::optional<std::string> extract_session_label(const std::string &filename, const std::string &base_name) {
    const std::string prefix = base_name + std::string(k_session_separator);
    if (filename.rfind(prefix, 0) != 0) {
      return std::nullopt;
    }
    auto log_pos = filename.find(k_log_suffix);
    if (log_pos == std::string::npos || log_pos <= prefix.size()) {
      return std::nullopt;
    }
    return filename.substr(0, log_pos);
  }

  std::vector<session_file_entry> scan_session_entries(const std::filesystem::path &root, const std::string &base_name) {
    namespace fs = std::filesystem;
    std::vector<session_file_entry> entries;
    std::error_code ec;
    if (!fs::exists(root, ec)) {
      return entries;
    }
    for (fs::directory_iterator it(root, ec); it != fs::directory_iterator(); ++it) {
      if (ec) {
        break;
      }
      std::error_code file_ec;
      if (!it->is_regular_file(file_ec)) {
        continue;
      }
      auto label = extract_session_label(it->path().filename().string(), base_name);
      if (!label) {
        continue;
      }
      entries.push_back({*label, it->path()});
    }
    std::sort(entries.begin(), entries.end(), [](const session_file_entry &lhs, const session_file_entry &rhs) {
      if (lhs.label != rhs.label) {
        return lhs.label < rhs.label;
      }
      return lhs.path.filename().string() < rhs.path.filename().string();
    });
    return entries;
  }

  void purge_old_sessions(const std::filesystem::path &root, const std::string &base_name) {
    auto entries = scan_session_entries(root, base_name);
    if (entries.empty()) {
      return;
    }
    std::map<std::string, std::vector<std::filesystem::path>> sessions;
    for (const auto &entry : entries) {
      sessions[entry.label].push_back(entry.path);
    }
    while (sessions.size() >= logging::detail::k_max_session_count) {
      auto it = sessions.begin();
      for (const auto &path : it->second) {
        std::error_code remove_ec;
        std::filesystem::remove(path, remove_ec);
      }
      sessions.erase(it);
    }
  }

  class session_log_buffer: public std::streambuf {
  public:
    session_log_buffer(const std::filesystem::path &root, const std::string &label, std::uintmax_t threshold, std::size_t max_rollovers):
        root_(root),
        label_(label),
        current_log_path_(root_ / (label_ + std::string(k_log_suffix))),
        bytes_written_(0),
        threshold_(threshold),
        max_rollovers_(max_rollovers),
        rollover_counter_(0) {
      open_stream();
    }

    session_log_buffer(const session_log_buffer &) = delete;
    session_log_buffer &operator=(const session_log_buffer &) = delete;

    const std::filesystem::path &current_log_path() const {
      return current_log_path_;
    }

  protected:
    std::streamsize xsputn(const char *s, std::streamsize count) override {
      std::streamsize written = 0;
      while (written < count) {
        if (!ensure_stream()) {
          break;
        }
        if (threshold_ > 0 && bytes_written_ >= threshold_) {
          rotate_file();
          continue;
        }
        auto space_left = static_cast<std::streamsize>(threshold_ - bytes_written_);
        if (space_left == 0) {
          rotate_file();
          continue;
        }
        auto chunk = std::min<std::streamsize>(space_left, count - written);
        stream_->write(s + written, chunk);
        if (!stream_->good()) {
          break;
        }
        bytes_written_ += static_cast<std::size_t>(chunk);
        written += chunk;
      }
      return written;
    }

    int overflow(int ch) override {
      if (ch == traits_type::eof()) {
        return traits_type::not_eof(ch);
      }
      char c = traits_type::to_char_type(ch);
      std::streamsize result = xsputn(&c, 1);
      return result == 1 ? ch : traits_type::eof();
    }

    int sync() override {
      if (stream_ && stream_->is_open()) {
        stream_->flush();
        return stream_->good() ? 0 : -1;
      }
      return 0;
    }

  private:
    bool ensure_stream() {
      if (stream_ && stream_->is_open()) {
        return true;
      }
      return open_stream();
    }

    bool open_stream() {
      std::error_code ec;
      stream_ = std::make_unique<std::ofstream>(current_log_path_, std::ios::binary | std::ios::trunc);
      if (!stream_ || !stream_->is_open()) {
        stream_.reset();
        return false;
      }
      const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
      stream_->write(reinterpret_cast<const char *>(bom), sizeof(bom));
      stream_->flush();
      bytes_written_ = 0;
      return true;
    }

    void rotate_file() {
      if (!stream_) {
        return;
      }
      stream_->flush();
      stream_.reset();

      rollover_counter_ += 1;
      std::error_code remove_existing;
      auto rollover_name = label_ + std::string(k_log_suffix) + "." + std::to_string(rollover_counter_);
      auto rollover_path = root_ / rollover_name;
      std::filesystem::remove(rollover_path, remove_existing);
      std::error_code rename_ec;
      std::filesystem::rename(current_log_path_, rollover_path, rename_ec);
      if (rename_ec) {
        std::error_code copy_ec;
        std::filesystem::copy_file(current_log_path_, rollover_path, std::filesystem::copy_options::overwrite_existing, copy_ec);
        std::error_code remove_ec;
        std::filesystem::remove(current_log_path_, remove_ec);
      }
      rollovers_.push_back(rollover_path);
      if (rollovers_.size() > max_rollovers_) {
        std::error_code del_ec;
        std::filesystem::remove(rollovers_.front(), del_ec);
        rollovers_.pop_front();
      }
      bytes_written_ = 0;
      open_stream();
    }

    std::filesystem::path root_;
    std::string label_;
    std::filesystem::path current_log_path_;
    std::unique_ptr<std::ofstream> stream_;
    std::size_t bytes_written_;
    std::uintmax_t threshold_;
    std::size_t max_rollovers_;
    std::deque<std::filesystem::path> rollovers_;
    std::uint64_t rollover_counter_;
  };

  class session_log_stream: public std::ostream {
  public:
    session_log_stream(const std::filesystem::path &root, const std::string &label):
        std::ostream(nullptr),
        buffer_(root, label, logging::detail::k_rollover_size_bytes, logging::detail::k_max_session_rollovers) {
      rdbuf(&buffer_);
    }

    const std::filesystem::path &current_log_path() const {
      return buffer_.current_log_path();
    }

  private:
    session_log_buffer buffer_;
  };

  boost::shared_ptr<std::ostream> create_single_file_stream(const std::string &log_file) {
    auto file_stream = boost::make_shared<std::ofstream>(log_file, std::ios::binary | std::ios::trunc);
    if (!file_stream || !file_stream->is_open()) {
      return nullptr;
    }
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    file_stream->write(reinterpret_cast<const char *>(bom), sizeof(bom));
    file_stream->flush();
    return file_stream;
  }

  boost::shared_ptr<std::ostream> create_session_stream(const std::string &log_file) {
    const std::filesystem::path configured_path(log_file);
    const auto root = resolve_log_root(configured_path);
    const auto base_name = derive_base_name(configured_path);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    purge_old_sessions(root, base_name);

    const auto label = make_session_label(base_name);
    logging::detail::log_root = root;
    logging::detail::log_basename = base_name;
    logging::detail::session_label = label;
    logging::detail::session_mode_enabled = true;

    auto session_stream = boost::make_shared<session_log_stream>(root, label);
    logging::detail::current_log_file = session_stream->current_log_path();

    return session_stream;
  }
}  // namespace

boost::shared_ptr<boost::log::sinks::asynchronous_sink<boost::log::sinks::text_ostream_backend>> sink;

bl::sources::severity_logger<int> verbose(0);  // Dominating output
bl::sources::severity_logger<int> debug(1);  // Follow what is happening
bl::sources::severity_logger<int> info(2);  // Should be informed about
bl::sources::severity_logger<int> warning(3);  // Strange events
bl::sources::severity_logger<int> error(4);  // Recoverable errors
bl::sources::severity_logger<int> fatal(5);  // Unrecoverable errors
#ifdef SUNSHINE_TESTS
bl::sources::severity_logger<int> tests(10);  // Automatic tests output
#endif

// severity keyword is declared in logging.h

namespace logging {
  deinit_t::~deinit_t() {
    deinit();
  }

  void deinit() {
    log_flush();
    bl::core::get()->remove_sink(sink);
    sink.reset();
  }

  void formatter(const boost::log::record_view &view, boost::log::formatting_ostream &os) {
    constexpr const char *message = "Message";
    constexpr const char *severity = "Severity";

    auto log_level = view.attribute_values()[severity].extract<int>().get();

    std::string_view log_type;
    switch (log_level) {
      case 0:
        log_type = "Verbose: "sv;
        break;
      case 1:
        log_type = "Debug: "sv;
        break;
      case 2:
        log_type = "Info: "sv;
        break;
      case 3:
        log_type = "Warning: "sv;
        break;
      case 4:
        log_type = "Error: "sv;
        break;
      case 5:
        log_type = "Fatal: "sv;
        break;
#ifdef SUNSHINE_TESTS
      case 10:
        log_type = "Tests: "sv;
        break;
#endif
    };

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - std::chrono::time_point_cast<std::chrono::seconds>(now)
    );

    auto t = std::chrono::system_clock::to_time_t(now);
    auto lt = *std::localtime(&t);

    os << "["sv << std::put_time(&lt, "%Y-%m-%d %H:%M:%S.") << boost::format("%03u") % ms.count() << "]: "sv
       << log_type << view.attribute_values()[message].extract<std::string>();
  }
#ifdef __ANDROID__
  namespace sinks = boost::log::sinks;
  namespace expr = boost::log::expressions;

  void android_log(const std::string &message, int severity) {
    android_LogPriority android_priority;
    switch (severity) {
      case 0:
        android_priority = ANDROID_LOG_VERBOSE;
        break;
      case 1:
        android_priority = ANDROID_LOG_DEBUG;
        break;
      case 2:
        android_priority = ANDROID_LOG_INFO;
        break;
      case 3:
        android_priority = ANDROID_LOG_WARN;
        break;
      case 4:
        android_priority = ANDROID_LOG_ERROR;
        break;
      case 5:
        android_priority = ANDROID_LOG_FATAL;
        break;
      default:
        android_priority = ANDROID_LOG_UNKNOWN;
        break;
    }
    __android_log_print(android_priority, "Sunshine", "%s", message.c_str());
  }

  // custom sink backend for android
  struct android_sink_backend: public sinks::basic_sink_backend<sinks::concurrent_feeding> {
    void consume(const bl::record_view &rec) {
      int log_sev = rec[severity].get();
      const std::string log_msg = rec[expr::smessage].get();
      // log to android
      android_log(log_msg, log_sev);
    }
  };
#endif

  [[nodiscard]] std::unique_ptr<deinit_t> init_internal(int min_log_level, const std::string &log_file, init_mode mode) {
    if (sink) {
      // Deinitialize the logging system before reinitializing it. This can probably only ever be hit in tests.
      deinit();
    }

#ifndef __ANDROID__
  #if defined(SUNSHINE_USE_DISPLAYDEVICE_LOGGING)
    setup_av_logging(min_log_level);
  #endif
  #if defined(SUNSHINE_USE_DISPLAYDEVICE_LOGGING) || defined(SETUP_LIBDISPLAYDEVICE_LOGGING)
    setup_libdisplaydevice_logging(min_log_level);
  #endif
#endif

    sink = boost::make_shared<text_sink>();

#ifndef SUNSHINE_TESTS
    boost::shared_ptr<std::ostream> console_stream {&std::cout, boost::null_deleter()};
    sink->locked_backend()->add_stream(console_stream);
#endif

    boost::shared_ptr<std::ostream> file_stream;
    if (mode == init_mode::single_file) {
      const std::filesystem::path log_path(log_file);
      logging::detail::session_mode_enabled = false;
      logging::detail::log_root = log_path.parent_path();
      logging::detail::log_basename = derive_base_name(log_path);
      logging::detail::session_label.clear();
      logging::detail::current_log_file = log_path;
      file_stream = create_single_file_stream(log_file);
    } else {
      file_stream = create_session_stream(log_file);
    }

    if (file_stream) {
      sink->locked_backend()->add_stream(file_stream);
    }

    sink->set_filter(severity >= min_log_level);
    sink->set_formatter(&formatter);

    // Flush after each log record to ensure log file contents on disk isn't stale.
    // This is particularly important when running from a Windows service.
    sink->locked_backend()->auto_flush(true);

    bl::core::get()->add_sink(sink);

#ifdef __ANDROID__
    auto android_sink = boost::make_shared<sinks::synchronous_sink<android_sink_backend>>();
    bl::core::get()->add_sink(android_sink);
#endif
    return std::make_unique<deinit_t>();
  }

  [[nodiscard]] std::unique_ptr<deinit_t> init(int min_log_level, const std::string &log_file) {
    return init_internal(min_log_level, log_file, init_mode::session);
  }

  [[nodiscard]] std::unique_ptr<deinit_t> init(int min_log_level, const char *log_file) {
    return init(min_log_level, std::string(log_file));
  }

  [[nodiscard]] std::unique_ptr<deinit_t> init(int min_log_level, const std::filesystem::path &log_file) {
    return init(min_log_level, log_file.string());
  }

  [[nodiscard]] std::unique_ptr<deinit_t> init_single_file(int min_log_level, const std::string &log_file) {
    return init_internal(min_log_level, log_file, init_mode::single_file);
  }

  [[nodiscard]] std::unique_ptr<deinit_t> init_single_file(int min_log_level, const char *log_file) {
    return init_single_file(min_log_level, std::string(log_file));
  }

  [[nodiscard]] std::unique_ptr<deinit_t> init_single_file(int min_log_level, const std::filesystem::path &log_file) {
    return init_single_file(min_log_level, log_file.string());
  }

  [[nodiscard]] std::unique_ptr<deinit_t> init_append(int min_log_level, const std::string &log_file) {
    if (sink) {
      deinit();
    }

#ifndef __ANDROID__
  #if defined(SUNSHINE_USE_DISPLAYDEVICE_LOGGING)
    setup_av_logging(min_log_level);
  #endif
  #if defined(SUNSHINE_USE_DISPLAYDEVICE_LOGGING) || defined(SETUP_LIBDISPLAYDEVICE_LOGGING)
    setup_libdisplaydevice_logging(min_log_level);
  #endif
#endif

    sink = boost::make_shared<text_sink>();

#ifndef SUNSHINE_TESTS
    boost::shared_ptr<std::ostream> stream {&std::cout, boost::null_deleter()};
    sink->locked_backend()->add_stream(stream);
#endif

    // Open in append mode to avoid cross-process truncation races. If the file is empty
    // (newly created), write a UTF-8 BOM once to aid detection in Notepad.
    bool should_write_bom = false;
    try {
      namespace fs = std::filesystem;
      std::error_code ec;
      auto sz = fs::exists(log_file, ec) ? fs::file_size(log_file, ec) : 0ull;
      if (ec) {
        sz = 0ull;
      }
      should_write_bom = (sz == 0ull);
    } catch (...) {
      should_write_bom = true;  // best-effort
    }

    auto file_stream = boost::make_shared<std::ofstream>(log_file, std::ios::binary | std::ios::app);
    if (file_stream->is_open() && should_write_bom) {
      const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
      file_stream->write(reinterpret_cast<const char *>(bom), 3);
      file_stream->flush();
    }
    sink->locked_backend()->add_stream(file_stream);

    sink->set_filter(severity >= min_log_level);
    sink->set_formatter(&formatter);
    sink->locked_backend()->auto_flush(true);
    bl::core::get()->add_sink(sink);
    return std::make_unique<deinit_t>();
  }

  // Overloads delegating to string version to support const char* and std::filesystem::path
  [[nodiscard]] std::unique_ptr<deinit_t> init_append(int min_log_level, const char *log_file) {
    return init_append(min_log_level, std::string(log_file));
  }

  [[nodiscard]] std::unique_ptr<deinit_t> init_append(int min_log_level, const std::filesystem::path &log_file) {
    // Delegate to string overload; std::ofstream accepts narrow UTF-8 on Windows 10+ when using .string()
    return init_append(min_log_level, log_file.string());
  }
#ifdef SETUP_AV_LOGGING
  void setup_av_logging(int min_log_level) {
    if (min_log_level >= 1) {
      av_log_set_level(AV_LOG_QUIET);
    } else {
      av_log_set_level(AV_LOG_DEBUG);
    }
    av_log_set_callback([](void *ptr, int level, const char *fmt, va_list vl) {
      static int print_prefix = 1;
      char buffer[1024];

      av_log_format_line(ptr, level, fmt, vl, buffer, sizeof(buffer), &print_prefix);
      if (level <= AV_LOG_ERROR) {
        // We print AV_LOG_FATAL at the error level. FFmpeg prints things as fatal that
        // are expected in some cases, such as lack of codec support or similar things.
        BOOST_LOG(error) << buffer;
      } else if (level <= AV_LOG_WARNING) {
        BOOST_LOG(warning) << buffer;
      } else if (level <= AV_LOG_INFO) {
        BOOST_LOG(info) << buffer;
      } else if (level <= AV_LOG_VERBOSE) {
        // AV_LOG_VERBOSE is less verbose than AV_LOG_DEBUG
        BOOST_LOG(debug) << buffer;
      } else {
        BOOST_LOG(verbose) << buffer;
      }
    });
  }
#else
  void setup_av_logging(int) {
    // no-op
  }
#endif
#ifdef SETUP_LIBDISPLAYDEVICE_LOGGING
  void setup_libdisplaydevice_logging(int min_log_level) {
    constexpr int min_level {static_cast<int>(display_device::Logger::LogLevel::verbose)};
    constexpr int max_level {static_cast<int>(display_device::Logger::LogLevel::fatal)};
    const auto log_level {static_cast<display_device::Logger::LogLevel>(std::min(std::max(min_level, min_log_level), max_level))};

    display_device::Logger::get().setLogLevel(log_level);
    display_device::Logger::get().setCustomCallback([](const display_device::Logger::LogLevel level, const std::string &message) {
      switch (level) {
        case display_device::Logger::LogLevel::verbose:
          BOOST_LOG(verbose) << message;
          break;
        case display_device::Logger::LogLevel::debug:
          BOOST_LOG(debug) << message;
          break;
        case display_device::Logger::LogLevel::info:
          BOOST_LOG(info) << message;
          break;
        case display_device::Logger::LogLevel::warning:
          BOOST_LOG(warning) << message;
          break;
        case display_device::Logger::LogLevel::error:
          BOOST_LOG(error) << message;
          break;
        case display_device::Logger::LogLevel::fatal:
          BOOST_LOG(fatal) << message;
          break;
      }
    });
  }
#else
  void setup_libdisplaydevice_logging(int) {
    // no-op
  }
#endif

  void reconfigure_min_log_level(int min_log_level) {
// Reconfigure external logging subsystems first so their callbacks
// respect the new level immediately.
#if defined(SUNSHINE_USE_DISPLAYDEVICE_LOGGING)
    setup_av_logging(min_log_level);
#endif
#if defined(SUNSHINE_USE_DISPLAYDEVICE_LOGGING) || defined(SETUP_LIBDISPLAYDEVICE_LOGGING)
    setup_libdisplaydevice_logging(min_log_level);
#endif

    // If we have an existing sink, update its filter to the new level.
    if (sink) {
      sink->set_filter(severity >= min_log_level);
    }
  }

  void log_flush() {
    if (sink) {
      sink->flush();
    }
  }

  std::filesystem::path current_log_file() {
    return detail::current_log_file;
  }

  std::filesystem::path log_directory() {
    return detail::log_root;
  }

  std::vector<std::filesystem::path> recent_session_logs(std::size_t max_sessions) {
    if (!detail::session_mode_enabled) {
      if (detail::current_log_file.empty()) {
        return {};
      }
      return {detail::current_log_file};
    }
    max_sessions = std::min(max_sessions, detail::k_max_session_count);
    auto entries = scan_session_entries(detail::log_root, detail::log_basename);
    if (entries.empty()) {
      return {};
    }
    std::map<std::string, std::vector<std::filesystem::path>> sessions;
    for (const auto &entry : entries) {
      sessions[entry.label].push_back(entry.path);
    }
    std::vector<std::string> labels;
    labels.reserve(sessions.size());
    for (const auto &pair : sessions) {
      labels.push_back(pair.first);
    }
    if (labels.size() > max_sessions) {
      labels.erase(labels.begin(), labels.begin() + (labels.size() - max_sessions));
    }
    std::vector<std::filesystem::path> result;
    result.reserve(entries.size());
    for (const auto &label : labels) {
      auto it = sessions.find(label);
      if (it == sessions.end()) {
        continue;
      }
      auto &paths = it->second;
      std::sort(paths.begin(), paths.end(), [](const auto &a, const auto &b) {
        return a.filename().string() < b.filename().string();
      });
      for (const auto &path : paths) {
        result.push_back(path);
      }
    }
    return result;
  }

  std::optional<std::filesystem::path> session_log_directory() {
    if (!detail::session_mode_enabled) {
      return std::nullopt;
    }
    if (detail::log_root.empty()) {
      return std::nullopt;
    }
    return detail::log_root;
  }

  void print_help(const char *name) {
    std::cout
      << "Usage: "sv << name << " [options] [/path/to/configuration_file] [--cmd]"sv << std::endl
      << "    Any configurable option can be overwritten with: \"name=value\""sv << std::endl
      << std::endl
      << "    Note: The configuration will be created if it doesn't exist."sv << std::endl
      << std::endl
      << "    --help                    | print help"sv << std::endl
      << "    --creds username password | set user credentials for the Web manager"sv << std::endl
      << "    --version                 | print the version of sunshine"sv << std::endl
      << std::endl
      << "    flags"sv << std::endl
      << "        -0 | Read PIN from stdin"sv << std::endl
      << "        -1 | Do not load previously saved state and do retain any state after shutdown"sv << std::endl
      << "           | Effectively starting as if for the first time without overwriting any pairings with your devices"sv << std::endl
      << "        -2 | Force replacement of headers in video stream"sv << std::endl
      << "        -p | Enable/Disable UPnP"sv << std::endl
      << std::endl;
  }

  std::string bracket(const std::string &input) {
    return "["s + input + "]"s;
  }

  std::wstring bracket(const std::wstring &input) {
    return L"["s + input + L"]"s;
  }

}  // namespace logging
