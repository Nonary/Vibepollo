#pragma once

#include <virtual_display/driver/control_client.h>

namespace VDISPLAY::catalog_policy {
  namespace driver = virtual_display::driver;

  // Additive control 3.8 wire bits. Keep the consumer compatible with the pinned
  // 3.7 headers; only the queried producer can authorize sending the create bit.
  inline constexpr std::uint32_t requested_resolution_create_flag = 0x2u;
  inline constexpr std::uint32_t requested_resolution_state_flag = 0x4u;

  inline bool descriptor_is_valid(std::uint32_t width, std::uint32_t height, std::uint32_t refresh) {
    driver::CreateTemporaryDisplayRequest probe {};
    probe.lease_id = driver::kMinOpaqueLeaseId;
    probe.display_id = 1;
    probe.width = width;
    probe.height = height;
    probe.refresh_rate_millihz = refresh;
    probe.display_name[0] = 'X';
    // Validate against the existing descriptor contract before any departure.
    // Keep flags zero so the pinned 3.7 validator can validate the shared shape.
    return driver::validate_create_temporary_display(probe, nullptr) == driver::ValidationError::None;
  }

  inline bool supported(driver::ControlClient &client) {
    const auto version = client.query_protocol_version();
    return version.ok() && version.value.api_namespace == driver::kApiNamespaceGuid &&
           version.value.major == 3 && version.value.minor >= 8;
  }

  // A normal shared-capture join must not borrow another client's private
  // enumeration policy. Unknown producer state cannot authorize that join.
  inline bool allows_shared_capture(driver::ControlClient &client) {
    const auto version = client.query_protocol_version();
    if (!version.ok()) return false;
    if (version.value.minor < 8) return true;
    const auto state = client.query_display_state();
    if (!state.ok() || state.value.entry_count > driver::kMaxDisplayStateEntries) return false;
    for (std::uint32_t i = 0; i < state.value.entry_count; ++i) {
      const auto &entry = state.value.entries[i];
      if (entry.kind == driver::kDisplayStateKindTemporary && (entry.flags & requested_resolution_state_flag)) return false;
    }
    return true;
  }

  enum class inspection { unknown, absent, matches, replace_owned };

  inline inspection inspect(
    driver::ControlClient &client, std::uint64_t display_id,
    std::uint32_t width, std::uint32_t height, bool requested_only
  ) {
    const auto version = client.query_protocol_version();
    if (!version.ok()) return inspection::unknown;
    if (version.value.major != 3 || version.value.minor < 8) {
      // Existing 3.7 full-catalog sessions retain their behavior. A producer
      // that cannot report the new bit cannot satisfy an explicit request.
      return requested_only ? inspection::unknown : inspection::matches;
    }
    const auto state = client.query_display_state();
    if (!state.ok() || state.value.entry_count > driver::kMaxDisplayStateEntries) return inspection::unknown;
    const driver::DisplayStateEntry *found = nullptr;
    for (std::uint32_t i = 0; i < state.value.entry_count; ++i) {
      const auto &entry = state.value.entries[i];
      if (entry.kind != driver::kDisplayStateKindTemporary || entry.display_id != display_id) continue;
      if (found) return inspection::unknown;
      found = &entry;
    }
    if (!found) return inspection::absent;
    const bool actual = (found->flags & requested_resolution_state_flag) != 0;
    return actual == requested_only && (!actual || (found->width == width && found->height == height)) ?
      inspection::matches : inspection::replace_owned;
  }

  enum class ownership { owned, absent, failed };
  enum class reconciliation { reuse, absent, removed_retry, failed };

  // No departure is authorized by a display ID alone. Inspect before reclaim,
  // then require successful capability-backed reclaim before exact removal.
  // The caller retries a successful owned removal without adapter recovery.
  template<class Reclaim, class Remove>
  reconciliation reconcile_existing(
    driver::ControlClient &client, std::uint64_t display_id,
    std::uint32_t width, std::uint32_t height, bool requested_only,
    bool session_boundary, Reclaim reclaim, Remove remove
  ) {
    const auto state = inspect(client, display_id, width, height, requested_only);
    if (state == inspection::unknown || (state == inspection::replace_owned && !session_boundary)) return reconciliation::failed;
    const auto claim = reclaim();
    if (claim == ownership::failed) return reconciliation::failed;
    if (claim == ownership::absent) return reconciliation::absent;
    const auto reclaimed_state = inspect(client, display_id, width, height, requested_only);
    if (reclaimed_state == inspection::matches) return reconciliation::reuse;
    if (reclaimed_state != inspection::replace_owned || !session_boundary) return reconciliation::failed;
    return remove() ? reconciliation::removed_retry : reconciliation::failed;
  }

  inline bool set_create_policy(driver::ControlClient &client, driver::CreateTemporaryDisplayRequest &request, bool requested_only) {
    if (!requested_only) return true;
    if (!descriptor_is_valid(request.width, request.height, request.refresh_rate_millihz) || !supported(client)) return false;
    request.flags |= requested_resolution_create_flag;
    return true;
  }
}  // namespace VDISPLAY::catalog_policy
