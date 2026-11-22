#pragma once

#include <string_view>
#include <utility>

namespace platf::steam::stream_ui {
  enum class action {
    open,
    close,
  };

  constexpr std::string_view uri(action command) {
    return command == action::open ? "steam://open/bigpicture" : "steam://close/bigpicture";
  }

  // One opt-in applies to one app lifetime, including its resumable sessions.
  // Steam's UI mode is never read. An eligible launch schedules a close even
  // when Steam was already in Big Picture or the open command reported failure.
  class session {
  public:
    session() = default;
    session(const session &) = delete;
    session &operator=(const session &) = delete;

    session(session &&other) noexcept:
        close_pending_(std::exchange(other.close_pending_, false)) {}

    session &operator=(session &&other) noexcept {
      if (this != &other) {
        close_pending_ = std::exchange(other.close_pending_, false);
      }
      return *this;
    }

    template<class Command>
    void start(bool enabled, bool steam_launch, Command &&run) {
      if (enabled && steam_launch && !std::exchange(close_pending_, true)) {
        run(action::open);
      }
    }

    template<class Command>
    void finish(Command &&run) {
      if (std::exchange(close_pending_, false)) {
        run(action::close);
      }
    }

  private:
    bool close_pending_ = false;
  };
}  // namespace platf::steam::stream_ui
