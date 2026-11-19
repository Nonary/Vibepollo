"""Run each production KMS frame-preparation path with deterministic driver failures."""

from pathlib import Path
import subprocess
import sys
import tempfile


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    for end in range(opening + 1, len(source)):
        depth += (source[end] == "{") - (source[end] == "}")
        if depth == 0:
            return source[start:end + 1]
    raise ValueError(signature)


root = Path(sys.argv[1]).resolve()
platform = root / "src/platform/linux"
read = lambda name: (platform / name).read_text()
graphics = method(read("graphics.cpp"), "std::optional<rgb_t> rgb_import_t::prepare")
vulkan = method(read("vulkan_encode.cpp"), "bool prepare_source(")
pyro = method(read("pyrowave_core.cpp"), "Vulkan::ImageHandle prepare_surface(")

# Exercise the frame/sequence gate used by each GL encoder, including the
# actual call into rgb_import_t. Stop at color conversion after import.
methods = []
for name, file, owner in [
    ("nvenc", "cuda.cpp", "class gl_cuda_nvenc_t"),
    ("ffmpeg_cuda", "cuda.cpp", "class gl_cuda_vram_t"),
    ("vaapi", "vaapi.cpp", "class va_vram_t"),
]:
    convert = method(read(file).split(owner, 1)[1], "int convert(")
    start = convert.index("if (descriptor.sequence == 0)")
    end = convert.index("sws.load_vram(", start)
    methods.append(f"int {name}() {{ {convert[start:end]} return rgb; }}")

ram = method(read("kmsgrab.cpp").split("class display_ram_t:", 1)[1], "capture_e snapshot(")
ram = ram[ram.index("auto rgb_opt ="):ram.index("auto &rgb =")]
assert "prepare_source(descriptor.sd, submission_slot)" in method(read("vulkan_encode.cpp"), "int convert(")
assert "d.prepare_surface(s," in method(read("pyrowave_core.cpp"), "bool core_t::encode(")
assert "snapshot_device.copy(snapshot, snapshot_render_device, frame, import)" in read("pyrowave_encode.cpp")

