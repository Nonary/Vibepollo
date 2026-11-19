"""Run the production X11 enumeration and capture initialization with a disappearing monitor."""
from pathlib import Path
import subprocess
import sys
import tempfile

source = (Path(sys.argv[1]) / "src/platform/linux/x11grab.cpp").read_text()


def method(marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    for pos in range(opening + 1, len(source)):
        depth += (source[pos] == "{") - (source[pos] == "}")
        if not depth:
            return source[start:pos + 1]
    raise ValueError(marker)


program = r"""
#include <cassert>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
using namespace std::literals;
#define BOOST_LOG(level) std::ostringstream()
constexpr int RR_Connected = 0;
struct resources { int noutput = 1; int outputs[1] {0}; };
struct output { const char *name = "HDMI-A-1"; int nameLen = 8, crtc = 1, connection = RR_Connected; };
struct crtc { int width = 1920, height = 1080, x = 120, y = 30; };
using screen_res_t = std::unique_ptr<resources>;
using output_info_t = std::unique_ptr<output>;
using crtc_info_t = std::unique_ptr<crtc>;
enum class missing { none, resources, output, crtc };
missing fault {};
namespace x11 {
  using xdisplay_t = std::unique_ptr<int>;
  int *OpenDisplay(const char *) { return new int {}; }
  namespace rr {
    resources *GetScreenResources(int *, int) { return fault == missing::resources ? nullptr : new resources; }
    output *GetOutputInfo(int *, resources *, int) { return fault == missing::output ? nullptr : new output; }
    crtc *GetCrtcInfo(int *, resources *, int) { return fault == missing::crtc ? nullptr : new crtc; }
  }
}
int DefaultRootWindow(int *) { return 1; }
int load_x11() { return 0; }
int load_xcb() { return 0; }
namespace util { int from_view(const std::string &v) { return std::stoi(v); } }
namespace video { struct config_t { int framerate = 60; }; }
struct display {
  x11::xdisplay_t xdisplay {x11::OpenDisplay(nullptr)};
  int xwindow {}, width {}, height {}, offset_x {}, offset_y {}, env_width {}, env_height {};
  std::chrono::nanoseconds delay {};
  std::string captured_connector;
  struct { int width = 2560, height = 1440; } xattr;
  void refresh() {}
"""
program += method("    int init(const std::string &display_name, const ::video::config_t &config)") + "\n};\n"
program += method("  std::vector<std::string> x11_display_names(bool connector_names)")
program += """
int main() {
  for (const auto lost : {missing::resources, missing::output, missing::crtc}) {
    fault = lost;
    display attempt;
    // A CRTC may disappear between successful enumeration and opening the monitor.
    assert(x11_display_names(true).empty() == (lost != missing::crtc));
    assert(attempt.init("0", {}) < 0 && attempt.captured_connector.empty());
    assert(attempt.init("", {}) == 0 && attempt.width == 2560 && attempt.height == 1440);
  }
  fault = missing::none;
  assert(x11_display_names(false) == std::vector<std::string> {"0"});
  assert(x11_display_names(true) == std::vector<std::string> {"HDMI-A-1"});
  display active;
  assert(active.init("0", {}) == 0 && active.captured_connector == "HDMI-A-1");
  assert(active.width == 1920 && active.height == 1080 && active.offset_x == 120 && active.offset_y == 30);
}
"""
with tempfile.TemporaryDirectory() as work:
    cpp, binary = Path(work) / "x11.cpp", Path(work) / "x11"
    cpp.write_text(program)
    subprocess.run([sys.argv[2], "-std=c++20", cpp, "-o", binary], check=True)
    subprocess.run([binary], check=True)
