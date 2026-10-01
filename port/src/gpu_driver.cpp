// WWE SmackDown vs. Raw 2011 - custom GPU drivers on Android (gpu_driver.h).
//
// The launcher keeps driver packages (Mesa Turnip and the like: a zip with
// meta.json and the driver's .so) in the app's own storage and writes the
// chosen one's path to gpu_driver. When the SDK creates its Vulkan instance
// it asks this opener for the loader: libadrenotools loads the phone's
// libvulkan in a namespace where it loads that driver instead of the phone's
// (Android's loader keeps doing the swapchain). The hook libraries are in the
// app's native library folder (GameActivity passes it as
// SVR2011_NATIVE_LIB_DIR). Any failure: the phone's own driver.

#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

#if SVR2011_ADRENOTOOLS
#include <dlfcn.h>
#include <unistd.h>

#include <adrenotools/driver.h>
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

void* OpenVulkanLoader() {
#if SVR2011_ADRENOTOOLS
  const std::string path = REXCVAR_GET(gpu_driver);
  if (path.empty()) return nullptr;
  const size_t slash = path.rfind('/');
  const char* hooks = std::getenv("SVR2011_NATIVE_LIB_DIR");
  const char* tmp = std::getenv("SVR2011_CACHE_DIR");
  if (slash == std::string::npos || !hooks || access(path.c_str(), R_OK) != 0) {
    REXLOG_WARN("custom GPU driver {} can't be used (missing?) - the phone's own driver", path);
    return nullptr;
  }
  const std::string dir = path.substr(0, slash + 1), name = path.substr(slash + 1);
  std::string hook_dir = hooks;
  if (!hook_dir.empty() && hook_dir.back() != '/') hook_dir += '/';
  void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp, hook_dir.c_str(), dir.c_str(),
                                            name.c_str(), nullptr, nullptr);
  if (!handle) {
    REXLOG_WARN("custom GPU driver {} didn't load: {} - the phone's own driver", name, dlerror() ? dlerror() : "?");
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
