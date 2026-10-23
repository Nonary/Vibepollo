#pragma once
#include "truehdr.h"

#include <ffnvcodec/dynlink_loader.h>
#include <glad/gl.h>

namespace platf::linux_truehdr {
  class gl_converter_t {
  public:
    gl_converter_t(GladGLContext &gl, CudaFunctions &cuda, CUstream stream, const std::filesystem::path &runtime, int width, int height);
    ~gl_converter_t();
    // Source includes crop/cursor composition. Apply the scanout LUT in SDR,
    // before the effect; return PQ pixels for the ordinary HDR YUV conversion.
    GLuint convert(GLuint source, GLuint sdr_lut, bool enabled, int sdr_brightness, const settings_t &settings);
    GLuint prepared_sdr_texture() const;
    // Probe teardown on a thread without the owning EGL context follows the
    // same deferred context reclamation policy as the encoder's registrations.
    void abandon_gl();

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl;
  };
}  // namespace platf::linux_truehdr
