/** @file Regression coverage for the production guarded restore sequence. */
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <src/platform/linux/private_display_restore_transaction.h>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace transaction = platf::linux_private_display::restore_transaction;
namespace policy = platf::linux_private_display::restore_policy;

namespace {
  using json = nlohmann::json;

  json desktop() {
    return {{"outputs", {
      {{"name", "eDP-1"}, {"connected", true}, {"enabled", false}, {"rotation", 1}},
      {{"name", "DP-1"}, {"connected", true}, {"enabled", true}, {"currentModeId", "25"},
       {"priority", 1}, {"scale", 1.5}, {"rotation", 1}, {"hdr", true},
       {"pos", {{"x", 0}, {"y", 0}}}, {"size", {{"width", 2560}, {"height", 1440}}}},
      {{"name", "DP-2"}, {"connected", true}, {"enabled", true}, {"currentModeId", "31"},
       {"priority", 2}, {"scale", 1.0}, {"rotation", 2}, {"hdr", false},
       {"pos", {{"x", 2560}, {"y", 0}}}, {"size", {{"width", 1920}, {"height", 1080}}}}
    }}};
  }

  struct plan_t {
    std::vector<std::string> activate, deactivate, guard_activate;
    std::optional<std::string> guard_output;
  };

  struct compositor_t {
    json baseline = desktop();
    json current = baseline;
    std::set<std::string> retiring {"Virtual-1"};
    std::vector<std::string> events;
    std::vector<std::vector<std::string>> configurations;
    std::string fail_stage;
    std::string block_activation_output;
    bool no_guard {false};
    bool panel_reenabled_by_hotplug {true};
    bool disconnect_acknowledged_without_removal {false};
    bool prevent_final_repair {false};
    bool disable_guard_after_hotplug {false};
    bool guard_recovery_configured {false}, guard_recovery_checked {false};
    unsigned max_active_outputs {0};
    unsigned allowed_checks {0}, cancel_at_check {0};
    unsigned snapshot_checks {0}, capture_checks {0};
    bool cancelled {false};

    compositor_t() {
      current["outputs"][1]["enabled"] = false;
      current["outputs"][1]["priority"] = 2;
      current["outputs"][2]["enabled"] = false;
      current["outputs"][2]["priority"] = 3;
      current["outputs"].push_back({{"name", "Virtual-1"}, {"connected", true}, {"enabled", true}, {"priority", 1}});
    }

    json *find(const std::string &name) {
      for (auto &output : current["outputs"]) {
        if (output.value("name", std::string {}) == name) return &output;
      }
      return nullptr;
    }

    plan_t plan(const json &saved_configuration, const json &present_configuration, const std::set<std::string> &retire) {
      plan_t result;
      std::vector<policy::candidate_t> candidates;
      for (const auto &saved : saved_configuration["outputs"]) {
        const auto name = saved.value("name", std::string {});
        bool connected = false;
        for (const auto &present : present_configuration["outputs"]) {
          if (present.value("name", std::string {}) == name) connected = present.value("connected", false);
        }
        candidates.push_back({name, saved.value("enabled", false), connected, name.starts_with("Virtual-"), retire.contains(name)});
        if (!connected) continue;
        if (saved.value("enabled", false)) {
          result.activate.push_back("output." + name + ".enable");
          result.activate.push_back("output." + name + ".mode." + saved.value("currentModeId", std::string {"1"}));
          result.activate.push_back("output." + name + ".priority." + std::to_string(saved.value("priority", 0)));
        } else {
          result.deactivate.push_back("output." + name + ".disable");
        }
      }
      for (const auto &name : retire) result.deactivate.push_back("output." + name + ".disable");
      if (!no_guard) result.guard_output = policy::select_guard(candidates);
      if (result.guard_output) {
        const auto prefix = "output." + *result.guard_output + ".";
        for (const auto &argument : result.activate) {
          if (argument.starts_with(prefix)) result.guard_activate.push_back(argument);
        }
      }
      return result;
    }

    bool allowed() {
      ++allowed_checks;
      if (cancel_at_check && allowed_checks >= cancel_at_check) cancelled = true;
      return !cancelled;
    }

