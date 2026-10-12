#include <windows.h>

int main(int argc, char **argv) {
  if (argc != 2) {
    return 1;
  }
  const auto layer = LoadLibraryA(argv[1]);
  if (!layer) {
    return 2;
  }
  using function_t = void (*)();
  using get_instance_proc_addr_t = function_t(__stdcall *)(void *, const char *);
  const auto get_proc = reinterpret_cast<get_instance_proc_addr_t>(GetProcAddress(layer, "vkGetInstanceProcAddr"));
  const bool exported = get_proc && get_proc(nullptr, "vkCreateInstance") &&
                        get_proc(nullptr, "vkGetPhysicalDeviceSurfaceFormatsKHR");
  FreeLibrary(layer);
  return exported ? 0 : 3;
}