program = r"""
#include "src/platform/linux/dmabuf_surface.h"
#include <cassert>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <optional>
#include <string>

using Surface = egl::surface_descriptor_t;
int tier;
std::string calls;
const Surface *original;
int direct(const Surface &s) {
  if (&s == original) { calls += 'D'; return tier == 1 ? 1 : 0; }
  calls += 'I';
  return tier == 2 ? 2 : 0;
}
template<class F> bool snapshot_copy(const Surface &s, const F &accept) {
  assert(&s == original);
  calls += 'G';
  auto output = s;
  return accept(output);
}
int upload(const Surface &s) { assert(&s == original); calls += 'C'; return 3; }
struct Handle { int *get() const { return nullptr; } };
namespace gbm { using device = int; }
namespace gl { struct { void Finish() {} } ctx; }
int eglGetCurrentContext() { return 1; }
int eglQueryAPI() { return 1; }
#define BOOST_LOG(level) std::clog
namespace egl {
  struct display_t { using pointer = int*; };
  using rgb_t = int;
  struct rgb_import_t {
    struct { bool has_output() { return false; }
      bool copy(int*, int*, const Surface &s, const auto &accept) { return snapshot_copy(s, accept); }
    } snapshot;
    bool snapshot_logged = false;
    bool direct_logged = false;
    std::optional<int> prepare(int*, int*, const Surface&);
  };
  std::optional<int> import_source(int*, const Surface &s) {
    if (int result = direct(s)) return result;
    return std::nullopt;
  }
  std::optional<int> upload_source(int*, const Surface &s) { return upload(s); }
  int create_blank(int) { return 0; }
  @GRAPHICS@
}
struct Gl {
  egl::rgb_import_t rgb_import;
  Handle display, gbm;
  int img = 0, rgb = 0;
  unsigned sequence = 0;
  struct { Surface sd; unsigned sequence = 0; } descriptor;
  @METHODS@
  int ram() {
    struct { Surface &sd; } surface {descriptor.sd};
    struct capture_e { enum { error = -1 }; };
    @RAM@
    return *rgb_opt;
  }
};
struct Vk {
  struct { int sources[1]; } cmd;
  int snapshots[1], snapshot_node;
  struct { bool copy(int, int, const Surface &s, const auto &accept) { return snapshot_copy(s, accept); } } snapshot_device;
  bool import_dmabuf(const Surface &s, int &out) { return (out = direct(s)); }
  bool upload_source(const Surface &s, int &out) { return (out = upload(s)); }
  @VULKAN@
};
namespace Vulkan { using ImageHandle = std::shared_ptr<int>; }
namespace linear_upload { bool should_attempt_linear_upload(const Surface &s, bool ready) { return ready && s.modifier == 0; } }
struct Pyro {
  struct source_t { const Surface *surface; std::function<bool(const Surface&,const std::function<bool(const Surface&)>&)> gpu_snapshot; };
  Vulkan::ImageHandle import(const Surface &s, std::string&, bool &ready) { ready = true; int value = direct(s); return value ? std::make_shared<int>(value) : nullptr; }
  int format_for(unsigned) { return 0; }
  Vulkan::ImageHandle upload_linear_surface(const Surface &s, int, std::string&) { return std::make_shared<int>(upload(s)); }
  @PYRO@
};
int main() {
  Gl gl;
  Vk vk {};
  Pyro pyro;
  auto &s = gl.descriptor.sd;
  original = &s;
  s.direct_import_required = true;
  s.fds[0] = open("/dev/zero", O_RDONLY);
  auto exercise = [&](const char *name, auto run) {
    for (auto [route, modifier, expected, trace] : {
        std::tuple{1, 0ULL, 1, "D"}, {2, 0ULL, 2, "DGI"}, {3, 0ULL, 3, "DGIC"},
        {3, egl::IMPLICIT_DMA_BUF_MODIFIER + 0ULL, -1, "DGI"}, {3, 42ULL, -1, "DGI"},
        {2, 42ULL, 2, "DGI"}, {1, 0ULL, 1, "D"}}) {
      tier = route; s.modifier = modifier; ++gl.descriptor.sequence; calls.clear();
      assert(run() == expected && calls == trace);
    }
    std::cout << name << ": direct, GPU, CPU, unsafe layout rejection and recovery passed\n";
  };
  exercise("Native NVENC", [&] { return gl.nvenc(); });
  exercise("FFmpeg NVENC", [&] { return gl.ffmpeg_cuda(); });
  exercise("VAAPI", [&] { return gl.vaapi(); });
  exercise("Software KMS capture", [&] { return gl.ram(); });
  exercise("Vulkan Video", [&] { return vk.prepare_source(s, 0) ? vk.cmd.sources[0] : -1; });
  exercise("PyroWave", [&] {
    Pyro::source_t frame {&s, [](const auto &input,const auto &accept) { return snapshot_copy(input, accept); }};
    bool uploaded = false; std::string error;
    auto image = pyro.prepare_surface(frame, error, uploaded);
    assert(uploaded == (image && *image == 3));
    return image ? *image : -1;
  });
  close(s.fds[0]);
}
"""
program = "#include <memory>\n#include <tuple>\n" + program
for key, value in dict(GRAPHICS=graphics, METHODS="\n".join(methods), RAM=ram, VULKAN=vulkan, PYRO=pyro).items():
    program = program.replace("@" + key + "@", value)
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "routes.cpp"
    binary = Path(tmp) / "routes"
    source.write_text(program)
    subprocess.run([sys.argv[2], "-std=c++23", "-I", root, source, "-o", binary], check=True)
    subprocess.run([binary], check=True)
