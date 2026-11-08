#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
namespace display_helper_ipc {
  struct endpoint_t { std::string pipe_name; std::uint32_t parent_pid; };
  inline std::optional<endpoint_t> endpoint_from_command_line(int argc, char *argv[]) {
    std::optional<std::string> name;
    std::optional<std::uint32_t> pid;
    for (int i = 1; i < argc; ++i) {
      const std::string_view arg = argv[i] ? argv[i] : "";
      if (arg.starts_with("--ipc-pipe=")) {
        if (name) return std::nullopt;
        const auto value = arg.substr(11);
        constexpr std::string_view prefix = "sunshine_display_helper_";
        if (!value.starts_with(prefix) || value.size() != prefix.size() + 36) return std::nullopt;
        for (std::size_t j = 0; j < 36; ++j) {
          const char ch = value[prefix.size() + j];
          if (j == 8 || j == 13 || j == 18 || j == 23) { if (ch != '-') return std::nullopt; }
          else if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return std::nullopt;
        }
        name = value;
      } else if (arg.starts_with("--ipc-parent=")) {
        if (pid) return std::nullopt;
        const auto value = arg.substr(13);
        std::uint32_t parsed = 0;
        auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !parsed) return std::nullopt;
        pid = parsed;
      } else if (arg.starts_with("--ipc-handle=")) return std::nullopt;
    }
    if (!name || !pid) return std::nullopt;
    return endpoint_t{*name, *pid};
  }
}
