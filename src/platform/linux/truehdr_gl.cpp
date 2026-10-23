#include "truehdr_gl.h"

#include "cuda_interop.h"

#include <array>
#include <dlfcn.h>
#include <stdexcept>
#include <string>

namespace platf::linux_truehdr {
  namespace {
    void check(CUresult result, const char *operation) {
      if (result != CUDA_SUCCESS) {
        throw std::runtime_error(std::string("RTX HDR CUDA: ") + operation + " (" + std::to_string(result) + ")");
      }
    }

    constexpr const char *vertex = R"GLSL(#version 300 es
precision highp float;
out vec2 uv;
void main() {
  vec2 p=vec2(float(gl_VertexID >> 1), float(gl_VertexID & 1));
  gl_Position=vec4(p*4.0-1.0,0.0,1.0); uv=p*2.0;
})GLSL";
    constexpr const char *fragment = R"GLSL(#version 300 es
precision highp float;
uniform sampler2D source_image;
uniform sampler2D sdr_lut;
uniform bool fallback;
uniform float white_nits;
in vec2 uv;
out vec4 pixel;
float lookup(float x,int channel) {
  int n=textureSize(sdr_lut,0).x;
  float p=clamp(x,0.0,1.0)*float(n-1);
  int a=int(floor(p));
  return mix(texelFetch(sdr_lut,ivec2(a,0),0)[channel],
    texelFetch(sdr_lut,ivec2(min(a+1,n-1),0),0)[channel],fract(p));
}
void main() {
  vec3 c=texture(source_image,uv).rgb;
  c=vec3(lookup(c.r,0),lookup(c.g,1),lookup(c.b,2));
  if(fallback) {
    c=mix(c/12.92,pow((c+0.055)/1.055,vec3(2.4)),greaterThan(c,vec3(0.04045)));
    c=mat3(0.627404,0.069097,0.016391, 0.329283,0.919540,0.088013, 0.043313,0.011362,0.895595)*c;
    vec3 p=pow(max(c,vec3(0.0))*white_nits/10000.0,vec3(2610.0/16384.0));
    c=pow((3424.0/4096.0+(2413.0/128.0)*p)/(1.0+(2392.0/128.0)*p),vec3(2523.0/32.0));
  }
  pixel=vec4(c,1.0);
})GLSL";
  }  // namespace

  struct gl_converter_t::impl_t {
    GladGLContext &gl;
    CudaFunctions &cuda;
    CUstream stream;
    int width, height;
    GLuint textures[2] {}, buffer = 0, framebuffer = 0, program = 0;
    CUgraphicsResource input_resource = nullptr, output_resource = nullptr;
    std::unique_ptr<effect_t> effect;
    bool owns_gl = true;

    impl_t(GladGLContext &gl, CudaFunctions &cuda, CUstream stream, int width, int height):
        gl(gl),
        cuda(cuda),
        stream(stream),
        width(width),
        height(height) {}

    ~impl_t() {
      cuda.cuStreamSynchronize(stream);
      effect.reset();
      if (owns_gl) {
        if (input_resource) {
          cuda.cuGraphicsUnregisterResource(input_resource);
        }
        if (output_resource) {
          cuda.cuGraphicsUnregisterResource(output_resource);
        }
        if (buffer) {
          gl.DeleteBuffers(1, &buffer);
        }
        if (framebuffer) {
          gl.DeleteFramebuffers(1, &framebuffer);
        }
        if (program) {
          gl.DeleteProgram(program);
        }
        gl.DeleteTextures(2, textures);
      }
    }

    GLuint shader(GLenum type, const char *source) {
      const GLuint shader = gl.CreateShader(type);
      gl.ShaderSource(shader, 1, &source, nullptr);
      gl.CompileShader(shader);
      GLint status = 0;
      gl.GetShaderiv(shader, GL_COMPILE_STATUS, &status);
      if (!status) {
        std::array<char, 2048> log {};
        gl.GetShaderInfoLog(shader, log.size(), nullptr, log.data());
        gl.DeleteShader(shader);
        throw std::runtime_error(std::string("RTX HDR shader: ") + log.data());
      }
      return shader;
    }

    void init(const std::filesystem::path &runtime) {
      effect = std::make_unique<effect_t>(runtime, width, height, stream);
      const GLuint vs = shader(GL_VERTEX_SHADER, vertex);
      GLuint fs = 0;
      try {
        fs = shader(GL_FRAGMENT_SHADER, fragment);
      } catch (...) {
        gl.DeleteShader(vs);
        throw;
      }
      program = gl.CreateProgram();
      gl.AttachShader(program, vs);
      gl.AttachShader(program, fs);
      gl.LinkProgram(program);
      gl.DeleteShader(vs);
      gl.DeleteShader(fs);
      GLint linked = 0;
      gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
      if (!linked) {
        throw std::runtime_error("RTX HDR: cannot link preprocessing shader");
      }
      gl.GenTextures(2, textures);
      for (int i = 0; i < 2; ++i) {
        gl.BindTexture(GL_TEXTURE_2D, textures[i]);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl.TexImage2D(GL_TEXTURE_2D, 0, i ? GL_RGB10_A2 : GL_RGBA8, width, height, 0, GL_RGBA, i ? GL_UNSIGNED_INT_2_10_10_10_REV : GL_UNSIGNED_BYTE, nullptr);
      }
      gl.GenFramebuffers(1, &framebuffer);
      gl.GenBuffers(1, &buffer);
      gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
      gl.BufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(width) * height * 4, nullptr, GL_STREAM_DRAW);
      gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      check(cuda.cuGraphicsGLRegisterImage(&input_resource, textures[0], GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY), "register RGBA8 input");
      // CUDA does not support registering RGB10_A2 textures. A persistent PBO
      // transfers the packed SDK output to GL without any CPU readback.
      // FFmpeg's deliberately small CUDA table does not include this GL entry.
      const auto register_buffer = reinterpret_cast<CUresult (*)(CUgraphicsResource *, unsigned, unsigned)>(
        dlsym(RTLD_DEFAULT, "cuGraphicsGLRegisterBuffer")
      );
      if (!register_buffer) {
        throw std::runtime_error("RTX HDR: CUDA buffer interop is unavailable");
      }
      check(register_buffer(&output_resource, buffer, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD), "register HDR output buffer");
    }

    GLuint convert(GLuint source, GLuint lut, bool enabled, int brightness, const settings_t &settings) {
      gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
      gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[enabled ? 0 : 1], 0);
      const GLenum attachment = GL_COLOR_ATTACHMENT0;
      gl.DrawBuffers(1, &attachment);
      if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("RTX HDR: incomplete preprocessing framebuffer");
      }
      gl.UseProgram(program);
      gl.ActiveTexture(GL_TEXTURE0);
      gl.BindTexture(GL_TEXTURE_2D, source);
      gl.ActiveTexture(GL_TEXTURE1);
      gl.BindTexture(GL_TEXTURE_2D, lut);
      gl.Uniform1i(gl.GetUniformLocation(program, "source_image"), 0);
      gl.Uniform1i(gl.GetUniformLocation(program, "sdr_lut"), 1);
      gl.Uniform1i(gl.GetUniformLocation(program, "fallback"), !enabled);
      gl.Uniform1f(gl.GetUniformLocation(program, "white_nits"), 100.0f + brightness);
      // Dithering an already quantized SDR image changes the SDK input bytes
      // between draws, including when only a tuning slider changed.
      gl.Disable(GL_DITHER);
      gl.Viewport(0, 0, width, height);
      gl.Disable(GL_BLEND);
      gl.DrawArrays(GL_TRIANGLES, 0, 3);
      gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
      gl.Flush();
      if (enabled) {
        CUgraphicsResource resources[] {input_resource, output_resource};
        cuda::graphics_mapping_t mapping(cuda, resources, 2, stream);
        check(mapping.map(), "map effect resources");
        CUDA_MEMCPY2D copy {};
        copy.srcMemoryType = CU_MEMORYTYPE_ARRAY;
        check(cuda.cuGraphicsSubResourceGetMappedArray(&copy.srcArray, input_resource, 0, 0), "get input array");
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = effect->input();
        copy.dstPitch = effect->input_pitch();
        copy.WidthInBytes = width * 4;
        copy.Height = height;
        check(cuda.cuMemcpy2DAsync(&copy, stream), "copy SDR input");
        effect->run(settings);
        CUdeviceptr output = 0;
        std::size_t bytes = 0;
        check(cuda.cuGraphicsResourceGetMappedPointer(&output, &bytes, output_resource), "get HDR output pointer");
        if (bytes < static_cast<std::size_t>(width) * height * 4) {
          throw std::runtime_error("RTX HDR: output buffer is too small");
        }
        copy = {};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = effect->output();
        copy.srcPitch = effect->output_pitch();
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = output;
        copy.dstPitch = width * 4;
        copy.WidthInBytes = width * 4;
        copy.Height = height;
        check(cuda.cuMemcpy2DAsync(&copy, stream), "copy HDR output");
        check(mapping.unmap(), "unmap effect resources");
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
        gl.ActiveTexture(GL_TEXTURE0);
        gl.BindTexture(GL_TEXTURE_2D, textures[1]);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, nullptr);
        gl.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
      }
      gl.ActiveTexture(GL_TEXTURE0);
      if (gl.GetError() != GL_NO_ERROR) {
        throw std::runtime_error("RTX HDR: OpenGL conversion failed");
      }
      return textures[1];
    }
  };

  gl_converter_t::gl_converter_t(GladGLContext &gl, CudaFunctions &cuda, CUstream stream, const std::filesystem::path &runtime, int width, int height):
      impl(std::make_unique<impl_t>(gl, cuda, stream, width, height)) {
    impl->init(runtime);
  }

  gl_converter_t::~gl_converter_t() = default;

  GLuint gl_converter_t::convert(GLuint source, GLuint lut, bool enabled, int brightness, const settings_t &settings) {
    return impl->convert(source, lut, enabled, brightness, settings);
  }

  void gl_converter_t::abandon_gl() {
    impl->owns_gl = false;
  }

  GLuint gl_converter_t::prepared_sdr_texture() const {
    return impl->textures[0];
  }
}  // namespace platf::linux_truehdr
