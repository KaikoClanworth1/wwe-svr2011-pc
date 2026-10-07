// Icons & renders page: the game's pictures by what they are for - the
// superstars' renders (DLC_HD.pac SSFA / SSFB / SSFC) and face icons
// (MENU/SSFD), the arena banners (MatchHD.pac MASI), VS screen themes
// (M<nn>I) and loading screens (LoadHD.pac LOAD), the crowd's signs
// (audience.pac AUDE/BORD). Any of them exports as PNG.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "svrfmt/pac.h"
#include "svrfmt/png.h"

namespace mm {
namespace icons_page {

using namespace svrfmt;

namespace {

int g_tab = 0;      // 0 superstars, 1 arenas, 2 signs
int g_test_tab = -1;
int g_star = -1;    // index in Stars()
int g_arena = 0;    // tile
char g_find[64] = "";

struct Shown {
  std::string name;
  Picture pic;
  int w = 0, h = 0;
  std::string format;
};

std::string FormatName(const Bytes& dds) {
  DdsInfo i;
  if (!DdsInfoOf(dds, i)) return "?";
  return std::to_string(i.w) + " x " + std::to_string(i.h) + " " +
         (i.format == DxtFormat::kDxt1 ? "DXT1" : i.format == DxtFormat::kDxt3 ? "DXT3" : i.format == DxtFormat::kDxt5 ? "DXT5" : "ARGB");
}

void ExportPng(const Picture& p, const std::string& stem) {
  const COMDLG_FILTERSPEC spec[] = {{L"PNG picture (*.png)", L"*.png"}};
  const std::wstring f = PickFile(true, L"Export the picture", spec, 1, L"png", (Wide(IdFrom(stem, "picture")) + L".png").c_str());
  if (!f.empty()) Status(SavePng(Utf8(f), p.image) ? "Exported " + Utf8(f) : "Could not write " + Utf8(f));
}

// A picture with its size under it and an export button.
void Card(Shown& s, float w) {
  ImGui::BeginGroup();
  s.pic.Draw(w, w * (s.h ? float(s.h) / std::max(1, s.w) : 1.0f));
  ImGui::TextDisabled("%s  %s", s.name.c_str(), s.format.c_str());
  ImGui::PushID(s.name.c_str());
  if (!s.pic.Empty() && ImGui::SmallButton("Export PNG...")) ExportPng(s.pic, s.name);
  ImGui::PopID();
  ImGui::EndGroup();
}

// ---- superstars

std::map<int, std::vector<Shown>> g_star_pics;  // id -> SSFA, SSFB, SSFC, icon
std::map<uint32_t, Bytes> g_icons;               // the face icon bank
bool g_icons_loaded = false;

void LoadStarPics(int id) {
  if (g_star_pics.count(id)) return;
  auto& v = g_star_pics[id];
  Bytes d;
  Epac e;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"DLC_HD.pac"), d) || !EpacRead(d, e)) return;
  char key[8];
  std::snprintf(key, sizeof key, "%04d", id);
  const char* groups[3] = {"SSFA", "SSFB", "SSFC"};
  const char* names[3] = {"Render (select, SSFA)", "Render (SSFB)", "Bust (SSFC)"};
  for (int g = 0; g < 3; ++g)
    for (const auto& grp : e.groups)
      if (grp.type == groups[g])
        for (const auto& en : grp.entries)
          if (en.name == key) {
            Shown s;
            s.name = names[g];
            const Bytes dds = Unpack(en.data);
            s.format = FormatName(dds);
            Image img;
            if (DdsDecode(dds, img)) s.w = img.w, s.h = img.h, s.pic.Set(std::move(img));
            v.push_back(std::move(s));
          }
  if (!g_icons_loaded) {  // MENU/SSFD: PACH {0: BPE(PACH {id: DDS 64 x 64})}
    g_icons_loaded = true;
    for (const auto& grp : e.groups)
      if (grp.type == "MENU")
        for (const auto& en : grp.entries)
          if (en.name == "SSFD") {
            std::vector<PachEntry> outer, inner;
            if (PachRead(en.data, outer) && !outer.empty() && PachRead(Unpack(outer[0].data), inner))
              for (auto& x : inner) g_icons[x.id] = std::move(x.data);
          }
  }
  if (const auto it = g_icons.find(uint32_t(id)); it != g_icons.end()) {
    Shown s;
    s.name = "Face icon (SSFD)";
    s.format = FormatName(it->second);
    Image img;
    if (DdsDecode(it->second, img)) s.w = img.w, s.h = img.h, s.pic.Set(std::move(img));
    v.push_back(std::move(s));
  }
}

