/**
 * @file src/platform/linux/gpu_snapshot.h
 * @brief Copy an external RGB DMA-BUF into a GPU-renderable DMA-BUF.
 */
#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include "dmabuf_surface.h"

namespace gbm {
  struct device;
}

namespace egl {
  class gpu_snapshot_t {
  public:
    gpu_snapshot_t();
    ~gpu_snapshot_t();

    using import_t = std::function<bool(const surface_descriptor_t &)>;
    // The consumer imports each candidate with its original context current.
    // A successful buffer is borrowed until the next copy on this instance.
    bool copy(void *display, gbm::device *device, const surface_descriptor_t &source, const import_t &import);
    bool has_output() const;

  private:
    std::chrono::steady_clock::time_point retry_after {};
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };
}