    bool configure(const std::vector<std::string> &arguments, const char *phase) {
      EXPECT_FALSE(cancelled) << "configuration attempted after cancellation";
      const std::string stage = std::string {phase} == "topology restore guard activation" ? "guard configuration" :
                                std::string {phase} == "topology restore deactivation" ? "topology configuration" :
                                std::string {phase} == "post-disconnect restore guard activation" ? "guard recovery configuration" : "final configuration";
      events.push_back(stage);
      configurations.push_back(arguments);
      if (fail_stage == stage) return false;
      if (stage == "guard recovery configuration") guard_recovery_configured = true;
      if (stage == "final configuration" && prevent_final_repair) return true;
      auto proposed = current;
      for (const auto &argument : arguments) {
        for (auto &output : proposed["outputs"]) {
          const auto prefix = "output." + output.value("name", std::string {}) + ".";
          if (argument == prefix + "enable") output["enabled"] = true;
          if (argument == prefix + "disable") output["enabled"] = false;
        }
      }
      const auto proposed_active = std::ranges::count_if(proposed["outputs"], [](const auto &output) {
        return output.value("connected", false) && output.value("enabled", false);
      });
      if (max_active_outputs && proposed_active > max_active_outputs) return false;
      for (const auto &argument : arguments) {
        for (const auto &saved : baseline["outputs"]) {
          const auto name = saved.value("name", std::string {});
          if (argument == "output." + name + ".enable") {
            if (block_activation_output == name) continue;
            if (auto *output = find(name); output && output->value("connected", false)) {
              const int priority = output->value("priority", 0);
              *output = saved;
              // KWin keeps physical priorities shifted until private hot-unplug.
              if (auto *private_output = find("Virtual-1"); private_output && private_output->value("connected", false)) {
                (*output)["priority"] = priority;
              }
            }
          } else if (argument == "output." + name + ".disable") {
            if (auto *output = find(name)) (*output)["enabled"] = false;
          } else if (argument == "output." + name + ".priority." + std::to_string(saved.value("priority", 0))) {
            if (auto *output = find(name)) {
              const auto *private_output = find("Virtual-1");
              if (!private_output || !private_output->value("connected", false)) (*output)["priority"] = saved.value("priority", 0);
            }
          }
        }
        // This models the dangerous effect the engine must filter out.
        for (const auto &name : retiring) {
          if (argument == "output." + name + ".disable") {
            if (auto *output = find(name)) (*output)["enabled"] = false;
          }
        }
      }
      return true;
    }

    bool wait_snapshot(const json &expected, bool final) {
      ++snapshot_checks;
      const auto stage = final ? "final verification" : expected["outputs"].size() == 1 ?
                           (guard_recovery_configured && !guard_recovery_checked ? "guard recovery activation" :
                            snapshot_checks == 1 ? "guard activation" : "guard activation recheck") : "topology activation";
      if (std::string {stage} == "guard recovery activation") guard_recovery_checked = true;
      events.emplace_back(stage);
      return fail_stage != stage && policy::snapshot_matches(expected, current, final);
    }

    bool wait_capture(const std::string &name) {
      ++capture_checks;
      const auto stage = std::ranges::find(events, "final verification") != events.end() ? "final guard capture" :
                         guard_recovery_configured ? "guard recovery capture" : disconnected() ? "post-retirement guard capture" :
                         capture_checks == 1 ? "guard capture" : "guard capture recheck";
      events.emplace_back(stage);
      const auto *output = find(name);
      return fail_stage != stage && output && output->value("connected", false) && output->value("enabled", false);
    }

    bool disconnect(const std::string &name) {
      EXPECT_FALSE(cancelled) << "connector disconnected after cancellation";
      events.push_back("disconnect " + name);
      if (fail_stage == "disconnect") return false;
      if (disconnect_acknowledged_without_removal) return true;
      if (auto *output = find(name)) {
        (*output)["connected"] = false;
        (*output)["enabled"] = false;
      }
      if (auto *first = find("DP-1")) (*first)["priority"] = 1;
      if (auto *second = find("DP-2")) (*second)["priority"] = 2;
      if (panel_reenabled_by_hotplug) (*find("eDP-1"))["enabled"] = true;
      if (disable_guard_after_hotplug) (*find("DP-1"))["enabled"] = false;
      return true;
    }

    bool wait_disconnected(const std::set<std::string> &names) {
      events.emplace_back("connector disappearance");
      if (fail_stage == "connector disappearance") return false;
      for (const auto &name : names) {
        if (const auto *output = find(name); output && output->value("connected", false)) return false;
      }
      return true;
    }

