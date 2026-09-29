#include <gtest/gtest.h>

#include "src/platform/linux/routed_link.h"

#include <fstream>
#include <filesystem>
#include <sstream>

TEST(LinuxRoutedLink, LoopbackDoesNotAdvertiseNetworkCapacity) {
  const auto loopback = boost::asio::ip::make_address("127.0.0.1");
  platf::routed_link_info_t info;
  EXPECT_EQ(platf::routed_link_bps(loopback, loopback, &info), 0u);
}

TEST(LinuxRoutedLink, UsesTheOutboundEthernetInterface) {
  std::ifstream routes("/proc/net/route");
  std::string line;
  std::getline(routes, line);
  std::string iface;
  while (std::getline(routes, line)) {
    std::istringstream row(line);
    std::string candidate, destination;
    row >> candidate >> destination;
    if (destination == "00000000") {
      iface = candidate;
      break;
    }
  }
  if (iface.empty()) GTEST_SKIP() << "No IPv4 default route";
  const auto device = std::filesystem::path("/sys/class/net") / iface;
  std::error_code error;
  if (!std::filesystem::exists(device / "device", error)) {
    GTEST_SKIP() << "Default route is not a physical interface";
  }
  std::ifstream speed(device / "speed");
  std::uint64_t mbps = 0;
  speed >> mbps;
  if (!speed || mbps == 0 || mbps > 400'000) {
    GTEST_SKIP() << "Physical default route has no readable Ethernet speed";
  }

  platf::routed_link_info_t info;
  const auto actual = platf::routed_link_bps(
    boost::asio::ip::make_address("0.0.0.0"),
    boost::asio::ip::make_address("1.1.1.1"), &info);
  EXPECT_EQ(actual, mbps * 1'000'000);
  EXPECT_EQ(info.alias, iface);
}

TEST(LinuxRoutedLink, RejectsMismatchedAddressFamilies) {
  EXPECT_EQ(platf::routed_link_bps(
              boost::asio::ip::make_address("::1"),
              boost::asio::ip::make_address("192.0.2.1")), 0u);
}
