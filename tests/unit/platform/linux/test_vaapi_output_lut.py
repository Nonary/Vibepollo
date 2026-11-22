"""Exercise the VAAPI frame conversion and LUT uploader with inert GL edges.

The real production method bodies are compiled into the fixture; this is not an
AMD hardware, EGL, shader, or end-to-end pixel-output test. A negative control
removes only the LUT dispatch from the compiled body and must fail specifically
at the nonidentity-LUT check.
"""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1]).resolve()
compiler = sys.argv[2] if len(sys.argv) > 2 else "c++"


def function_body(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening:index + 1]
    raise ValueError(f"Unterminated function: {signature}")


convert = function_body(
    "src/platform/linux/vaapi.cpp",
    "int convert(platf::img_t &img) override {\n      auto &descriptor = (egl::img_descriptor_t &) img;",
)
lut = function_body("src/platform/linux/graphics.cpp", "void sws_t::apply_output_lut(")
call = "sws.apply_output_lut(descriptor.crtc_gamma_lut, descriptor.crtc_gamma_lut_serial);"
if convert.count(call) != 1:
    raise AssertionError("Expected exactly one LUT dispatch in the VAAPI conversion method")

fixture = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <memory>
#include <optional>
#include <vector>
using GLsizei=int;
constexpr int GL_TEXTURE1=1, GL_TEXTURE0=0, GL_TEXTURE_2D=2, GL_RGB16=3, GL_RGB=4, GL_UNSIGNED_SHORT=5;
namespace gl {
struct Context { int active=-1, uploads=0, width=0; std::vector<std::array<std::uint16_t,3>> data;
 void ActiveTexture(int v){active=v;}
 void BindTexture(int target,int texture){assert(target==GL_TEXTURE_2D && active==GL_TEXTURE1 && texture==99);}
 void TexImage2D(int target,int level,int internal,int w,int h,int border,int format,int type,const void* p) {
   assert(active==GL_TEXTURE1 && target==GL_TEXTURE_2D && level==0 && internal==GL_RGB16);
   assert(h==1 && border==0 && format==GL_RGB && type==GL_UNSIGNED_SHORT);
   width=w; ++uploads;
   auto b=static_cast<const std::array<std::uint16_t,3>*>(p); data.assign(b,b+w);
 }
}; Context ctx;
}
namespace platf { struct img_t {virtual ~img_t()=default;}; }
namespace egl {
struct img_descriptor_t: platf::img_t { using gamma_lut_t=std::vector<std::array<std::uint16_t,3>>; std::shared_ptr<const gamma_lut_t> crtc_gamma_lut; std::uint64_t crtc_gamma_lut_serial=0, sequence=0; int sd=0; };
struct R { std::array<int,1> tex{42}; }; using rgb_t=std::shared_ptr<R>;
rgb_t create_blank(platf::img_t&) { return std::make_shared<R>(); }
bool fail_import=false, upload_success=false;
std::optional<rgb_t> import_source(int,int) { if(fail_import) return {}; return std::make_shared<R>(); }
std::optional<rgb_t> upload_source(int,int) { if(upload_success) return std::make_shared<R>(); return {}; }
template<class I,class U> std::optional<rgb_t> import_source_with_cpu_fallback(int s,I i,U u) { auto r=i(s); return r ? r : u(s); }
struct sws_t { std::array<int,1> output_lut{99}; std::uint64_t output_lut_serial=std::numeric_limits<std::uint64_t>::max(); int loads=0, conversions=0, uploads_at_convert=0;
 void apply_output_lut(const std::shared_ptr<const img_descriptor_t::gamma_lut_t>& lut,std::uint64_t serial) LUT_BODY
 void load_vram(img_descriptor_t&,int,int,int){++loads;} void convert(int){++conversions; uploads_at_convert=gl::ctx.uploads;}
}; }
struct Target {int buf=0;}; struct Display {int get(){return 0;}};
struct VA { egl::sws_t sws; std::shared_ptr<Target> nv12=std::make_shared<Target>(); std::uint64_t sequence=0; egl::rgb_t rgb; Display display; int offset_x=0,offset_y=0;
 int convert(platf::img_t &img) CONVERT_BODY
};
int main(){
 using Lut=egl::img_descriptor_t::gamma_lut_t; VA va; va.sws.apply_output_lut({},0);
 assert(gl::ctx.uploads==1 && gl::ctx.width==2 && gl::ctx.data[0][0]==0 && gl::ctx.data[1][0]==65535 && gl::ctx.active==GL_TEXTURE0);
 egl::img_descriptor_t img; img.sequence=1; auto a=std::make_shared<Lut>(Lut{{{0,3,5}},{{10000,20000,30000}},{{65535,65000,64000}}}); img.crtc_gamma_lut=a; img.crtc_gamma_lut_serial=11;
 assert(va.convert(img)==0);
 if (!(gl::ctx.uploads==2 && gl::ctx.data==*a && va.sws.uploads_at_convert==2)) {
   std::fputs("nonidentity LUT missing before conversion\n", stderr);
   return 42;
 }
 assert(va.convert(img)==0 && gl::ctx.uploads==2); // same source sequence and serial
 auto b=std::make_shared<Lut>(Lut{{{0,0,0}},{{900,800,700}},{{50000,40000,30000}}}); img.crtc_gamma_lut=b; img.crtc_gamma_lut_serial=12;
 assert(va.convert(img)==0 && gl::ctx.uploads==3 && gl::ctx.data==*b && va.sws.uploads_at_convert==3); // changed LUT on same frame sequence
 img.crtc_gamma_lut.reset(); img.crtc_gamma_lut_serial=0;
 assert(va.convert(img)==0 && gl::ctx.uploads==4 && gl::ctx.width==2 && gl::ctx.data[1][0]==65535); // restore identity
 assert(va.convert(img)==0 && gl::ctx.uploads==4);
 // An invalid single-entry LUT with a new serial also falls back to identity.
 img.crtc_gamma_lut=std::make_shared<Lut>(Lut{{{5,6,7}}}); img.crtc_gamma_lut_serial=14;
 assert(va.convert(img)==0 && gl::ctx.uploads==5 && gl::ctx.width==2 && gl::ctx.data[1][0]==65535);
 img.sequence=0; img.crtc_gamma_lut=a; img.crtc_gamma_lut_serial=15;
 assert(va.convert(img)==0 && gl::ctx.data==*a); // dummy frame
 img.sequence=2; img.crtc_gamma_lut=b; img.crtc_gamma_lut_serial=16; egl::fail_import=true; egl::upload_success=true;
 assert(va.convert(img)==0 && gl::ctx.data==*b); // CPU-upload fallback gets the same LUT
 int before=gl::ctx.uploads, renders=va.sws.conversions; img.sequence=3; egl::upload_success=false;
 assert(va.convert(img)==-1 && gl::ctx.uploads==before && va.sws.conversions==renders); // failed import never renders
}
'''


def compile_and_run(directory, name, convert_body):
    code = fixture.replace("LUT_BODY", lut).replace("CONVERT_BODY", convert_body)
    source_path = directory / f"{name}.cpp"
    binary_path = directory / name
    source_path.write_text(code)
    subprocess.run(
        [compiler, "-std=c++23", "-Wall", "-Wextra", "-Werror", str(source_path), "-o", str(binary_path)],
        check=True,
    )
    return subprocess.run([str(binary_path)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix="vaapi-lut-test-") as temp:
    directory = pathlib.Path(temp)
    positive = compile_and_run(directory, "vaapi_lut", convert)
    if positive.returncode:
        raise AssertionError(f"Production fixture failed: {positive.stderr}")

    negative = compile_and_run(directory, "without_lut", convert.replace(call, ""))
    if negative.returncode != 42 or "nonidentity LUT missing before conversion" not in negative.stderr:
        raise AssertionError(f"LUT omission negative control failed unexpectedly: {negative.returncode}, {negative.stderr}")

print("VAAPI LUT conversion fixture and omission negative control passed")