    std::optional<json> query() {
      events.emplace_back("settled query");
      return fail_stage == "settled query" ? std::nullopt : std::make_optional(current);
    }

    transaction::result_e restore() {
      const auto initial = current;
      return transaction::perform_guarded_restore(baseline, initial, retiring, *this);
    }

    bool disconnected() const {
      return std::ranges::any_of(events, [](const auto &event) { return event.starts_with("disconnect "); });
    }
  };
}

TEST(LinuxPrivateDisplayRestoreTransaction, ExclusiveRestoreVerifiesGuardBeforeRetirementThenRestoresFullDesktop) {
  compositor_t compositor;
  const auto original = compositor.baseline;
  EXPECT_EQ(compositor.restore(), transaction::result_e::restored);
  EXPECT_EQ(compositor.events, (std::vector<std::string> {
    "guard configuration", "guard activation", "guard capture", "topology configuration", "guard activation recheck",
    "guard capture recheck", "disconnect Virtual-1", "connector disappearance", "settled query", "post-retirement guard capture",
    "final configuration", "topology activation", "final verification", "final guard capture"}));
  ASSERT_EQ(compositor.configurations.size(), 3U);
  for (const auto &argument : compositor.configurations[1]) {
    EXPECT_FALSE(argument.starts_with("output.DP-1.")) << argument;
    EXPECT_FALSE(argument.starts_with("output.Virtual-1.")) << argument;
  }
  for (const auto &argument : compositor.configurations[2]) {
    EXPECT_TRUE(!argument.starts_with("output.DP-1.") || argument.starts_with("output.DP-1.priority.")) << argument;
  }
  EXPECT_TRUE(policy::snapshot_matches(original, compositor.current, true));
  EXPECT_EQ(compositor.baseline, original);
}

TEST(LinuxPrivateDisplayRestoreTransaction, EveryPreRetirementFailurePreservesPrivateScanoutAndBaseline) {
  const std::vector<std::pair<std::string, transaction::result_e>> cases {
    {"guard configuration", transaction::result_e::guard_configuration_failed},
    {"guard activation", transaction::result_e::guard_activation_failed},
    {"guard capture", transaction::result_e::guard_capture_failed},
    {"topology configuration", transaction::result_e::topology_configuration_failed},
    {"guard activation recheck", transaction::result_e::guard_activation_lost},
    {"guard capture recheck", transaction::result_e::guard_capture_lost},
  };
  for (const auto &[stage, result] : cases) {
    SCOPED_TRACE(stage);
    compositor_t compositor;
    const auto original = compositor.baseline;
    compositor.fail_stage = stage;
    EXPECT_EQ(compositor.restore(), result);
    EXPECT_FALSE(compositor.disconnected());
    ASSERT_NE(compositor.find("Virtual-1"), nullptr);
    EXPECT_TRUE((*compositor.find("Virtual-1"))["enabled"]);
    EXPECT_TRUE((*compositor.find("Virtual-1"))["connected"]);
    EXPECT_EQ(compositor.baseline, original);
  }
}

TEST(LinuxPrivateDisplayRestoreTransaction, NoSavedGuardCannotConfigureOrDisconnectAnything) {
  compositor_t compositor;
  compositor.no_guard = true;
  EXPECT_EQ(compositor.restore(), transaction::result_e::no_guard);
  EXPECT_TRUE(compositor.events.empty());
}

TEST(LinuxPrivateDisplayRestoreTransaction, FailedSecondaryActivationAfterPhysicalHandoffRetainsBaselineAndPhysicalGuard) {
  compositor_t compositor;
  compositor.block_activation_output = "DP-2";
  EXPECT_EQ(compositor.restore(), transaction::result_e::topology_activation_failed);
  EXPECT_TRUE((*compositor.find("DP-1"))["enabled"]);
  EXPECT_FALSE((*compositor.find("DP-2"))["enabled"]);
  EXPECT_FALSE((*compositor.find("Virtual-1"))["connected"]);
  EXPECT_TRUE(compositor.disconnected());
}

