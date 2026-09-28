// WWE SmackDown vs. Raw 2011 - saves as plain files (saves.h).

#include "saves.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

namespace svr2011 {

namespace {

namespace fs = std::filesystem;

constexpr const char* kTitleDir = "5451085D";
constexpr const char* kSavedGameDir = "00000001";

bool IsProfileDir(const fs::path& p) {
  const std::string name = p.filename().string();
  if (name.size() != 16 || name == "0000000000000000") return false;
  for (char c : name)
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

// One package folder -> <saves>\<name> (its single file) or, with more files,
// the folder itself; header and thumbnail -> <saves>\.info.
void MovePackage(const fs::path& package, const fs::path& header, const fs::path& saves) {
  const std::string name = package.filename().string();
  const fs::path info = saves / ".info";
  std::error_code ec;
  if (fs::exists(saves / name)) {
    REXLOG_WARN("[svr2011] saves: {} is already in {}; the old copy stays in {}", name,
                saves.string(), package.string());
    return;
  }
  std::vector<fs::path> files;
  bool dirs = false;
  for (const auto& e : fs::directory_iterator(package, ec)) {
    if (e.is_directory()) {
      dirs = true;
    } else if (e.path().filename() == "__thumbnail.png") {
      fs::rename(e.path(), info / (name + ".png"), ec);
    } else {
      files.push_back(e.path());
    }
  }
  if (files.size() == 1 && !dirs) {
    fs::rename(files[0], saves / name, ec);
    if (ec) {
      REXLOG_WARN("[svr2011] saves: could not move {} ({})", files[0].string(), ec.message());
      return;
    }
    const std::string inner = files[0].filename().string();
    if (inner != "SaveData.Dat") {
      if (FILE* f = std::fopen((info / (name + ".file")).string().c_str(), "wb")) {
        std::fwrite(inner.data(), 1, inner.size(), f);
        std::fclose(f);
      }
    }
    fs::remove(package, ec);
  } else {
    fs::rename(package, saves / name, ec);
    if (ec) {
      REXLOG_WARN("[svr2011] saves: could not move {} ({})", package.string(), ec.message());
      return;
    }
  }
  if (fs::exists(header)) fs::rename(header, info / (name + ".header"), ec);
  REXLOG_INFO("[svr2011] saves: moved {} to {}", name, saves.string());
}

}  // namespace

void UseFlatSaves(rex::system::KernelState* kernel_state, const fs::path& user_data,
                  const fs::path& saves) {
  std::error_code ec;
  fs::create_directories(saves / ".info", ec);
  for (const auto& profile : fs::directory_iterator(user_data, ec)) {
    if (!profile.is_directory() || !IsProfileDir(profile.path())) continue;
    const fs::path packages = profile.path() / kTitleDir / kSavedGameDir;
    const fs::path headers = profile.path() / kTitleDir / "Headers" / kSavedGameDir;
    std::vector<fs::path> list;
    for (const auto& p : fs::directory_iterator(packages, ec))
      if (p.is_directory()) list.push_back(p.path());
    for (const auto& p : list) MovePackage(p, headers / (p.filename().string() + ".header"), saves);
  }
  if (kernel_state && kernel_state->content_manager()) {
    kernel_state->content_manager()->SetFlatSavesRoot(saves);
    REXLOG_INFO("[svr2011] saves: {}", saves.string());
  }
}

}  // namespace svr2011
