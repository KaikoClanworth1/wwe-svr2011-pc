// VS screen styles (see vs_styles.h).
//
// The VS screen (the match set-up page: both renders, "A VS. B", the menu row
// and "<show> TONIGHT") is drawn over the arena's VS theme: menu/MatchHD.pac
// MENU/M<nn>L (the YXML layout), M<nn>I (textures + ATB animations) and
// M<nn>F (the font), picked in sub_8271D868 (the arena's number, 60-66 for
// some match types). SvR 2008 - 2010 have the same system, so their themes
// play here as they are (tools/vs_port.py only moves their text lines to
// 2011's places, above the menu row).
//
// Each installed style (<game>/Mods/VsStyles/<style>/: manifest.txt with
// type=VsStyle and name=, vs.pac with group MENU holding M<nn>L / I / F under
// 2011's numbers; a "disabled" file turns one off) becomes a group of one
// overlay pac, Mods/SuperstarOverlay/vsstyles.pac (VS00, VS01, ...), mounted
// with the superstar mods' files. While vs_style names a style, the game's
// file name resolver is pointed at that style's /VSnn/M<nn>x instead of
// /MENU/M<nn>x - a theme the style hasn't stays 2011's (so do 60-66). The
// game resolves every theme's names once, at start (the VS screen only checks
// they exist), so the setting applies from the next start.
#include "vs_styles.h"
#include "no_mods.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "../modmaker/svrfmt/pac.h"
#include "superstar_mods.h"

REXCVAR_DEFINE_STRING(vs_style, "", "UI",
                      "The VS screen's style: an installed VS style's folder in Mods/VsStyles (svr2008, svr2009, "
                      "svr2010 - made by the Mod Maker from that game's disc); empty: 2011's own. From the next start");

namespace fs = std::filesystem;

namespace {

struct Style {
  std::string folder, name;
  std::map<std::string, uint32_t> alias;  // "MENU/M01L" -> guest "/VS00/M01L"
};
std::vector<Style> g_styles;
std::vector<std::pair<std::string, std::string>> g_list;
rex::memory::Memory* g_memory = nullptr;

fs::path Game() { return rex::filesystem::GetExecutableFolder(); }

uint32_t GuestString(const std::string& s) {
  const uint32_t at = g_memory->SystemHeapAlloc(uint32_t(s.size() + 1));
  std::memcpy(g_memory->TranslateVirtual<char*>(at), s.c_str(), s.size() + 1);
  return at;
}

std::string ManifestValue(const fs::path& manifest, const char* key) {
  FILE* t = std::fopen(manifest.string().c_str(), "rb");
  if (!t) return {};
  std::string out;
  char line[512];
  const size_t n = std::strlen(key);
  while (std::fgets(line, sizeof line, t)) {
    std::string l = line;
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    if (l.size() > n && l.compare(0, n, key) == 0 && l[n] == '=') out = l.substr(n + 1);
  }
  std::fclose(t);
  return out;
}

// "M01L", "M60I", "M22F"
bool ThemeEntry(const std::string& n) {
  return n.size() == 4 && n[0] == 'M' && std::isdigit(uint8_t(n[1])) && std::isdigit(uint8_t(n[2])) &&
         (n[3] == 'L' || n[3] == 'I' || n[3] == 'F');
}

}  // namespace

