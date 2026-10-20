#include "src/platform/windows/virtual_display_catalog_policy.h"

#include <gtest/gtest.h>
#include <cstring>
#include <functional>
#include <vector>

namespace vdd = virtual_display::driver;
namespace policy = VDISPLAY::catalog_policy;
namespace {
  class Transport: public vdd::ControlTransport {
  public:
    vdd::ProtocolVersion version {};
    vdd::QueryDisplayStateResult state {};
    bool fail_version = false, fail_state = false;
    std::vector<std::uint32_t> calls;
    Transport() { version.minor = 8; }
    template<class T> bool reply(const T &value, void *output, std::size_t size, std::size_t &returned) {
      if (size != sizeof(T)) return false;
      std::memcpy(output, &value, size);
      returned = size;
      return true;
    }
    bool ioctl(std::uint32_t code, const void *, std::size_t, void *output, std::size_t size,
               std::size_t &returned, std::uint32_t &error) override {
      calls.push_back(code);
      error = 0; returned = 0;
      if (code == vdd::kIoctlGetProtocolVersion) return !fail_version && reply(version, output, size, returned);
      if (code == vdd::kIoctlQueryDisplayState) return !fail_state && reply(state, output, size, returned);
      return false;
    }
    void display(bool restricted, std::uint32_t width = 2420, std::uint32_t height = 1668) {
      state.entry_count = 1;
      state.entries[0].kind = vdd::kDisplayStateKindTemporary;
      state.entries[0].display_id = 325;
      state.entries[0].width = width;
      state.entries[0].height = height;
      state.entries[0].flags = restricted ? policy::requested_resolution_state_flag : 0;
    }
  };
}

TEST(VirtualDisplayCatalogPolicy, ActualOpenedVersionAuthorizesAdditiveCreateBit) {
  Transport transport;
  vdd::ControlClient client {transport};
  vdd::CreateTemporaryDisplayRequest request {};
  request.width = 2420; request.height = 1668; request.refresh_rate_millihz = 60000;
  ASSERT_TRUE(policy::set_create_policy(client, request, true));
  EXPECT_EQ(request.flags, 0x2u);
  ASSERT_EQ(transport.calls.size(), 1u);
  EXPECT_EQ(transport.calls[0], vdd::kIoctlGetProtocolVersion);
}

TEST(VirtualDisplayCatalogPolicy, OldWrongNamespaceWrongMajorAndUnknownNeverAuthorizeFlag) {
  for (int condition = 0; condition < 4; ++condition) {
    Transport transport;
    if (condition == 0) transport.version.minor = 7;
    if (condition == 1) transport.version.api_namespace = {};
    if (condition == 2) transport.version.major = 4;
    if (condition == 3) transport.fail_version = true;
    vdd::ControlClient client {transport};
    vdd::CreateTemporaryDisplayRequest request {};
    request.width = 2420; request.height = 1668; request.refresh_rate_millihz = 60000;
    EXPECT_FALSE(policy::set_create_policy(client, request, true));
    EXPECT_EQ(request.flags, 0u);
    EXPECT_EQ(policy::inspect(client, 325, 2420, 1668, true), policy::inspection::unknown);
  }
}

TEST(VirtualDisplayCatalogPolicy, DefaultFullCatalogUsesNoNewFlagAndKeeps37Behavior) {
  Transport transport; transport.version.minor = 7;
  vdd::ControlClient client {transport};
  vdd::CreateTemporaryDisplayRequest request {};
  EXPECT_TRUE(policy::set_create_policy(client, request, false));
  EXPECT_TRUE(transport.calls.empty());
  EXPECT_EQ(request.flags, 0u);
  EXPECT_EQ(policy::inspect(client, 325, 2420, 1668, false), policy::inspection::matches);
  EXPECT_EQ(transport.calls.size(), 1u); // no unsupported state query
}

TEST(VirtualDisplayCatalogPolicy, ActualPolicyAndDimensionsControlReuseAndOptOut) {
  Transport transport; transport.display(true);
  vdd::ControlClient client {transport};
  EXPECT_EQ(policy::inspect(client, 325, 2420, 1668, true), policy::inspection::matches);
  EXPECT_EQ(policy::inspect(client, 325, 1920, 1080, true), policy::inspection::replace_owned);
  EXPECT_EQ(policy::inspect(client, 325, 2420, 1668, false), policy::inspection::replace_owned);
  transport.display(false);
  EXPECT_EQ(policy::inspect(client, 325, 2420, 1668, true), policy::inspection::replace_owned);
  EXPECT_EQ(policy::inspect(client, 325, 1920, 1080, false), policy::inspection::matches);
}

TEST(VirtualDisplayCatalogPolicy, MissingDuplicatePermanentAndMalformedStateDoNotProvePolicy) {
  Transport transport;
  vdd::ControlClient client {transport};
  EXPECT_EQ(policy::inspect(client, 325, 2420, 1668, true), policy::inspection::absent);
  transport.display(true); transport.state.entries[0].kind = vdd::kDisplayStateKindPermanent;
  EXPECT_EQ(policy::inspect(client, 325, 2420, 1668, true), policy::inspection::absent);
  transport.display(true); transport.state.entry_count = 2; transport.state.entries[1] = transport.state.entries[0];
  EXPECT_EQ(policy::inspect(client, 325, 2420, 1668, true), policy::inspection::unknown);
  transport.state.entry_count = vdd::kMaxDisplayStateEntries + 1;
  EXPECT_EQ(policy::inspect(client, 325, 2420, 1668, true), policy::inspection::unknown);
  transport.fail_state = true;
  EXPECT_EQ(policy::inspect(client, 325, 2420, 1668, true), policy::inspection::unknown);
}

