/**
 * @file src/platform/common_services.h
 * @brief Injectable contracts for process environment and platform roots.
 */
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace platf::services {
  class environment_provider_t {
  public:
    virtual ~environment_provider_t() = default;
    virtual int set(const std::string &name, const std::string &value) = 0;
    virtual int unset(const std::string &name) = 0;
    virtual std::optional<std::string> get(const std::string &name) const = 0;
  };

  class environment_t {
  public:
    explicit environment_t(environment_provider_t &provider);
    int set(const std::string &name, const std::string &value);
    int unset(const std::string &name);
    std::optional<std::string> get(const std::string &name) const;

  private:
    environment_provider_t &_provider;
  };

  environment_t &process_environment();

  class host_name_provider_t {
  public:
    virtual ~host_name_provider_t() = default;
    virtual std::optional<std::string> read() const = 0;
  };

  class function_host_name_provider_t: public host_name_provider_t {
  public:
    using reader_t = std::function<std::optional<std::string>()>;
    explicit function_host_name_provider_t(reader_t reader);
    std::optional<std::string> read() const override;

  private:
    reader_t _reader;
  };

  std::string host_name_or(const host_name_provider_t &provider, std::string fallback = "Sunshine");

  std::filesystem::path home_config_root(const std::optional<std::filesystem::path> &home,
                                         const std::filesystem::path &fallback_home);
}  // namespace platf::services
