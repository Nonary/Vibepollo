/**
 * @file src/platform/common_services.cpp
 * @brief Injectable contracts for process environment and platform roots.
 */
#include "common_services.h"

#include <cstdlib>

namespace platf::services {
  environment_t::environment_t(environment_provider_t &provider):
      _provider(provider) {}

  int environment_t::set(const std::string &name, const std::string &value) {
    return name.empty() ? -1 : _provider.set(name, value);
  }

  int environment_t::unset(const std::string &name) {
    return name.empty() ? -1 : _provider.unset(name);
  }

  std::optional<std::string> environment_t::get(const std::string &name) const {
    return name.empty() ? std::nullopt : _provider.get(name);
  }

  namespace {
    class process_environment_provider_t: public environment_provider_t {
    public:
      int set(const std::string &name, const std::string &value) override {
#ifdef _WIN32
        return _putenv_s(name.c_str(), value.c_str());
#else
        return setenv(name.c_str(), value.c_str(), 1);
#endif
      }

      int unset(const std::string &name) override {
#ifdef _WIN32
        return _putenv_s(name.c_str(), "");
#else
        return unsetenv(name.c_str());
#endif
      }

      std::optional<std::string> get(const std::string &name) const override {
        const char *value = std::getenv(name.c_str());
        return value ? std::optional<std::string> {value} : std::nullopt;
      }
    };
  }  // namespace

  environment_t &process_environment() {
    static process_environment_provider_t provider;
    static environment_t environment {provider};
    return environment;
  }

  function_host_name_provider_t::function_host_name_provider_t(reader_t reader):
      _reader(std::move(reader)) {}

  std::optional<std::string> function_host_name_provider_t::read() const {
    return _reader ? _reader() : std::nullopt;
  }

  std::string host_name_or(const host_name_provider_t &provider, std::string fallback) {
    try {
      auto value = provider.read();
      if (value && !value->empty()) {
        return *value;
      }
    } catch (...) {
    }
    return fallback;
  }

  std::filesystem::path home_config_root(const std::optional<std::filesystem::path> &home,
                                         const std::filesystem::path &fallback_home) {
    const auto &root = home && !home->empty() ? *home : fallback_home;
    return root / ".config" / "sunshine";
  }
}  // namespace platf::services
