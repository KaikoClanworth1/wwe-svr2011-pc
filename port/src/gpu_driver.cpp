// WWE SmackDown vs. Raw 2011 - custom GPU drivers on Android (gpu_driver.h).
//
// The launcher keeps driver packages (Mesa Turnip and the like: a zip with
// meta.json and the driver's .so) in the app's own storage and writes the
// chosen one's path to gpu_driver. When the SDK creates its Vulkan instance
// it asks this opener for the loader: libadrenotools loads the phone's
// libvulkan in a namespace where it loads that driver instead of the phone's
// (Android's loader keeps doing the swapchain). The hook libraries are in the
// app's native library folder (GameActivity passes it as
// SVR2011_NATIVE_LIB_DIR). Any failure: the phone's own driver - and a note
// (gpu_driver_failed.txt in the app's cache) the launcher shows the player.

#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#if SVR2011_ADRENOTOOLS
#include <dlfcn.h>
#include <unistd.h>

#include <adrenotools/driver.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#endif

REXCVAR_DEFINE_STRING(gpu_driver, "", "GPU",
                      "Android: a custom Vulkan driver's .so (the launcher's Settings set it); empty: the phone's own");

// (rex/ui/vulkan/instance.h, declared here: that header needs RenderDoc's)
namespace rex::ui::vulkan {
using LoaderOpener = void* (*)();
void SetLoaderOpener(LoaderOpener opener);
}  // namespace rex::ui::vulkan

namespace svr2011 {
namespace {

#if SVR2011_ADRENOTOOLS
// Can the driver make a Vulkan instance? (Some load and then fail there - a
// Qualcomm driver for another GPU: ErrorOutOfHostMemory. The game would be
// left without a picture.)
VkResult ProbeDriver(void* handle) {
  auto get_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle, "vkGetInstanceProcAddr"));
  if (!get_proc) return VK_ERROR_INITIALIZATION_FAILED;
  auto create = reinterpret_cast<PFN_vkCreateInstance>(get_proc(nullptr, "vkCreateInstance"));
  if (!create) return VK_ERROR_INITIALIZATION_FAILED;
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  VkResult result = create(&info, nullptr, &instance);
  if (result != VK_SUCCESS) return result;
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get_proc(instance, "vkDestroyInstance"));
  if (destroy) destroy(instance, nullptr);
  return VK_SUCCESS;
}

// The launcher's note: this driver didn't work (it offers the phone's own back).
void NoteFailure(const std::string& path, const char* why) {
  const char* cache = std::getenv("SVR2011_CACHE_DIR");
  if (!cache) return;
  if (FILE* f = std::fopen((std::string(cache) + "/gpu_driver_failed.txt").c_str(), "w")) {
    std::fprintf(f, "%s\n%s\n", path.c_str(), why);
    std::fclose(f);
  }
}
#endif

void* OpenVulkanLoader() {
#if SVR2011_ADRENOTOOLS
  const std::string path = REXCVAR_GET(gpu_driver);
  if (path.empty()) return nullptr;
  const size_t slash = path.rfind('/');
  const char* hooks = std::getenv("SVR2011_NATIVE_LIB_DIR");
  const char* tmp = std::getenv("SVR2011_CACHE_DIR");
  if (slash == std::string::npos || !hooks || access(path.c_str(), R_OK) != 0) {
    REXLOG_WARN("custom GPU driver {} can't be used (missing?) - the phone's own driver", path);
    NoteFailure(path, "missing");
    return nullptr;
  }
  const std::string dir = path.substr(0, slash + 1), name = path.substr(slash + 1);
  std::string hook_dir = hooks;
  if (!hook_dir.empty() && hook_dir.back() != '/') hook_dir += '/';
  void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp, hook_dir.c_str(), dir.c_str(),
                                            name.c_str(), nullptr, nullptr);
  if (!handle) {
    const char* error = dlerror();
    REXLOG_WARN("custom GPU driver {} didn't load: {} - the phone's own driver", name, error ? error : "?");
    NoteFailure(path, "didn't load");
    return nullptr;
  }
  if (const VkResult result = ProbeDriver(handle); result != VK_SUCCESS) {
    // (left loaded: unloading a driver half set up isn't safe; it is unused)
    REXLOG_WARN("custom GPU driver {} can't start Vulkan (VkResult {}) - the phone's own driver", name,
                static_cast<int>(result));
    NoteFailure(path, "can't start Vulkan");
    return nullptr;
  }
  REXLOG_INFO("custom GPU driver: {}", path);
  return handle;
#else
  return nullptr;
#endif
}

// (registered when the library loads: before the SDK creates its Vulkan instance)
const bool g_registered = (rex::ui::vulkan::SetLoaderOpener(&OpenVulkanLoader), true);

}  // namespace
}  // namespace svr2011
