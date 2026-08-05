#pragma once

#include <string>
#include <string_view>

namespace command_policy {
  struct result_t {
    int exit_code;
    std::string output;
  };
  class runner_t {
  public:
    virtual ~runner_t() = default;
    virtual result_t run(std::string_view command, std::string_view working_directory) = 0;
  };
  inline bool has_expected_outcome(runner_t &runner, std::string_view command, std::string_view working_directory, bool should_succeed) {
    const auto result = runner.run(command, working_directory);
    return should_succeed ? result.exit_code == 0 : result.exit_code != 0;
  }
}  // namespace command_policy