TEST(LinuxPrivateDisplayRestoreTransaction, EveryPostRetirementFailureKeepsCompleteBaselineForRetry) {
  const std::vector<std::pair<std::string, transaction::result_e>> cases {
    {"disconnect", transaction::result_e::connector_disconnect_failed},
    {"connector disappearance", transaction::result_e::connector_disappearance_failed},
    {"settled query", transaction::result_e::settled_query_failed},
    {"final configuration", transaction::result_e::final_configuration_failed},
    {"topology activation", transaction::result_e::topology_activation_failed},
    {"final verification", transaction::result_e::final_verification_failed},
    {"final guard capture", transaction::result_e::final_guard_capture_failed},
  };
  for (const auto &[stage, result] : cases) {
    SCOPED_TRACE(stage);
    compositor_t compositor;
    const auto original = compositor.baseline;
    compositor.fail_stage = stage;
    EXPECT_EQ(compositor.restore(), result);
    EXPECT_TRUE(compositor.disconnected());
    EXPECT_EQ(compositor.baseline, original);
  }
}

TEST(LinuxPrivateDisplayRestoreTransaction, HotplugAcknowledgementMustWaitForActualConnectorDisappearance) {
  compositor_t compositor;
  compositor.disconnect_acknowledged_without_removal = true;
  EXPECT_EQ(compositor.restore(), transaction::result_e::connector_disappearance_failed);
  EXPECT_EQ(compositor.events.back(), "connector disappearance");
}

TEST(LinuxPrivateDisplayRestoreTransaction, StrictFinalMismatchCannotReportRestorationSuccess) {
  compositor_t compositor;
  *compositor.find("DP-2") = compositor.baseline["outputs"][2];
  (*compositor.find("DP-2"))["priority"] = 3;
  compositor.prevent_final_repair = true;
  EXPECT_EQ(compositor.restore(), transaction::result_e::final_verification_failed);
  EXPECT_TRUE((*compositor.find("eDP-1"))["enabled"]);
  EXPECT_FALSE(policy::snapshot_matches(compositor.baseline, compositor.current, true));
}

TEST(LinuxPrivateDisplayRestoreTransaction, MissingSecondaryRetiresPrivateBehindGuardButRetainsCompleteRecoveryBaseline) {
  compositor_t compositor;
  (*compositor.find("DP-2"))["connected"] = false;
  const auto original = compositor.baseline;
  EXPECT_EQ(compositor.restore(), transaction::result_e::final_verification_failed);
  EXPECT_TRUE(compositor.disconnected());
  EXPECT_TRUE((*compositor.find("DP-1"))["enabled"]);
  EXPECT_FALSE((*compositor.find("Virtual-1"))["connected"]);
  EXPECT_EQ(compositor.baseline, original);
}

TEST(LinuxPrivateDisplayRestoreTransaction, AlreadySettledDesktopAvoidsExtraPostDisconnectModeset) {
  compositor_t compositor;
  compositor.panel_reenabled_by_hotplug = false;
  *compositor.find("DP-2") = compositor.baseline["outputs"][2];
  (*compositor.find("DP-2"))["priority"] = 3;
  EXPECT_EQ(compositor.restore(), transaction::result_e::restored);
  EXPECT_EQ(compositor.configurations.size(), 2U);
  EXPECT_EQ(compositor.events.back(), "final guard capture");
}

TEST(LinuxPrivateDisplayRestoreTransaction, CancellationAtEveryAdmissionBoundaryStopsFurtherMutationsAndRetainsBaseline) {
  // Initial check, guard, deactivation, connector, post-hotplug repair, final completion.
  for (const bool recover_guard : {false, true}) {
    SCOPED_TRACE(recover_guard);
    for (unsigned check = 1; check <= (recover_guard ? 7U : 6U); ++check) {
      SCOPED_TRACE(check);
      compositor_t compositor;
      const auto original = compositor.baseline;
      compositor.disable_guard_after_hotplug = recover_guard;
      compositor.cancel_at_check = check;
      EXPECT_EQ(compositor.restore(), transaction::result_e::cancelled);
      EXPECT_TRUE(compositor.cancelled);
      EXPECT_EQ(compositor.baseline, original);
      EXPECT_EQ(compositor.allowed_checks, check);
    }
  }
}