namespace svr2011 {

void InstallVsStyles(rex::memory::Memory* memory) {
  g_memory = memory;
  if (NoMods()) return;  // (Start Without Mods: no_mods.h)
  std::error_code ec;
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(Game() / "Mods" / "VsStyles", ec))
    if (e.is_directory() && !fs::exists(e.path() / "disabled", ec) && fs::exists(e.path() / "vs.pac", ec) &&
        ManifestValue(e.path() / "manifest.txt", "type") == "VsStyle")
      folders.push_back(e.path());
  if (folders.empty()) return;
  std::sort(folders.begin(), folders.end());
  // The overlay: rebuilt when a style's vs.pac changed (its size and time, in vsstyles.pac.src)
  const fs::path out = Game() / "Mods" / "SuperstarOverlay" / "vsstyles.pac";
  std::string stamp;
  for (const auto& f : folders)
    stamp += f.filename().string() + "|" + std::to_string((unsigned long long)fs::file_size(f / "vs.pac", ec)) +
             "|" + std::to_string((long long)fs::last_write_time(f / "vs.pac", ec).time_since_epoch().count()) + "\n";
  std::string old;
  if (FILE* t = std::fopen((out.string() + ".src").c_str(), "rb")) {
    char b[4096];
    for (size_t n; (n = std::fread(b, 1, sizeof b, t)) > 0;) old.append(b, n);
    std::fclose(t);
  }
  const bool rebuild = old != stamp || !fs::exists(out, ec);
  svrfmt::Epac merged;
  for (const auto& f : folders) {
    svrfmt::Bytes d;
    svrfmt::Epac e;
    if (!svrfmt::ReadFile((f / "vs.pac").string(), d) || !svrfmt::EpacRead(d, e)) {
      REXLOG_WARN("[svr2011] VS styles: {} - vs.pac unreadable, left out", f.filename().string());
      continue;
    }
    char group[8];
    std::snprintf(group, sizeof group, "VS%02d", int(g_styles.size()));
    Style s;
    s.folder = f.filename().string();
    s.name = ManifestValue(f / "manifest.txt", "name");
    if (s.name.empty()) s.name = s.folder;
    svrfmt::EpacGroup g;
    g.type = group;
    for (auto& eg : e.groups) {
      if (eg.type != "MENU") continue;
      for (auto& en : eg.entries) {
        if (!ThemeEntry(en.name)) continue;
        s.alias["MENU/" + en.name] = GuestString(std::string("/") + group + "/" + en.name);
        if (rebuild) g.entries.push_back(std::move(en));
      }
    }
    if (s.alias.empty()) continue;
    if (merged.header.empty()) merged.header = e.header;
    if (rebuild) merged.groups.push_back(std::move(g));
    REXLOG_INFO("[svr2011] VS styles: {} ({}) - {} themes", s.folder, s.name, s.alias.size() / 3);
    g_list.push_back({s.folder, s.name});
    g_styles.push_back(std::move(s));
  }
  if (g_styles.empty()) return;
  if (rebuild) {
    svrfmt::Bytes o = svrfmt::EpacWrite(merged);
    uint32_t toc = 0;  // (header +4: the directory's size)
    for (const auto& g : merged.groups) toc += 12 + 12 * uint32_t(g.entries.size());
    o[4] = uint8_t(toc), o[5] = uint8_t(toc >> 8), o[6] = uint8_t(toc >> 16), o[7] = uint8_t(toc >> 24);
    fs::create_directories(out.parent_path(), ec);
    if (!svrfmt::WriteFile(out.string(), o)) {
      REXLOG_WARN("[svr2011] VS styles: can't write {}", out.string());
      g_styles.clear(), g_list.clear();
      return;
    }
    if (FILE* t = std::fopen((out.string() + ".src").c_str(), "wb")) {
      std::fwrite(stamp.data(), 1, stamp.size(), t);
      std::fclose(t);
    }
  }
  AddOverlayMount("vsstyles.pac");
}

const std::vector<std::pair<std::string, std::string>>& VsStyles() { return g_list; }

uint32_t VsStyleAlias(const char* name) {
  if (g_styles.empty() || !name) return 0;
  if (*name == '/') ++name;
  if (std::strncmp(name, "MENU/M", 6) != 0 || !ThemeEntry(std::string(name + 5, strnlen(name + 5, 8)))) return 0;
  const std::string chosen = REXCVAR_GET(vs_style);
  uint32_t alias = 0;
  for (const auto& s : g_styles)
    if (s.folder == chosen) {
      const auto it = s.alias.find(name);
      alias = it == s.alias.end() ? 0 : it->second;
    }
  // (a theme's layout asked for: the VS screen opens - tests wait for this line)
  static int logged = 0;
  if (name[8] == 'L' && logged++ < 200)
    REXLOG_INFO("[svr2011] VS styles: theme {} - {}", name + 5,
                alias ? chosen : chosen.empty() ? std::string("2011's") : chosen + " hasn't it, 2011's");
  return alias;
}

}  // namespace svr2011
