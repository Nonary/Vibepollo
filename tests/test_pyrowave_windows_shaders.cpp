// Compile every RGB conversion variant without requiring a GPU or NGX runtime.
#include <cstdio>
#include <d3dcompiler.h>
#include <filesystem>
#include <initializer_list>

int main(int argc, char **argv) {
  const std::filesystem::path path = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path(PYROWAVE_TEST_SHADER_PATH);
  const char *variants[] = {nullptr, "PQ", "LINEAR", "SDR_TO_PQ", "TRUEHDR_PEAK"};
  for (const char *variant : variants) {
    for (bool subsampled : {false, true}) {
      D3D_SHADER_MACRO macros[3] {};
      int count = 0;
      if (variant) {
        macros[count++] = {variant, "1"};
      }
      if (subsampled) {
        macros[count++] = {"SUBSAMPLE_420", "1"};
      }
      ID3DBlob *code = nullptr, *errors = nullptr;
      const auto result = D3DCompileFromFile(path.c_str(), macros, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main_cs", "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
      if (FAILED(result)) {
        std::fprintf(stderr, "%s %s shader failed: 0x%08lx\n", variant ? variant : "UNORM", subsampled ? "420" : "444", static_cast<unsigned long>(result));
        if (errors) {
          std::fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        }
      }
      if (code) {
        code->Release();
      }
      if (errors) {
        errors->Release();
      }
      if (FAILED(result)) {
        return 1;
      }
    }
  }
  std::puts("All 10 PyroWave compute shader variants compiled");
  return 0;
}