TEST(LinuxPrivateDisplayRestoreTransaction, CancellationBetweenConnectorRetirementsPreservesRemainingScanout) {
  compositor_t compositor;
  compositor.retiring.insert("Virtual-2");
  compositor.current["outputs"].push_back({{"name", "Virtual-2"}, {"connected", true}, {"enabled", true}});
  compositor.cancel_at_check = 5;
  EXPECT_EQ(compositor.restore(), transaction::result_e::cancelled);
  EXPECT_FALSE((*compositor.find("Virtual-1"))["connected"]);
  EXPECT_TRUE((*compositor.find("Virtual-2"))["connected"]);
  EXPECT_TRUE((*compositor.find("Virtual-2"))["enabled"]);
  EXPECT_EQ(compositor.events.back(), "post-retirement guard capture");
}

TEST(LinuxPrivateDisplayRestoreTransaction, BaselineAtCompositorOutputCapacityRestoresByRetiringPrivateBeforeFullActivation) {
  compositor_t compositor;
  // Two saved physical outputs already consume this compositor's two slots.
  // Activating them while the exclusive private output survives would require three.
  compositor.max_active_outputs = 2;
  EXPECT_EQ(compositor.restore(), transaction::result_e::restored);
  EXPECT_TRUE((*compositor.find("DP-1"))["enabled"]);
  EXPECT_TRUE((*compositor.find("DP-2"))["enabled"]);
  EXPECT_FALSE((*compositor.find("Virtual-1"))["connected"]);
}

TEST(LinuxPrivateDisplayRestoreTransaction, FirstHotplugThatDisablesGuardPreservesRemainingPrivateScanoutForRetry) {
  compositor_t compositor;
  compositor.retiring.insert("Virtual-2");
  compositor.current["outputs"].push_back({{"name", "Virtual-2"}, {"connected", true}, {"enabled", true}});
  compositor.disable_guard_after_hotplug = true;
  const auto original = compositor.baseline;
  EXPECT_EQ(compositor.restore(), transaction::result_e::guard_activation_lost);
  EXPECT_FALSE((*compositor.find("Virtual-1"))["connected"]);
  EXPECT_TRUE((*compositor.find("Virtual-2"))["connected"]);
  EXPECT_TRUE((*compositor.find("Virtual-2"))["enabled"]);
  EXPECT_EQ(compositor.events.back(), "guard activation recheck");
  EXPECT_EQ(compositor.baseline, original);
}

TEST(LinuxPrivateDisplayRestoreTransaction, LastHotplugThatChangesDesktopRecoversGuardBeforeFinalDisables) {
  compositor_t compositor;
  compositor.disable_guard_after_hotplug = true;
  EXPECT_EQ(compositor.restore(), transaction::result_e::restored);
  const auto recovery = std::ranges::find(compositor.events, "guard recovery configuration");
  const auto final = std::ranges::find(compositor.events, "final configuration");
  ASSERT_NE(recovery, compositor.events.end());
  ASSERT_NE(final, compositor.events.end());
  EXPECT_LT(recovery, final);
  EXPECT_TRUE(policy::snapshot_matches(compositor.baseline, compositor.current, true));
}

TEST(LinuxPrivateDisplayRestoreTransaction, FailedPostHotplugGuardRecoveryCannotDisableSurvivingDesktop) {
  const std::vector<std::pair<std::string, transaction::result_e>> cases {
    {"guard recovery configuration", transaction::result_e::guard_recovery_configuration_failed},
    {"guard recovery activation", transaction::result_e::guard_recovery_activation_failed},
    {"guard recovery capture", transaction::result_e::guard_recovery_capture_failed},
  };
  for (const auto &[stage, result] : cases) {
    SCOPED_TRACE(stage);
    compositor_t compositor;
    compositor.disable_guard_after_hotplug = true;
    compositor.fail_stage = stage;
    const auto original = compositor.baseline;
    EXPECT_EQ(compositor.restore(), result);
    EXPECT_EQ(std::ranges::find(compositor.events, "final configuration"), compositor.events.end());
    EXPECT_TRUE((*compositor.find("eDP-1"))["enabled"]);
    EXPECT_EQ(compositor.baseline, original);
  }
}

TEST(LinuxPrivateDisplayRestoreTransaction, LostPostRetirementCaptureReactivatesGuardBeforeCompletingDesktop) {
  compositor_t compositor;
  compositor.fail_stage = "post-retirement guard capture";
  EXPECT_EQ(compositor.restore(), transaction::result_e::restored);
  EXPECT_NE(std::ranges::find(compositor.events, "guard recovery configuration"), compositor.events.end());
  EXPECT_EQ(compositor.events.back(), "final guard capture");
}
