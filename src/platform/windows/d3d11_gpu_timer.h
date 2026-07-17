/**
 * @file src/platform/windows/d3d11_gpu_timer.h
 * @brief Non-blocking D3D11 GPU timestamp ring for low-overhead telemetry.
 */
#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include <d3d11.h>
#include <wrl/client.h>

namespace platf::dxgi {

  /**
   * Timestamp a short section of work without ever waiting for the GPU. Completed
   * samples are read several frames later with DONOTFLUSH; if the ring is full,
   * telemetry is skipped rather than perturbing the capture/encode path.
   */
  class d3d11_gpu_timer_t {
  public:
    bool begin(ID3D11Device *device, ID3D11DeviceContext *context) {
      if (!device || !context || active_slot_ || query_creation_failed_) {
        return false;
      }

      for (std::size_t offset = 0; offset < slots_.size(); ++offset) {
        const auto index = (next_slot_ + offset) % slots_.size();
        auto &slot = slots_[index];
        if (slot.pending || !ensure_queries(device, slot)) {
          continue;
        }

        context->Begin(slot.disjoint.Get());
        context->End(slot.start.Get());
        active_slot_ = index;
        next_slot_ = (index + 1) % slots_.size();
        return true;
      }
      return false;
    }

    void end(ID3D11DeviceContext *context) {
      if (!context || !active_slot_) {
        return;
      }

      auto &slot = slots_[*active_slot_];
      context->End(slot.end.Get());
      context->End(slot.disjoint.Get());
      slot.pending = true;
      active_slot_.reset();
    }

    std::vector<double> poll(ID3D11DeviceContext *context) {
      std::vector<double> completed;
      if (!context) {
        return completed;
      }

      for (auto &slot : slots_) {
        if (!slot.pending) {
          continue;
        }

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint {};
        const auto disjoint_status = context->GetData(
          slot.disjoint.Get(),
          &disjoint,
          sizeof(disjoint),
          D3D11_ASYNC_GETDATA_DONOTFLUSH
        );
        if (disjoint_status == S_FALSE) {
          continue;
        }
        if (FAILED(disjoint_status)) {
          slot.pending = false;
          continue;
        }

        UINT64 start = 0;
        UINT64 end = 0;
        const auto start_status = context->GetData(
          slot.start.Get(),
          &start,
          sizeof(start),
          D3D11_ASYNC_GETDATA_DONOTFLUSH
        );
        const auto end_status = context->GetData(
          slot.end.Get(),
          &end,
          sizeof(end),
          D3D11_ASYNC_GETDATA_DONOTFLUSH
        );
        if (start_status == S_FALSE || end_status == S_FALSE) {
          continue;
        }

        slot.pending = false;
        if (FAILED(start_status) || FAILED(end_status) || disjoint.Disjoint ||
            disjoint.Frequency == 0 || end < start) {
          continue;
        }

        completed.push_back(
          static_cast<double>(end - start) * 1000.0 /
          static_cast<double>(disjoint.Frequency)
        );
      }
      return completed;
    }

    void reset() {
      active_slot_.reset();
      next_slot_ = 0;
      query_creation_failed_ = false;
      for (auto &slot : slots_) {
        slot = {};
      }
    }

  private:
    struct slot_t {
      Microsoft::WRL::ComPtr<ID3D11Query> disjoint;
      Microsoft::WRL::ComPtr<ID3D11Query> start;
      Microsoft::WRL::ComPtr<ID3D11Query> end;
      bool pending = false;
    };

    bool ensure_queries(ID3D11Device *device, slot_t &slot) {
      if (slot.disjoint && slot.start && slot.end) {
        return true;
      }

      D3D11_QUERY_DESC desc {};
      desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
      if (FAILED(device->CreateQuery(&desc, slot.disjoint.ReleaseAndGetAddressOf()))) {
        query_creation_failed_ = true;
        return false;
      }
      desc.Query = D3D11_QUERY_TIMESTAMP;
      if (FAILED(device->CreateQuery(&desc, slot.start.ReleaseAndGetAddressOf())) ||
          FAILED(device->CreateQuery(&desc, slot.end.ReleaseAndGetAddressOf()))) {
        slot = {};
        query_creation_failed_ = true;
        return false;
      }
      return true;
    }

    static constexpr std::size_t QUERY_RING_SIZE = 8;
    std::array<slot_t, QUERY_RING_SIZE> slots_;
    std::optional<std::size_t> active_slot_;
    std::size_t next_slot_ = 0;
    // Query support/resource failures are generally persistent for this device.
    // Latch them until reset() so telemetry cannot issue eight failed driver calls
    // per frame indefinitely under memory pressure or on unsupported hardware.
    bool query_creation_failed_ = false;
  };

}  // namespace platf::dxgi
