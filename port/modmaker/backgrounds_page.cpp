// Main menu backgrounds page (the game: backgrounds_page.cpp,
// docs/MENU_BACKGROUNDS.md): pictures behind the main menu, after the game's
// own 12. A background pack (.svrmod: manifest.txt type=Background, logo=,
// title.<file>= for each picture; NN_<name>.dds, 1024 x 512 DXT5 - one
// picture alone is picture.dds) installs into <game>/Mods/Backgrounds/<id>.
// Pictures are fitted to where the menu puts its picture (the launcher's
// bg_map.h: covered, centred, not squashed).
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "mod_check.h"
#include "../launcher/bg_map.h"

namespace mm {
namespace backgrounds_page {

using namespace svrfmt;

namespace {

struct Background {
  char name[64] = "";
  Image texture;    // 1024 x 512, as the game gets it
  Picture preview;  // how the main menu shows it (1280 x 665, half size)
};
struct Pack {
  char name[64] = "", author[64] = "", version[16] = "1.0";
  bool logo = false;  // the game's "SmackDown vs RAW 2011" logo over them
  std::vector<Background> pictures;
};
Pack g_pack;
std::vector<std::wstring> g_test_files;  // --background <picture>..., --test-background-save <file>
std::wstring g_test_save;

// Any picture -> the 1024 x 512 texture (bg_map.h).
Image TextureFrom(const Image& src) {
  unsigned sw = 0, sh = 0;
  bg_cover_size(unsigned(src.w), unsigned(src.h), &sw, &sh);
  const Image scaled = Resize(src, int(sw), int(sh));
  Image out;
  out.w = BG_W, out.h = BG_H;
  out.rgba.assign(size_t(BG_W) * BG_H * 4, 255);
  bg_map(scaled.rgba.data(), sw, sh, out.rgba.data());
  for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;  // (opaque, as the game's own)
  return out;
}

// The texture as the main menu shows it (the visible 1280 x 665, half size).
Image PreviewOf(const Image& t) {
  Image out;
  out.w = 640, out.h = int(BG_VIEW_H / 2);
  out.rgba.assign(size_t(out.w) * out.h * 4, 255);
  const double split = BG_X0 + BG_XS1 * BG_SEAM;
  for (int y = 0; y < out.h; ++y) {
    const int ty = std::clamp(int((y * 2 + 1 - BG_Y0) / BG_YS), 0, BG_H - 1);
    for (int x = 0; x < out.w; ++x) {
      const double sx = x * 2 + 1;
      const int tx = std::clamp(int(sx < split ? (sx - BG_X0) / BG_XS1 : BG_SEAM + (sx - split) / BG_XS2), 0, BG_W - 1);
      std::memcpy(&out.rgba[(size_t(y) * out.w + x) * 4], &t.rgba[(size_t(ty) * BG_W + tx) * 4], 4);
    }
  }
  return out;
}

void Add(std::string name, Image texture) {
  Background b;
  std::snprintf(b.name, sizeof b.name, "%s", name.c_str());
  b.preview.Set(PreviewOf(texture));
  b.texture = std::move(texture);
  g_pack.pictures.push_back(std::move(b));
  Touch();
}

void AddPictures(const std::vector<std::wstring>& files) {
  for (const auto& f : files) {
    Image img;
    if (!LoadPicture(f, img)) continue;
    Add(Utf8(fs::path(f).stem().wstring()), TextureFrom(img));
  }
}

std::string PackId() { return IdFrom(g_pack.name, "backgrounds"); }

// The pictures' file names in the pack.
std::string FileOf(size_t i) {
  if (g_pack.pictures.size() == 1) return "picture.dds";
  char n[8];
  std::snprintf(n, sizeof n, "%02zu_", i + 1);
  return n + IdFrom(g_pack.pictures[i].name, "picture").substr(0, 40) + ".dds";
}

bool BuildPack(std::vector<ZipEntry>& files) {
  if (g_pack.pictures.empty()) {
    Status("Add some pictures first.");
    return false;
  }
  if (!g_pack.name[0])
    std::snprintf(g_pack.name, sizeof g_pack.name, "%s",
                  g_pack.pictures.size() == 1 && g_pack.pictures[0].name[0] ? g_pack.pictures[0].name : "My Backgrounds");
  std::string man = "type=Background\nid=" + PackId() + "\nname=" + g_pack.name + "\nauthor=" + g_pack.author +
                    "\nversion=" + g_pack.version + "\nlogo=" + (g_pack.logo ? "1" : "0") + "\n" + MadeWith();
  if (g_pack.pictures.size() > 1)
    for (size_t i = 0; i < g_pack.pictures.size(); ++i)
      if (g_pack.pictures[i].name[0]) man += "title." + FileOf(i) + "=" + g_pack.pictures[i].name + "\n";
  files.push_back({"manifest.txt", Bytes(man.begin(), man.end())});
  for (size_t i = 0; i < g_pack.pictures.size(); ++i)
    files.push_back({FileOf(i), DdsEncode(g_pack.pictures[i].texture, DxtFormat::kDxt5, false)});
  return true;
}

void SavePack() {
  std::vector<ZipEntry> files;
  if (!BuildPack(files)) return;
  const std::wstring f =
      PickFile(true, L"Save the background pack", kModFilter, 1, L"svrmod", (Wide(PackId()) + L".svrmod").c_str());
  if (f.empty()) return;
  if (WriteFile(Utf8(f), ZipWrite(files))) Status("Saved " + Utf8(f) + " (add it in the launcher's Mods tab with +).");
  else Status("The background pack could not be written.");
}

void InstallPack() {
  std::vector<ZipEntry> files;
  if (!BuildPack(files)) return;
  const fs::path dir = fs::path(g_game) / L"Mods" / L"Backgrounds" / fs::u8path(PackId());
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  for (const auto& f : files)
    if (!WriteFile(PathStr(dir / fs::u8path(f.name)), f.data)) {
      Status("Could not write into " + PathStr(dir) + " (is the game running?)");
      return;
    }
  Status("Installed: the main menu shows these after its own 12 pictures from the next game start (" + PathStr(dir) + ")." + InstallReport("Backgrounds", dir));
}

void Problems(std::vector<Problem>& p) {
  if (g_pack.pictures.empty())
    p.push_back(Error("No pictures yet: add pictures - any size, each is fitted to the main menu."));
  if (!g_pack.name[0]) p.push_back(Warning("The backgrounds have no name yet: they'd take the first picture's name."));
}

void Draw() {
  if (!g_test_files.empty()) {
    AddPictures(g_test_files);
    g_test_files.clear();
    if (!g_test_save.empty()) {
      std::vector<ZipEntry> files;
      if (BuildPack(files) && WriteFile(Utf8(g_test_save), ZipWrite(files))) Log("test: saved " + Utf8(g_test_save));
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
  }
  Heading("Main menu backgrounds",
          "Your pictures behind the main menu, after the game's 12. Any size: fitted without squashing (previews below).");
  std::vector<Problem> problems;
  Problems(problems);
  ImGui::BeginChild("bg_body", ImVec2(0, -48 * g_scale), false);
  TextField("Pack name", g_pack.name, sizeof g_pack.name, "My Backgrounds");
  TextField("Author (optional)", g_pack.author, sizeof g_pack.author);
  TextField("Version (optional)", g_pack.version, sizeof g_pack.version, "1.0");
  if (ImGui::Checkbox("Show the game's logo over them", &g_pack.logo)) Touch();
  Hint("The \"SmackDown vs RAW 2011\" logo the game puts at the top left. Off: fan art with its own logo shows "
       "as it is. Players can change it per picture in the game (X on the BACKGROUNDS page).");
  if (ImGui::Button("Add pictures...", ImVec2(220 * g_scale, 0))) AddPictures(PickFiles(L"Background pictures", kPictureFilter, 1));
  ImGui::SameLine();
  ImGui::TextDisabled("%zu picture%s", g_pack.pictures.size(), g_pack.pictures.size() == 1 ? "" : "s");
  ImGui::Separator();
  const float tw = 320 * g_scale, th = tw * float(BG_VIEW_H / BG_VIEW_W);
  const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (tw + 16 * g_scale)));
  for (size_t i = 0; i < g_pack.pictures.size(); ++i) {
    if (i % cols) ImGui::SameLine();
    ImGui::PushID(int(i));
    ImGui::BeginGroup();
    ImGui::Image(g_pack.pictures[i].preview.Id(), ImVec2(tw, th));
    ImGui::SetNextItemWidth(tw - 70 * g_scale);
    if (ImGui::InputTextWithHint("##name", "Name in the game", g_pack.pictures[i].name, sizeof g_pack.pictures[i].name))
      Touch();
    ImGui::SameLine();
    if (ImGui::SmallButton("remove")) {
      g_pack.pictures.erase(g_pack.pictures.begin() + long(i));
      Touch();
      ImGui::EndGroup();
      ImGui::PopID();
      break;
    }
    ImGui::EndGroup();
    ImGui::PopID();
  }
  ImGui::Spacing();
  DrawProblems(problems);
  ImGui::EndChild();
  switch (ModButtons(problems, false)) {
    case 1: SavePack(); break;
    case 2: InstallPack(); break;
  }
}

std::string StateText() {
  std::string s = std::string("name=") + g_pack.name + "\nauthor=" + g_pack.author + "\nversion=" + g_pack.version +
                  "\nlogo=" + (g_pack.logo ? "1" : "0") + "\n";
  for (const auto& b : g_pack.pictures) s += std::string("picture=") + b.name + "\n";
  return s;
}

void Reset() { g_pack = Pack(); }

void WriteProject(ProjectOut& out) {
  out.Key("name", g_pack.name);
  out.Key("author", g_pack.author);
  out.Key("version", g_pack.version);
  out.Key("logo", g_pack.logo ? "1" : "0");
  for (size_t i = 0; i < g_pack.pictures.size(); ++i) {
    out.Key("picture_name" + std::to_string(i), g_pack.pictures[i].name);
    out.Png("picture" + std::to_string(i), g_pack.pictures[i].texture, "backgrounds/" + std::to_string(i));
  }
}

bool Read(const ProjectIn& in, bool mod) {
  std::snprintf(g_pack.name, sizeof g_pack.name, "%s", in.Get("name").c_str());
  std::snprintf(g_pack.author, sizeof g_pack.author, "%s", in.Get("author").c_str());
  std::snprintf(g_pack.version, sizeof g_pack.version, "%s", in.Get("version").c_str());
  if (!g_pack.version[0]) std::snprintf(g_pack.version, sizeof g_pack.version, "1.0");
  g_pack.logo = in.Get("logo") == "1";
  if (mod) {
    // (the pictures are already fitted: read back as they are)
    std::vector<const ZipEntry*> dds;
    for (const auto& e : in.files)
      if (e.name != "manifest.txt" && Lower(e.name).ends_with(".dds")) dds.push_back(&e);
    std::sort(dds.begin(), dds.end(), [](auto* a, auto* b) { return a->name < b->name; });
    for (const ZipEntry* e : dds) {
      Image img;
      if (!DdsDecode(e->data, img)) continue;
      std::string name = in.Get(("title." + e->name).c_str());
      if (name.empty()) {
        name = e->name == "picture.dds" ? std::string(g_pack.name) : e->name.substr(0, e->name.size() - 4);
        if (name.size() > 3 && std::isdigit(uint8_t(name[0])) && std::isdigit(uint8_t(name[1])) && name[2] == '_')
          name = name.substr(3);
        std::replace(name.begin(), name.end(), '_', ' ');
      }
      Add(name, img.w == BG_W && img.h == BG_H ? img : Resize(img, BG_W, BG_H));
    }
  } else {
    for (int i = 0; i < 999; ++i) {
      Image img;
      if (!in.Png(("picture" + std::to_string(i)).c_str(), img)) break;
      Add(in.Get(("picture_name" + std::to_string(i)).c_str()), std::move(img));
    }
  }
  return true;
}

}  // namespace

void TestStart(const std::vector<std::wstring>& files, const std::wstring& save) {
  g_test_files = files;
  g_test_save = save;
}

PageHooks hooks = {
    "Background", Draw, Problems, Reset, StateText, WriteProject,
    [](const ProjectIn& in) { return Read(in, false); },
    [](const ProjectIn& in) { return Read(in, true); },
};

}  // namespace backgrounds_page
}  // namespace mm
