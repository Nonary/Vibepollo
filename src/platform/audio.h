/**
 * @file src/platform/audio.h
 * @brief Audio device boundary, independent of video and host services.
 */
#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platf {
  struct sink_t {
    // Play on host PC
    std::string host;

    // On macOS and Windows, it is not possible to create a virtual sink
    // Therefore, it is optional
    struct null_t {
      std::string stereo;
      std::string surround51;
      std::string surround71;
    };

    std::optional<null_t> null;
  };

  enum class capture_e : int {
    ok,  ///< Success
    reinit,  ///< Need to reinitialize
    timeout,  ///< Timeout
    interrupted,  ///< Capture was interrupted
    error  ///< Error
  };

  class mic_t {
  public:
    virtual capture_e sample(std::vector<float> &frame_buffer) = 0;

    virtual ~mic_t() = default;
  };

  class audio_control_t {
  public:
    virtual int set_sink(const std::string &sink) = 0;

    // Select a loopback endpoint without changing system routing, when supported.
    virtual int set_capture_sink([[maybe_unused]] const std::string &sink) {
      return -1;
    }

    virtual std::unique_ptr<mic_t> microphone(const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size, bool continuous, bool host_audio_enabled) = 0;

    /**
     * @brief Check if the audio sink is available in the system.
     * @param sink Sink to be checked.
     * @returns True if available, false otherwise.
     */
    virtual bool is_sink_available(const std::string &sink) = 0;

    virtual std::optional<sink_t> sink_info() = 0;

    /**
     * @brief Restores the host audio sink after streaming stops.
     * @note Most platforms restore a single sink. Windows overrides this to
     *       restore the endpoint captured for each audio role.
     */
    virtual int restore_sink(const std::string &sink) {
      return set_sink(sink);
    }

    /**
     * @brief Resets the default audio device away from virtual streaming speakers.
     * Implementations may continue trying in the background to restore the
     * preferred device after moving the default away from virtual speakers.
     * @param preferred_device Device identifier (platform-specific, e.g. on Windows the
     *        endpoint device_id) of the device that should be restored as default. If
     *        empty, implementations fall back to letting the OS pick whatever non-virtual
     *        device is currently present.
     */
    virtual void reset_default_device([[maybe_unused]] const std::string &preferred_device = {}) {}

    virtual ~audio_control_t() = default;
  };

}  // namespace platf