void StarsTab() {
  const auto& stars = Stars();
  ImGui::BeginChild("starlist", ImVec2(260 * g_scale, 0), true);
  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##f", "find a superstar", g_find, sizeof g_find);
  const std::string f = Lower(g_find);
  for (int i = 0; i < int(stars.size()); ++i) {
    if (!f.empty() && Lower(stars[i].name).find(f) == std::string::npos) continue;
    if (ImGui::Selectable((stars[i].name + "##" + std::to_string(i)).c_str(), g_star == i)) g_star = i;
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("starpics", ImVec2(0, 0), false);
  if (g_star < 0) {
    ImGui::TextDisabled("Pick a superstar: the select render, the other render, the bust and the face icon.");
  } else {
    const int id = stars[g_star].id;
    LoadStarPics(id);
    ImGui::Text("%s (id %d)", stars[g_star].name.c_str(), id);
    ImGui::TextDisabled("A superstar mod or a media pack can replace these (Superstar page: select picture; Media page: Menus & renders).");
    auto& v = g_star_pics[id];
    if (v.empty()) ImGui::TextDisabled("No renders in DLC_HD.pac for this id.");
    for (size_t k = 0; k < v.size(); ++k) {
      if (k) ImGui::SameLine();
      Card(v[k], (k < 2 ? 256 : k == 2 ? 192 : 96) * g_scale);
    }
  }
  ImGui::EndChild();
}

// ---- arenas

std::map<int, std::vector<Shown>> g_vs;  // tile -> VS theme textures
std::vector<Shown> g_loads;
bool g_loads_loaded = false;

void LoadVs(int tile) {
  if (g_vs.count(tile)) return;
  auto& v = g_vs[tile];
  Bytes d;
  Epac e;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"menu" / L"MatchHD.pac"), d) || !EpacRead(d, e)) return;
  char group[8];
  std::snprintf(group, sizeof group, "M%02dI", g_arenas[tile].number);
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name != group) continue;
      std::vector<PachEntry> ents;
      if (!PachRead(en.data, ents)) continue;
      for (const auto& pe : ents) {
        std::vector<BundleTexture> texs;
        if (!BundleRead(Unpack(pe.data), texs)) continue;
        for (const auto& t : texs) {
          Shown s;
          s.name = t.name;
          s.format = FormatName(t.data);
          Image img;
          if (DdsDecode(t.data, img)) s.w = img.w, s.h = img.h, s.pic.Set(std::move(img));
          v.push_back(std::move(s));
        }
      }
    }
}

void LoadLoads() {
  if (g_loads_loaded) return;
  g_loads_loaded = true;
  Bytes d;
  Epac e;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"menu" / L"LoadHD.pac"), d) || !EpacRead(d, e)) return;
  for (const auto& g : e.groups) {
    if (g.type != "LOAD") continue;
    for (const auto& en : g.entries) {
      const Bytes raw = Unpack(en.data);
      std::vector<BundleTexture> texs;
      std::vector<PachEntry> ents;
      if (IsTextureBundle(raw)) BundleRead(raw, texs);
      else if (PachRead(raw, ents))
        for (const auto& pe : ents) {
          std::vector<BundleTexture> t2;
          if (BundleRead(Unpack(pe.data), t2)) texs.insert(texs.end(), t2.begin(), t2.end());
        }
      for (const auto& t : texs) {
        Shown s;
        s.name = en.name + "/" + t.name;
        s.format = FormatName(t.data);
        Image img;
        if (DdsDecode(t.data, img)) s.w = img.w, s.h = img.h, s.pic.Set(std::move(img));
        g_loads.push_back(std::move(s));
      }
    }
  }
}

void ArenasTab() {
  ImGui::TextUnformatted("Select banners");
  ImGui::TextDisabled("MatchHD.pac MASI, 256 x 128 DXT5. An arena mod brings its own (Arena page, Details).");
  const float tw = 128 * g_scale;
  const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (tw + 12 * g_scale)));
  for (int i = 0; i < 20; ++i) {
    if (i % cols) ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushID(i);
    if (g_arena == i) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    if (!g_arenas[i].tile.Empty() && ImGui::ImageButton("b", g_arenas[i].tile.Id(), ImVec2(tw, tw / 2))) g_arena = i;
    if (g_arena == i) ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::TextDisabled("%s", g_arenas[i].name);
    ImGui::EndGroup();
  }
  if (!g_arenas[g_arena].tile.Empty() && ImGui::SmallButton("Export the banner as PNG...")) ExportPng(g_arenas[g_arena].tile, std::string(g_arenas[g_arena].name) + " banner");
  ImGui::Separator();
  ImGui::Text("VS screen theme: %s (MatchHD.pac M%02dI)", g_arenas[g_arena].name, g_arenas[g_arena].number);
  LoadVs(g_arena);
  auto& v = g_vs[g_arena];
  const int vcols = std::max(1, int(ImGui::GetContentRegionAvail().x / (200 * g_scale + 12 * g_scale)));
  int k = 0;
  for (auto& s : v) {
    if (s.w < 32) continue;
    if (k++ % vcols) ImGui::SameLine();
    Card(s, 200 * g_scale);
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Loading screens (LoadHD.pac LOAD)");
  ImGui::TextDisabled("An arena mod's load.dds replaces the one shown while its match loads (1024 x 512).");
  LoadLoads();
  k = 0;
  for (auto& s : g_loads) {
    if (k++ % 3) ImGui::SameLine();
    Card(s, 300 * g_scale);
  }
}

