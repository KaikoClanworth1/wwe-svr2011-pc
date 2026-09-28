// The PC port's changes to the installed disc files (text and menus), made
// at startup; see game_files.cpp.
#pragma once

#include <filesystem>

namespace svr2011 {

void PatchGameFiles(const std::filesystem::path& game_dir);

}  // namespace svr2011
