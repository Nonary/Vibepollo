/**
 * @file tests/unit/platform/test_common.cpp
 * @brief Deterministic tests for common platform service contracts.
 */
#include "../../tests_common.h"

#include <src/platform/common_services.h>

#include <map>
#include <stdexcept>

namespace {
  class fake_environment_provider_t: public platf::services::environment_provider_t {
  public:
    int set(const std::string &name, const std::string &value) override {
      if (fail_writes) {
        return 5;
      }
      values[name] = value;
      return 0;
    }

    int unset(const std::string &name) override {
      if (fail_writes) {
        return 5;
      }
      values.erase(name);
      return 0;
    }

    std::optional<std::string> get(const std::string &name) const override {
      const auto value = values.find(name);
      return value == values.end() ? std::nullopt : std::optional<std::string> {value->second};
    }

    std::map<std::string, std::string> values;
    bool fail_writes = false;
  };

  class fake_host_name_provider_t: public platf::services::host_name_provider_t {
  public:
    std::optional<std::string> value;
    bool throws = false;

    std::optional<std::string> read() const override {
      if (throws) {
        throw std::runtime_error("host name unavailable");
      }
      return value;
    }
  };
}  // namespace

TEST(EnvironmentService, SetGetAndUnsetUseOnlyInjectedProvider) {
  fake_environment_provider_t provider;
  platf::services::environment_t environment {provider};

  EXPECT_EQ(environment.set("SUNSHINE_UNIT_TEST_ENV_VAR", "test_value"), 0);
  EXPECT_EQ(environment.get("SUNSHINE_UNIT_TEST_ENV_VAR"), "test_value");
  EXPECT_EQ(environment.unset("SUNSHINE_UNIT_TEST_ENV_VAR"), 0);
  EXPECT_EQ(environment.get("SUNSHINE_UNIT_TEST_ENV_VAR"), std::nullopt);
}

TEST(EnvironmentService, EmptyNameIsRejectedWithoutCallingProvider) {
  fake_environment_provider_t provider;
  platf::services::environment_t environment {provider};

  EXPECT_EQ(environment.set("", "value"), -1);
  EXPECT_EQ(environment.unset(""), -1);
  EXPECT_EQ(environment.get(""), std::nullopt);
  EXPECT_TRUE(provider.values.empty());
}

TEST(EnvironmentService, ProviderFailuresArePreserved) {
  fake_environment_provider_t provider;
  provider.fail_writes = true;
  platf::services::environment_t environment {provider};

  EXPECT_EQ(environment.set("NAME", "value"), 5);
  EXPECT_EQ(environment.unset("NAME"), 5);
}

TEST(HostNameService, UsesValueAndFallsBackForEmptyOrFailure) {
  fake_host_name_provider_t provider;
  provider.value = "deterministic-host";
  EXPECT_EQ(platf::services::host_name_or(provider), "deterministic-host");

  provider.value = "";
  EXPECT_EQ(platf::services::host_name_or(provider), "Sunshine");

  provider.throws = true;
  EXPECT_EQ(platf::services::host_name_or(provider, "fallback"), "fallback");
}

TEST(AppDataRootPolicy, PrefersProvidedHomeAndUsesFallbackWhenMissing) {
  EXPECT_EQ(
    platf::services::home_config_root(std::filesystem::path {"/provided"}, "/fallback"),
    std::filesystem::path {"/provided/.config/sunshine"}
  );
  EXPECT_EQ(
    platf::services::home_config_root(std::nullopt, "/fallback"),
    std::filesystem::path {"/fallback/.config/sunshine"}
  );
}
