/**
 * @file tests/unit/test_network.cpp
 * @brief Test pure network policies.
 */
#include "../tests_common.h"

#include <src/network_policy.h>

struct MdnsInstanceNameTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(MdnsInstanceNameTest, Run) {
  auto [input, expected] = GetParam();
  const auto expected_instance = expected == "Sunshine" ? "Apollo" : expected;
  ASSERT_EQ(net::mdns_instance_name(input), expected_instance);
}

INSTANTIATE_TEST_SUITE_P(
  MdnsInstanceNameTests,
  MdnsInstanceNameTest,
  testing::Values(
    std::make_tuple("shortname-123", "shortname-123"),
    std::make_tuple("space 123", "space-123"),
    std::make_tuple("hostname.domain.test", "hostname"),
    std::make_tuple("&", "Sunshine"),
    std::make_tuple("", "Sunshine"),
    std::make_tuple("😁", "Sunshine"),
    std::make_tuple(std::string(128, 'a'), std::string(63, 'a'))
  )
);

TEST(NetworkBindAddressPolicy, DefaultsToIpv4WildcardWhenUnconfigured) {
  const auto bind_addr = net::select_bind_address("", net::af_e::IPV4);
  ASSERT_EQ(bind_addr, "0.0.0.0");
}

TEST(NetworkBindAddressPolicy, DefaultsToIpv6WildcardWhenUnconfigured) {
  const auto bind_addr = net::select_bind_address("", net::af_e::BOTH);
  ASSERT_EQ(bind_addr, "::");
}

TEST(NetworkBindAddressPolicy, UsesConfiguredIpv4Address) {
  const auto bind_addr = net::select_bind_address("192.168.1.100", net::af_e::IPV4);
  ASSERT_EQ(bind_addr, "192.168.1.100");
}

TEST(NetworkBindAddressPolicy, UsesConfiguredIpv6Address) {
  const auto bind_addr = net::select_bind_address("::1", net::af_e::BOTH);
  ASSERT_EQ(bind_addr, "::1");
}

TEST(NetworkBindAddressPolicy, ConfiguredAddressOverridesRequestedFamily) {
  const auto bind_addr = net::select_bind_address("2001:db8::1", net::af_e::IPV4);
  ASSERT_EQ(bind_addr, "2001:db8::1");
}

TEST(NetworkBindAddressPolicy, KeepsLoopbackAddresses) {
  const auto bind_addr_v4 = net::select_bind_address("127.0.0.1", net::af_e::IPV4);
  ASSERT_EQ(bind_addr_v4, "127.0.0.1");

  const auto bind_addr_v6 = net::select_bind_address("::1", net::af_e::BOTH);
  ASSERT_EQ(bind_addr_v6, "::1");
}

TEST(NetworkBindAddressPolicy, KeepsLinkLocalAddresses) {
  const auto bind_addr_v4 = net::select_bind_address("169.254.1.1", net::af_e::IPV4);
  ASSERT_EQ(bind_addr_v4, "169.254.1.1");

  const auto bind_addr_v6 = net::select_bind_address("fe80::1", net::af_e::BOTH);
  ASSERT_EQ(bind_addr_v6, "fe80::1");
}

TEST(NetworkBindAddressPolicy, TrimsConfiguredAddressAndTreatsWhitespaceAsUnconfigured) {
  EXPECT_EQ(net::select_bind_address("  192.168.1.100\t", net::af_e::IPV4), "192.168.1.100");
  EXPECT_EQ(net::select_bind_address(" \t", net::af_e::BOTH), "::");
}

TEST(NetworkBindAddressPolicy, SelectsWildcardAddressForAddressFamily) {
  ASSERT_EQ(net::af_to_any_address_string(net::af_e::IPV4), "0.0.0.0");
  ASSERT_EQ(net::af_to_any_address_string(net::af_e::BOTH), "::");
}

TEST(NetworkAddressPolicy, TcpProtocolMatchesBindAddressFamily) {
  const auto v4_protocol = net::tcp_protocol_for_address(boost::asio::ip::make_address("192.168.1.100"));
  ASSERT_EQ(v4_protocol, boost::asio::ip::tcp::v4());

  const auto v6_protocol = net::tcp_protocol_for_address(boost::asio::ip::make_address("::1"));
  ASSERT_EQ(v6_protocol, boost::asio::ip::tcp::v6());
}