TEST(VirtualDisplayCatalogPolicy, SamePolicyReclaimPreservesMonitorAndCurrentMode) {
  Transport transport; transport.display(true);
  vdd::ControlClient client {transport};
  int reclaims = 0, removals = 0;
  const auto result = policy::reconcile_existing(client, 325, 2420, 1668, true, true,
    [&] { ++reclaims; return policy::ownership::owned; }, [&] { ++removals; return true; });
  EXPECT_EQ(result, policy::reconciliation::reuse);
  EXPECT_EQ(reclaims, 1); EXPECT_EQ(removals, 0);
}

TEST(VirtualDisplayCatalogPolicy, ChangedPolicyRequiresBoundaryAndSecureOwnershipBeforeRemoval) {
  for (int control = 0; control < 4; ++control) {
    Transport transport; transport.display(false);
    vdd::ControlClient client {transport};
    int reclaims = 0, removals = 0;
    const auto result = policy::reconcile_existing(client, 325, 2420, 1668, true, control != 0,
      [&] { ++reclaims; return control == 1 ? policy::ownership::failed : policy::ownership::owned; },
      [&] { ++removals; return control != 2; });
    EXPECT_EQ(result, control == 3 ? policy::reconciliation::removed_retry : policy::reconciliation::failed);
    EXPECT_EQ(reclaims, control == 0 ? 0 : 1);
    EXPECT_EQ(removals, control < 2 ? 0 : 1);
  }
}

TEST(VirtualDisplayCatalogPolicy, UnknownCapabilityOrStatePreservesTargetAndSkipsReclaimAndRemoval) {
  for (bool fail_version : {false, true}) {
    Transport transport; transport.display(false);
    transport.fail_version = fail_version; transport.fail_state = !fail_version;
    vdd::ControlClient client {transport};
    int effects = 0;
    EXPECT_EQ(policy::reconcile_existing(client, 325, 2420, 1668, true, true,
      [&] { ++effects; return policy::ownership::owned; }, [&] { ++effects; return true; }), policy::reconciliation::failed);
    EXPECT_EQ(effects, 0);
  }
}

TEST(VirtualDisplayCatalogPolicy, ExpiredOwnedRecordCanCreateAgainWithoutUnownedRemoval) {
  Transport transport;
  vdd::ControlClient client {transport};
  int removals = 0;
  EXPECT_EQ(policy::reconcile_existing(client, 325, 2420, 1668, true, true,
    [] { return policy::ownership::absent; }, [&] { ++removals; return true; }), policy::reconciliation::absent);
  EXPECT_EQ(removals, 0);
}

TEST(VirtualDisplayCatalogPolicy, PrivateCatalogCannotBecomeAnotherClientsSharedCaptureTarget) {
  Transport transport; transport.display(true);
  vdd::ControlClient client {transport};
  EXPECT_FALSE(policy::allows_shared_capture(client));
  transport.display(false);
  EXPECT_TRUE(policy::allows_shared_capture(client));
  transport.fail_state = true;
  EXPECT_FALSE(policy::allows_shared_capture(client));
  transport.version.minor = 7;
  EXPECT_TRUE(policy::allows_shared_capture(client));
}


TEST(VirtualDisplayCatalogPolicy, InvalidDescriptorFailsBeforeReplacementPreflight) {
  EXPECT_TRUE(policy::descriptor_is_valid(2420, 1668, 60000));
  EXPECT_FALSE(policy::descriptor_is_valid(0, 1668, 60000));
  EXPECT_FALSE(policy::descriptor_is_valid(2420, 0, 60000));
  EXPECT_FALSE(policy::descriptor_is_valid(7681, 1668, 60000));
  EXPECT_FALSE(policy::descriptor_is_valid(2420, 4321, 60000));
  EXPECT_FALSE(policy::descriptor_is_valid(2420, 1668, 0));
}

TEST(VirtualDisplayCatalogPolicy, SharedCaptureRequiresVerifiedVersionAndStateButPermitsKnown37) {
  for (int condition = 0; condition < 5; ++condition) {
    Transport transport; transport.display(false);
    if (condition == 0) transport.fail_version = true;
    if (condition == 1) transport.version.api_namespace = {};
    if (condition == 2) transport.version.major = 4;
    if (condition == 3) transport.fail_state = true;
    if (condition == 4) transport.version.minor = 7;
    vdd::ControlClient client {transport};
    EXPECT_EQ(policy::allows_shared_capture(client), condition == 4);
    if (condition == 4) { EXPECT_EQ(transport.calls.size(), 1u); }
  }
}

TEST(VirtualDisplayCatalogPolicy, OptOutUnknownStateDoesNotAuthorizeReclaimOrRemoval) {
  for (bool fail_version : {false, true}) {
    Transport transport; transport.display(true);
    transport.fail_version = fail_version; transport.fail_state = !fail_version;
    vdd::ControlClient client {transport};
    int effects = 0;
    EXPECT_EQ(policy::reconcile_existing(client, 325, 2420, 1668, false, true,
      [&] { ++effects; return policy::ownership::owned; }, [&] { ++effects; return true; }), policy::reconciliation::failed);
    EXPECT_EQ(effects, 0);
  }
}
