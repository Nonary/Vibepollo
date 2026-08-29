"""Exercise the production black-frame producer used by Remote Input."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/platform/common.h').read_text()
start = source.index('    capture_e capture_synthetic_black(')
end = source.index('    virtual std::unique_ptr<avcodec_encode_device_t>', start)
method = source[start:end]
with tempfile.TemporaryDirectory(prefix='synthetic-capture-') as directory:
    path = Path(directory)
    (path / 'test.cpp').write_text(r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
enum class capture_e {ok,error};
struct img_t {
 std::optional<std::chrono::steady_clock::time_point> frame_timestamp,host_processing_timestamp;
};
struct display {
 using push_captured_image_cb_t=std::function<bool(std::shared_ptr<img_t>&&,bool)>;
 using pull_free_image_cb_t=std::function<bool(std::shared_ptr<img_t>&)>;
 int dummy_result=0,dummy_calls=0;
 int dummy_img(img_t*) { ++dummy_calls; return dummy_result; }
''' + method + r'''
};
int main() {
 display d;
 int pushed=0;
 auto image=std::make_shared<img_t>();
 auto pull=[&](auto& out){out=image;return true;};
 auto stop=[&](auto&&,bool){++pushed;return false;};
 // Invalid and zero rates remain safe, and stopping never requires a sleep.
 for(int rate:{-1,0,60,144}) {
  auto before=std::chrono::steady_clock::now();
  assert(d.capture_synthetic_black(stop,pull,rate)==capture_e::ok);
  assert(image->frame_timestamp && *image->frame_timestamp>=before);
  assert(image->host_processing_timestamp==image->frame_timestamp);
 }
 assert(pushed==4);
 d.dummy_result=-1;
 assert(d.capture_synthetic_black(stop,pull,60)==capture_e::error);
 assert(pushed==4);
 d.dummy_result=0;
 auto cancelled=[](auto&){return false;};
 auto empty=[](auto& out){out.reset();return true;};
 assert(d.capture_synthetic_black(stop,cancelled,60)==capture_e::ok);
 assert(d.capture_synthetic_black(stop,empty,60)==capture_e::ok);
 assert(pushed==4);
 // Reusing an image must publish a fresh timestamp on each frame.
 int frames=0;
 std::optional<std::chrono::steady_clock::time_point> previous;
 auto two_frames=[&](auto&& out,bool captured){
  assert(captured && out==image);
  assert(out->frame_timestamp && (!previous || out->frame_timestamp>previous));
  previous=out->frame_timestamp;
  return ++frames<2;
 };
 assert(d.capture_synthetic_black(two_frames,pull,144)==capture_e::ok);
 assert(frames==2);
}
''')
    subprocess.run(['g++', '-std=c++17', str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
print('PASS: Remote Input frame timestamps, reused images, cancellation, and initialization failure')
