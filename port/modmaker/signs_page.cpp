// Crowd signs page (the game: crowd_signs.cpp, docs/CROWD_SIGNS.md): pictures
// the crowd holds up - 128 x 64 DXT1 with mipmaps, as the game's own
// (audience.pac AUDE/BORD). A sign pack (.svrmod: manifest.txt type=signs,
// NN_<name>.dds) installs into <game>/Mods/Signs/<id>; its signs join the
// general signs every match draws from.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app.h"

namespace mm {
namespace signs_page {

using namespace svrfmt;

namespace {

struct Sign {
  std::string name;
  Picture picture;  // 128 x 64
};
struct SignPack {
  char name[64] = "", author[64] = "", version[16] = "1.0";
  std::vector<Sign> signs;
};
SignPack g_pack;
std::vector<std::wstring> g_test_files;  // --sign <picture>..., --test-sign-save <file>
std::wstring g_test_save;

void AddSigns(const std::vector<std::wstring>& files) {
  for (const auto& f : files) {
    Image img;
    if (!LoadPicture(f, img)) continue;
    Sign sg;
    sg.name = Utf8(fs::path(f).stem().wstring());
    sg.picture.Set(SignPicture(img));
    g_pack.signs.push_back(std::move(sg));
    Touch();
  }
}

std::string PackId() { return IdFrom(g_pack.name, "signs"); }

bool BuildPack(std::vector<ZipEntry>& files) {
  if (g_pack.signs.empty()) {
    Status("Add some pictures first.");
    return false;
  }
  if (!g_pack.name[0]) std::snprintf(g_pack.name, sizeof g_pack.name, "My Signs");
  const std::string man = "type=signs\nid=" + PackId() + "\nname=" + g_pack.name + "\nauthor=" + g_pack.author +
                          "\nversion=" + g_pack.version + "\n";
  files.push_back({"manifest.txt", Bytes(man.begin(), man.end())});
  for (size_t i = 0; i < g_pack.signs.size(); ++i) {
    char n[16];
    std::snprintf(n, sizeof n, "%02zu_", i + 1);
    std::string stem;
    for (char c : g_pack.signs[i].name) stem += std::isalnum(uint8_t(c)) ? c : '_';
    files.push_back({n + stem.substr(0, 40) + ".dds", DdsEncode(g_pack.signs[i].picture.image, DxtFormat::kDxt1, true)});
  }
  return true;
}

void SavePack() {
  std::vector<ZipEntry> files;
  if (!BuildPack(files)) return;
  const std::wstring f = PickFile(true, L"Save the sign pack", kModFilter, 1, L"svrmod", (Wide(PackId()) + L".svrmod").c_str());
  if (f.empty()) return;
  if (WriteFile(Utf8(f), ZipWrite(files))) Status("Saved " + Utf8(f) + " (add it in the launcher's Mods tab with +).");
  else Status("The sign pack could not be written.");
}

void InstallPack() {
  std::vector<ZipEntry> files;
  if (!BuildPack(files)) return;
  const fs::path dir = fs::path(g_game) / L"Mods" / L"Signs" / fs::u8path(PackId());
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  for (const auto& f : files)
    if (!WriteFile(PathStr(dir / fs::u8path(f.name)), f.data)) {
      Status("Could not write into " + PathStr(dir) + " (is the game running?)");
      return;
    }
  Status("Installed: the crowd holds these signs in every match from the next game start (" + PathStr(dir) + ").");
}

void Problems(std::vector<Problem>& p) {
  if (g_pack.signs.empty()) p.push_back(Error("No signs yet.", "Add pictures: any picture is fitted onto a white 128 x 64 board."));
  if (!g_pack.name[0]) p.push_back(Warning("No pack name: \"My Signs\" is used."));
  if (g_pack.signs.size() > 64) p.push_back(Warning("A lot of signs: each match shows at most 16, drawn from the game's and every pack's."));
}

void Draw() {
  if (!g_test_files.empty()) {
    AddSigns(g_test_files);
    g_test_files.clear();
    if (!g_test_save.empty()) {
      std::vector<ZipEntry> files;
      if (BuildPack(files) && WriteFile(Utf8(g_test_save), ZipWrite(files))) Log("test: saved " + Utf8(g_test_save));
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
  }
  Heading("Crowd signs", "New signs for the crowd to hold up in every match, with the game's own. Any picture: it is "
                         "fitted onto a white 128 x 64 board (a wide picture fills it best).");
  std::vector<Problem> problems;
  Problems(problems);
  ImGui::BeginChild("signs_body", ImVec2(0, -48 * g_scale), false);
  TextField("Pack name", g_pack.name, sizeof g_pack.name, "My Signs");
  TextField("Author (optional)", g_pack.author, sizeof g_pack.author);
  TextField("Version (optional)", g_pack.version, sizeof g_pack.version, "1.0");
  if (ImGui::Button("Add pictures...", ImVec2(220 * g_scale, 0))) AddSigns(PickFiles(L"Sign pictures", kPictureFilter, 1));
  ImGui::SameLine();
  ImGui::TextDisabled("%zu sign%s", g_pack.signs.size(), g_pack.signs.size() == 1 ? "" : "s");
  ImGui::Separator();
  const float tw = 128 * 1.5f * g_scale, th = 64 * 1.5f * g_scale;
  const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (tw + 16 * g_scale)));
  for (size_t i = 0; i < g_pack.signs.size(); ++i) {
    if (i % cols) ImGui::SameLine();
    ImGui::PushID(int(i));
    ImGui::BeginGroup();
    ImGui::Image(g_pack.signs[i].picture.Id(), ImVec2(tw, th));
    ImGui::TextDisabled("%s", g_pack.signs[i].name.substr(0, 18).c_str());
    ImGui::SameLine(tw - 60 * g_scale);
    if (ImGui::SmallButton("remove")) {
      g_pack.signs.erase(g_pack.signs.begin() + long(i));
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
  std::string s = std::string("name=") + g_pack.name + "\nauthor=" + g_pack.author + "\nversion=" + g_pack.version + "\n";
  for (const auto& sg : g_pack.signs) s += "sign=" + sg.name + "\n";
  return s;
}

void Reset() { g_pack = SignPack(); }

void WriteProject(ProjectOut& out) {
  out.Key("name", g_pack.name);
  out.Key("author", g_pack.author);
  out.Key("version", g_pack.version);
  for (size_t i = 0; i < g_pack.signs.size(); ++i) {
    out.Key("sign_name" + std::to_string(i), g_pack.signs[i].name);
    out.Png("sign" + std::to_string(i), g_pack.signs[i].picture.image, "signs/" + std::to_string(i));
  }
}

bool Read(const ProjectIn& in, bool mod) {
  std::snprintf(g_pack.name, sizeof g_pack.name, "%s", in.Get("name").c_str());
  std::snprintf(g_pack.author, sizeof g_pack.author, "%s", in.Get("author").c_str());
  std::snprintf(g_pack.version, sizeof g_pack.version, "%s", in.Get("version").c_str());
  if (!g_pack.version[0]) std::snprintf(g_pack.version, sizeof g_pack.version, "1.0");
  if (mod) {
    for (const auto& e : in.files) {
      if (e.name == "manifest.txt" || !Lower(e.name).ends_with(".dds")) continue;
      Image img;
      if (!DdsDecode(e.data, img)) continue;
      Sign sg;
      sg.name = e.name.substr(e.name.size() > 3 && std::isdigit(uint8_t(e.name[0])) && e.name[2] == '_' ? 3 : 0);
      sg.name = sg.name.substr(0, sg.name.size() - 4);
      sg.picture.Set(img.w == 128 && img.h == 64 ? img : Resize(img, 128, 64));
      g_pack.signs.push_back(std::move(sg));
    }
  } else {
    for (int i = 0; i < 999; ++i) {
      Image img;
      if (!in.Png(("sign" + std::to_string(i)).c_str(), img)) break;
      Sign sg;
      sg.name = in.Get(("sign_name" + std::to_string(i)).c_str());
      sg.picture.Set(img);
      g_pack.signs.push_back(std::move(sg));
    }
  }
  return true;
}

}  // namespace

// Any picture -> a sign: fitted onto a white 128 x 64 board (no stretching).
Image SignPicture(const Image& src) {
  Image out;
  out.w = 128, out.h = 64;
  out.rgba.assign(size_t(128) * 64 * 4, 255);
  const float k = std::min(128.0f / src.w, 64.0f / src.h);
  const int w = std::max(1, int(src.w * k)), h = std::max(1, int(src.h * k));
  const Image fit = Resize(src, w, h);
  const int x0 = (128 - w) / 2, y0 = (64 - h) / 2;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint8_t* q = &fit.rgba[(size_t(y) * w + x) * 4];
      uint8_t* o = &out.rgba[(size_t(y + y0) * 128 + x + x0) * 4];
      const int a = q[3];
      for (int c = 0; c < 3; ++c) o[c] = uint8_t((q[c] * a + 255 * (255 - a)) / 255);  // (over white)
      o[3] = 255;
    }
  return out;
}

void TestStart(const std::vector<std::wstring>& files, const std::wstring& save) {
  g_test_files = files;
  g_test_save = save;
}

PageHooks hooks = {
    "signs", Draw, Problems, Reset, StateText, WriteProject,
    [](const ProjectIn& in) { return Read(in, false); },
    [](const ProjectIn& in) { return Read(in, true); },
};

}  // namespace signs_page
}  // namespace mm
