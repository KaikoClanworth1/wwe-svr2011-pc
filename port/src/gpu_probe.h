// WWE SmackDown vs. Raw 2011 - which graphics API this PC can draw the game with.

#pragma once

#include <filesystem>
#include <string>

namespace svr2011 {

// gpu_backend "any" on Windows: "d3d12" when Direct3D 12 can run the native
// renderer, else "vulkan" when Vulkan can, else "d3d11" (older GPUs and
// drivers). `why` gets the reasons the better ones were passed over (empty
// for "d3d12"). The probes (a device each, ~0.4 s) run when the GPU or its
// driver changed: the answer is kept in `cache_file` with their ids.
// `cached` tells whether it came from there.
std::string PickGraphicsApi(const std::filesystem::path& cache_file, std::string* why, bool* cached);

}  // namespace svr2011