// ---- crowd signs

std::vector<std::pair<uint32_t, Shown>> g_signs;
bool g_signs_loaded = false;

void LoadSigns() {
  if (g_signs_loaded) return;
  g_signs_loaded = true;
  Bytes d;
  Epac e;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"audience.pac"), d) || !EpacRead(d, e)) return;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (g.type != "AUDE" || en.name != "BORD") continue;
      std::vector<PachEntry> outer, inner;
      if (!PachRead(en.data, outer) || outer.empty() || !PachRead(Unpack(outer[0].data), inner)) continue;
      for (const auto& x : inner) {
        std::vector<BundleTexture> texs;
        if (!BundleRead(Unpack(x.data), texs) || texs.empty()) continue;
        Shown s;
        s.name = "sign " + std::to_string(x.id);
        s.format = FormatName(texs[0].data);
        Image img;
        if (DdsDecode(texs[0].data, img)) s.w = img.w, s.h = img.h, s.pic.Set(std::move(img));
        g_signs.push_back({x.id, std::move(s)});
      }
    }
}

void SignsTab() {
  LoadSigns();
  ImGui::Text("%zu signs in audience.pac AUDE/BORD (128 x 64 DXT1). Ids: a superstar's id x 10 + 1..4 are the signs "
              "that superstar's fans hold; 4001+ the Created Superstar picker's; 5001+ the general ones.", g_signs.size());
  ImGui::TextDisabled("Sign packs and superstar mods add to these (Crowd signs page; Superstar page: crowd signs).");
  ImGui::SetNextItemWidth(200 * g_scale);
  ImGui::InputTextWithHint("##sf", "find by id", g_find, sizeof g_find);
  const std::string f = g_find;
  const float tw = 128 * g_scale;
  const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (tw + 12 * g_scale)));
  ImGui::BeginChild("signs", ImVec2(0, 0), false);
  int k = 0;
  for (auto& [id, s] : g_signs) {
    if (!f.empty() && std::to_string(id).find(f) == std::string::npos) continue;
    if (k++ % cols) ImGui::SameLine();
    ImGui::BeginGroup();
    s.pic.Draw(tw, tw / 2);
    ImGui::TextDisabled("%u", id);
    if (ImGui::IsItemHovered()) {
      const StarInfo* st = id < 4000 ? StarById(int(id / 10)) : nullptr;
      const std::string who = st ? st->name + "'s fans" : id >= 5000 ? "general" : id >= 4000 ? "Created Superstar picker" : "superstar " + std::to_string(id / 10);
      ImGui::SetTooltip("%s", who.c_str());
    }
    ImGui::EndGroup();
  }
  ImGui::EndChild();
}

}  // namespace

void TestTab(int tab) { g_test_tab = tab; }

void Draw() {
  Heading("Icons & renders", "The game's pictures by what they are for: superstar renders and face icons, arena "
                             "banners, VS screens and loading screens, the crowd's signs. Export any as PNG.");
  if (g_game.empty()) {
    ImGui::TextDisabled("No game folder.");
    return;
  }
  if (ImGui::BeginTabBar("icon_tabs")) {
    const char* names[3] = {"Superstars", "Arenas", "Crowd signs"};
    for (int t = 0; t < 3; ++t) {
      const ImGuiTabItemFlags fl = g_test_tab == t ? ImGuiTabItemFlags_SetSelected : 0;
      if (ImGui::BeginTabItem(names[t], nullptr, fl)) {
        ImGui::BeginChild("tabbody", ImVec2(0, 0), false);
        if (t == 0) StarsTab();
        else if (t == 1) ArenasTab();
        else SignsTab();
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
    }
    g_test_tab = -1;
    ImGui::EndTabBar();
  }
}

}  // namespace icons_page
}  // namespace mm
