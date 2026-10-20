// CAW pictures page: the picture the game shows for a Created Superstar in
// its lists (made at FINALIZE), replaced from any picture. The game keeps it
// in the CAW's save, <game>/Saves/NNCreateSuperStar.cas: four attire slots of
// 149,528 bytes at 746,340 + attire * 149,528, each holding the 256 x 512
// render (DXT5, 16-bit words swapped, no header) at +0, the 128 x 128 thumb
// (the Community Creations preview) at +0x20414, and two option bytes at
// +0x24814. Replacing them means redoing the save's checksums: a byte sum,
// big-endian at .cas +12, +0x148FC8 and +0x148DBC, copied into SaveData.dat's
// record for the slot (0x13FB8 + slot * 0x6BC, +0x65C) whose own checksum
// (+0x18, the sum of 0x1C..0x812DC) follows. Research: svr2011-mod-maker-2.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "app.h"
#include "svrfmt/png.h"
extern "C" {
#include "../launcher/caw_import.h"
}

namespace mm {
namespace caw_page {

using namespace svrfmt;

namespace {

constexpr size_t kCasSize = 1347532, kSlotAt = 746340, kSlotSize = 149528, kRenderBytes = 131072,
                 kRenderField = 132114, kThumbAt = 0x20414, kThumbBytes = 16384, kThumbField = 17408,
                 kRecordAt = 0x1483C4, kSum1 = 12, kSum2 = 0x148FC8, kSum3 = 0x148DBC;
constexpr size_t kSaveRecords = 0x13FB8, kSaveRecord = 0x6BC, kSaveSumAt = 0x18, kSaveSumFrom = 0x1C, kSaveSumTo = 0x812DC;

struct Attire {
  bool present = false;
  Picture render, thumb;    // as decoded (RGBA)
  Image new_render;         // a replacement waiting to be saved (256 x 512 RGBA), empty = none
  Image new_thumb;
  bool thumb_from_render = true;
};
struct Caw {
  fs::path file;
  std::string label;  // "01. <name>" (the save's slot number and the record's name)
  std::string name;   // the record's name (+0x22, 8-bit as the game has it): caw_announcer.txt's key
  int slot = -1;      // the save's slot (SaveData.dat record)
  Attire attires[4];
  bool loaded = false;
  std::string error;
};
std::vector<Caw> g_caws;
std::wstring g_loaded_for;
int g_sel = -1;
int g_test_index = -1, g_test_attire = 0;  // --caw <index>[,<attire>,<picture>[,save]]
std::wstring g_test_picture;
bool g_test_save = false;
std::string g_test_announcer;  // --caw-announcer <CLIP>: set for the --caw <index> CAW

// <game>\Mods\caw_announcer.txt: "<CAW full name>=<CLIP>" lines, kept in order (also names no save has now)
std::vector<std::pair<std::string, std::string>> g_calls;
fs::path CallsFile() { return fs::path(g_game) / L"Mods" / L"caw_announcer.txt"; }
void LoadCalls() {
  g_calls.clear();
  Bytes b;
  if (!ReadFile(PathStr(CallsFile()), b)) return;
  std::string t(b.begin(), b.end()), line;
  for (size_t at = 0; at <= t.size();) {
    size_t e = t.find('\n', at);
    if (e == std::string::npos) e = t.size();
    line = t.substr(at, e - at);
    at = e + 1;
    while (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.rfind('=');  // (as the game splits it)
    if (eq != std::string::npos && eq > 0) g_calls.push_back({line.substr(0, eq), line.substr(eq + 1)});
  }
}
std::string CallFor(const std::string& name) {
  for (const auto& [n, clip] : g_calls)
    if (n == name) return clip;
  return {};
}
bool SetCall(const std::string& name, const std::string& clip) {
  bool found = false;
  for (auto it = g_calls.begin(); it != g_calls.end();)
    if (it->first == name) {
      if (clip.empty() || found) it = g_calls.erase(it);
      else it->second = clip, found = true, ++it;
    } else ++it;
  if (!found && !clip.empty()) g_calls.push_back({name, clip});
  std::string t;
  for (const auto& [n, c] : g_calls) t += n + "=" + c + "\r\n";
  std::error_code ec;
  fs::create_directories(CallsFile().parent_path(), ec);
  return WriteFile(PathStr(CallsFile()), Bytes(t.begin(), t.end()));
}
// The record's 8-bit name as UTF-8 for showing (the game's text is Latin-1-like).
std::string Shown(const std::string& raw) {
  std::string out;
  for (unsigned char ch : raw)
    if (ch < 0x80) out += char(ch);
    else out += char(0xC0 | ch >> 6), out += char(0x80 | (ch & 0x3F));
  return out;
}

// ---- the picture format: raw DXT5 blocks, every 16-bit word swapped

Bytes DdsHeader(int w, int h) {
  Bytes hd(128, 0);
  std::memcpy(&hd[0], "DDS ", 4);
  PutLe32(&hd[4], 124);
  PutLe32(&hd[8], 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000);  // caps, height, width, pixelformat, linear size
  PutLe32(&hd[12], uint32_t(h));
  PutLe32(&hd[16], uint32_t(w));
  PutLe32(&hd[20], uint32_t(std::max(1, (w + 3) / 4) * std::max(1, (h + 3) / 4) * 16));
  PutLe32(&hd[28], 1);
  PutLe32(&hd[76], 32);
  PutLe32(&hd[80], 0x4);  // fourcc
  std::memcpy(&hd[84], "DXT5", 4);
  PutLe32(&hd[108], 0x1000);  // texture
  return hd;
}

Bytes Swapped(const uint8_t* p, size_t n) {
  Bytes out(p, p + n);
  for (size_t i = 0; i + 1 < n; i += 2) std::swap(out[i], out[i + 1]);
  return out;
}

bool DecodeRaw(const uint8_t* p, int w, int h, Image& out) {
  Bytes dds = DdsHeader(w, h);
  const Bytes blocks = Swapped(p, size_t(w / 4) * size_t(h / 4) * 16);
  dds.insert(dds.end(), blocks.begin(), blocks.end());
  return DdsDecode(dds, out);
}

Bytes EncodeRaw(const Image& img) {  // (img already w x h)
  const Bytes dds = DdsEncode(img, DxtFormat::kDxt5, false);
  return dds.size() > 128 ? Swapped(dds.data() + 128, dds.size() - 128) : Bytes();
}

uint32_t Be(const Bytes& b, size_t at) { return Be32(&b[at]); }
void PutBe(Bytes& b, size_t at, uint32_t v) { PutBe32(&b[at], v); }
uint64_t Sum(const Bytes& b, size_t from, size_t n) {
  uint64_t s = 0;
  for (size_t i = from; i < from + n && i < b.size(); ++i) s += b[i];
  return s;
}

// The name in the record: the first run of UTF-16 letters that looks like a name is not
// pinned down; the save's display name is used ("01.CREATED SUPERSTAR").
std::string LabelOf(const fs::path& f) {
  const std::string n = PathStr(f.filename());
  const int k = std::isdigit(uint8_t(n[0])) && std::isdigit(uint8_t(n[1])) ? std::atoi(n.substr(0, 2).c_str()) : 0;
  char b[64];
  std::snprintf(b, sizeof b, "%02d.CREATED SUPERSTAR", k + 1);
  return b;
}

void LoadCaw(Caw& c) {
  c.loaded = true;
  Bytes d;
  if (!ReadFile(PathStr(c.file), d) || d.size() != kCasSize) {
    c.error = "not a Created Superstar save (" + std::to_string(d.size()) + " bytes)";
    return;
  }
  c.slot = Le16(&d[kRecordAt + 0x20]) ? Le16(&d[kRecordAt + 0x20]) : Be16(&d[kRecordAt + 0x20]);
  c.slot = Be16(&d[kRecordAt + 0x20]);
  for (int a = 0; a < 4; ++a) {
    Attire& at = c.attires[a];
    const uint8_t* s = &d[kSlotAt + size_t(a) * kSlotSize];
    bool any = false;
    for (size_t i = 0; i < kRenderBytes && !any; i += 97) any |= s[i] != 0;
    at.present = any;
    if (!any) continue;
    Image img;
    if (DecodeRaw(s, 256, 512, img)) at.render.Set(std::move(img));
    if (DecodeRaw(s + kThumbAt, 128, 128, img)) at.thumb.Set(std::move(img));
  }
}

void Scan() {
  g_caws.clear();
  g_sel = -1;
  g_loaded_for = g_game;
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Saves", ec))
    if (e.is_regular_file(ec) && Lower(PathStr(e.path().filename())).ends_with("createsuperstar.cas")) files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (const auto& f : files) {
    Caw c;
    c.file = f;
    c.label = LabelOf(f);
    if (FILE* fp = _wfopen(f.c_str(), L"rb")) {  // (the record's name only: the whole save is read when picked)
      char n[41] = {};
      if (std::fseek(fp, long(kRecordAt + 0x22), SEEK_SET) == 0 && std::fread(n, 1, 40, fp) == 40) c.name = n;
      std::fclose(fp);
      if (!c.name.empty()) c.label = c.label.substr(0, 3) + " " + Shown(c.name);
    }
    g_caws.push_back(std::move(c));
  }
  LoadCalls();
}

// A picture fitted into 256 x 512 (centred, transparent around) and its head thumb.
Image FitRender(const Image& src) {
  Image out;
  out.w = 256, out.h = 512;
  out.rgba.assign(size_t(256) * 512 * 4, 0);
  const float k = std::min(256.0f / src.w, 512.0f / src.h);
  const int w = std::max(1, int(src.w * k)), h = std::max(1, int(src.h * k));
  const Image fit = Resize(src, w, h);
  const int x0 = (256 - w) / 2, y0 = (512 - h) / 2;
  for (int y = 0; y < h; ++y) std::memcpy(&out.rgba[(size_t(y + y0) * 256 + x0) * 4], &fit.rgba[size_t(y) * w * 4], size_t(w) * 4);
  return out;
}
Image HeadThumb(const Image& render) {  // the top 256 rows' middle, as the game's head-and-shoulders crop
  Image head;
  head.w = head.h = 192;
  head.rgba.resize(size_t(192) * 192 * 4);
  // the figure's horizontal middle (alpha-weighted) in the top quarter
  double sx = 0, sa = 0;
  for (int y = 0; y < 160; ++y)
    for (int x = 0; x < 256; ++x) {
      const double a = render.rgba[(size_t(y) * 256 + x) * 4 + 3];
      sx += a * x, sa += a;
    }
  const int cx = sa > 0 ? int(sx / sa) : 128;
  const int left = std::clamp(cx - 96, 0, 256 - 192);
  // the top of the figure
  int top = 0;
  for (; top < 400; ++top) {
    bool any = false;
    for (int x = 0; x < 256 && !any; ++x) any = render.rgba[(size_t(top) * 256 + x) * 4 + 3] > 40;
    if (any) break;
  }
  top = std::clamp(top - 8, 0, 512 - 192);
  for (int y = 0; y < 192; ++y) std::memcpy(&head.rgba[size_t(y) * 192 * 4], &render.rgba[(size_t(y + top) * 256 + left) * 4], 192 * 4);
  return Resize(head, 128, 128);
}

bool Save(Caw& c, std::string& err) {
  if (GameRunning()) { err = "the game is running: close it first (it keeps the saves open)"; return false; }
  Bytes d;
  if (!ReadFile(PathStr(c.file), d) || d.size() != kCasSize) { err = "could not read the save"; return false; }
  const fs::path save_dat = fs::path(g_game) / L"Saves" / L"SaveData.dat";
  Bytes sd;
  const bool have_sd = ReadFile(PathStr(save_dat), sd) && sd.size() > kSaveSumTo;
  // backups once
  std::error_code ec;
  // (in Saves\.backup: the game's saves folder takes every file in it as a save - a .bak beside them got a header)
  const fs::path backups = c.file.parent_path() / L".backup";
  fs::create_directories(backups, ec);
  const fs::path cas_bak = backups / (c.file.filename().wstring() + L".bak"), sd_bak = backups / L"SaveData.dat.bak";
  if (!fs::exists(cas_bak, ec)) fs::copy_file(c.file, cas_bak, ec);
  if (have_sd && !fs::exists(sd_bak, ec)) fs::copy_file(save_dat, sd_bak, ec);
  int64_t delta = 0;
  for (int a = 0; a < 4; ++a) {
    Attire& at = c.attires[a];
    if (at.new_render.rgba.empty()) continue;
    uint8_t* s = &d[kSlotAt + size_t(a) * kSlotSize];
    const Bytes render = EncodeRaw(at.new_render);
    const Bytes thumb = EncodeRaw(at.thumb_from_render || at.new_thumb.rgba.empty() ? HeadThumb(at.new_render) : at.new_thumb);
    if (render.size() != kRenderBytes || thumb.size() != kThumbBytes) { err = "the picture did not encode"; return false; }
    delta -= int64_t(Sum(d, kSlotAt + size_t(a) * kSlotSize, kRenderField)) + int64_t(Sum(d, kSlotAt + size_t(a) * kSlotSize + kThumbAt, kThumbField));
    std::memcpy(s, render.data(), kRenderBytes);
    std::memset(s + kRenderBytes, 0, kRenderField - kRenderBytes);
    std::memcpy(s + kThumbAt, thumb.data(), kThumbBytes);
    delta += int64_t(Sum(d, kSlotAt + size_t(a) * kSlotSize, kRenderField)) + int64_t(Sum(d, kSlotAt + size_t(a) * kSlotSize + kThumbAt, kThumbField));
  }
  const uint32_t was = Be(d, kSum1), sum = uint32_t(int64_t(was) + delta);
  PutBe(d, kSum1, sum), PutBe(d, kSum2, sum), PutBe(d, kSum3, sum);
  if (!WriteFile(PathStr(c.file), d)) { err = "could not write the save"; return false; }
  // (problem reports - "my CAW won't load": what changed, and the sums before and after)
  std::string attires;
  for (int a = 0; a < 4; ++a)
    if (!c.attires[a].new_render.rgba.empty())
      attires += (attires.empty() ? "" : "+") + std::to_string(a + 1) + (c.attires[a].present ? "" : " (had no picture)");
  char line[512];
  std::snprintf(line, sizeof line, "CAW pictures: %s (slot %d) attire %s: picture %zu + head %zu bytes; checksum %08X -> %08X",
                PathStr(c.file.filename()).c_str(), c.slot, attires.c_str(), kRenderBytes, kThumbBytes, unsigned(was), unsigned(sum));
  std::string sd_note = "; SaveData.dat not changed";
  if (have_sd && c.slot >= 0 && c.slot < 50) {
    const size_t rec = kSaveRecords + size_t(c.slot) * kSaveRecord;
    if (rec + 0x660 <= sd.size()) {
      const uint32_t rec_was = Be(sd, rec + 0x65C), file_was = Be(sd, kSaveSumAt);
      PutBe(sd, rec + 0x65C, sum);
      PutBe(sd, kSaveSumAt, uint32_t(Sum(sd, kSaveSumFrom, kSaveSumTo - kSaveSumFrom)));
      char n[200];
      std::snprintf(n, sizeof n, "; SaveData.dat record %08X -> %08X%s, file sum %08X -> %08X", unsigned(rec_was), unsigned(sum),
                    rec_was == was ? "" : " (it didn't match the .cas before)", unsigned(file_was), unsigned(Be(sd, kSaveSumAt)));
      sd_note = n;
      if (!WriteFile(PathStr(save_dat), sd)) {
        Log(std::string(line) + sd_note + " - SaveData.dat NOT WRITTEN");
        err = "the .cas is written but SaveData.dat could not be (the game may call the CAW damaged)";
        return false;
      }
    }
  }
  Log(line + sd_note);
  for (auto& at : c.attires) {
    if (at.new_render.rgba.empty()) continue;
    at.render.Set(at.new_render);
    at.thumb.Set(at.thumb_from_render || at.new_thumb.rgba.empty() ? HeadThumb(at.new_render) : at.new_thumb);
    at.new_render = Image();
    at.new_thumb = Image();
  }
  return true;
}

}  // namespace

void TestAnnouncer(const std::string& clip) { g_test_announcer = clip; }

void TestStart(int index, int attire, const std::wstring& picture, bool save) {
  g_test_index = index, g_test_attire = attire, g_test_picture = picture, g_test_save = save;
}

void Draw() {
  if (g_test_index >= 0 && !g_game.empty()) {
    if (g_loaded_for != g_game) Scan();
    if (g_test_index < int(g_caws.size())) {
      g_sel = g_test_index;
      Caw& c = g_caws[g_sel];
      if (!c.loaded) LoadCaw(c);
      Image img;
      if (!g_test_picture.empty() && g_test_attire >= 0 && g_test_attire < 4 && LoadPicture(g_test_picture, img)) {
        c.attires[g_test_attire].new_render = FitRender(img);
        c.attires[g_test_attire].thumb_from_render = true;
        if (g_test_save) {
          std::string err;
          Log(Save(c, err) ? "test: caw saved" : "test: caw not saved: " + err);
          WriteLogTo(g_test_picture + L".caw.log");
          PostMessageW(g_wnd, WM_CLOSE, 0, 0);
        }
      }
      if (!g_test_announcer.empty() && !c.name.empty()) {
        Log(SetCall(c.name, g_test_announcer == "-" ? "" : g_test_announcer) ? "test: caw announcer set" : "test: caw announcer not written");
        WriteLogTo(CallsFile().wstring() + L".log");
        PostMessageW(g_wnd, WM_CLOSE, 0, 0);
      }
    }
    g_test_index = -1;
  }
  Heading("CAW pictures", "The picture the game shows for a Created Superstar in its lists (the one FINALIZE made), "
                          "replaced from any picture of yours. Edits go into the save in the game's Saves folder; a "
                          "copy is kept the first time (Saves\\.backup). The Ring announcer row gives a CAW a superstar's own introduction.");
  if (g_game.empty()) {
    ImGui::TextDisabled("No game folder.");
    return;
  }
  if (g_loaded_for != g_game) Scan();
  ImGui::BeginChild("list", ImVec2(260 * g_scale, 0), true);
  if (ImGui::SmallButton("Rescan")) Scan();
  ImGui::SameLine();
  if (ImGui::SmallButton("Import a .cas...")) {
    if (GameRunning()) {
      Status("The game is running: close it first.");
    } else {
      const COMDLG_FILTERSPEC spec[] = {{L"Created Superstar (*.cas, Xbox 360 package)", L"*.cas;*"}};
      const std::wstring f = PickFile(false, L"A Created Superstar save to bring into the game's saves", spec, 1);
      if (!f.empty()) {
        wchar_t name[40] = L"", err[512] = L"";
        CawLogo* logos = nullptr;
        int n_logos = 0;
        const std::wstring saves = (fs::path(g_game) / L"Saves").wstring();
        const int slot = caw_import(f.c_str(), saves.c_str(), name, 40, &logos, &n_logos, err, 512);
        if (logos) free(logos);
        if (slot < 0) Status("Not imported: " + Utf8(err));
        else Status("Imported " + Utf8(name) + " into slot " + std::to_string(slot + 1) + (n_logos ? " (its Paint Tool logos: import it in the launcher's Saves tab to get those too)" : "") + ".");
        Scan();
      }
    }
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Someone else's Created Superstar (.cas, or an Xbox 360 package) into a free slot of the game's saves.");
  if (g_caws.empty()) ImGui::TextWrapped("No Created Superstar saves in %s.", PathStr(fs::path(g_game) / L"Saves").c_str());
  for (int i = 0; i < int(g_caws.size()); ++i)
    if (ImGui::Selectable((g_caws[i].label + (CallFor(g_caws[i].name).empty() ? "" : "  > " + CallFor(g_caws[i].name)) + "##" +
                           std::to_string(i)).c_str(), g_sel == i)) {
      g_sel = i;
      if (!g_caws[i].loaded) LoadCaw(g_caws[i]);
    }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("caw", ImVec2(0, 0), false);
  if (g_sel < 0) {
    ImGui::TextDisabled("Pick a Created Superstar. Each attire has its own picture: the 256 x 512 render the lists "
                        "show and the 128 x 128 head the Community Creations preview uses.");
    ImGui::EndChild();
    return;
  }
  Caw& c = g_caws[g_sel];
  if (!c.error.empty()) {
    ImGui::TextColored(kBad, "%s", c.error.c_str());
    ImGui::EndChild();
    return;
  }
  ImGui::Text("%s  (save slot %d, %s)", c.label.c_str(), c.slot, PathStr(c.file.filename()).c_str());
  if (!c.name.empty()) {  // the ring announcer: a stock superstar's name call for this CAW (caw_announcer.txt)
    char clip[32];
    std::snprintf(clip, sizeof clip, "%s", CallFor(c.name).c_str());
    if (star_page::PickClip("Ring announcer", clip, sizeof clip, star_page::AnnouncerNames(), "the game's own (a nickname)", 300)) {
      if (SetCall(c.name, clip)) Status(clip[0] ? Shown(c.name) + " is announced as " + clip + " (Mods\\caw_announcer.txt)." : Shown(c.name) + ": the game's own call again.");
      else Status("Could not write " + PathStr(CallsFile()) + ".");
    }
    Hint("The ring announcer introduces this Created Superstar with a superstar's own recorded introduction and name "
         "(JEFFHARDY: \"... from Cameron, North Carolina, Jeff Hardy!\"). Written to Mods\\caw_announcer.txt at once - "
         "no save needed, the game picks it up even while running. Keyed by the name, so a renamed CAW needs it again.");
  }
  bool pending = false;
  for (int a = 0; a < 4; ++a) {
    Attire& at = c.attires[a];
    if (!at.present) continue;
    ImGui::PushID(a);
    ImGui::BeginGroup();
    ImGui::Text("Attire %d", a + 1);
    Picture& shown = at.render;
    static Picture preview[4];
    if (!at.new_render.rgba.empty()) {
      if (preview[a].Empty() || preview[a].image.rgba != at.new_render.rgba) preview[a].Set(at.new_render);
      preview[a].Draw(160 * g_scale, 320 * g_scale);
      ImGui::TextColored(kWarn, "new (not saved yet)");
      pending = true;
    } else {
      preview[a].Clear();
      shown.Draw(160 * g_scale, 320 * g_scale);
      ImGui::TextDisabled("256 x 512 render");
    }
    at.thumb.Draw(64 * g_scale, 64 * g_scale);
    ImGui::SameLine();
    ImGui::TextDisabled("128 x 128 head\n(Community Creations)");
    if (ImGui::Button("Picture...", ImVec2(160 * g_scale, 0))) {
      const std::wstring f = PickFile(false, L"The new picture (a PNG with a transparent background fits the lists best)", kPictureFilter, 1);
      Image img;
      if (!f.empty() && LoadPicture(f, img)) at.new_render = FitRender(img), at.thumb_from_render = true;
    }
    if (!at.new_render.rgba.empty()) {
      if (ImGui::Button("Head picture...", ImVec2(160 * g_scale, 0))) {
        const std::wstring f = PickFile(false, L"The head picture (shown at 128 x 128)", kPictureFilter, 1);
        Image img;
        if (!f.empty() && LoadPicture(f, img)) at.new_thumb = Resize(img, 128, 128), at.thumb_from_render = false;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Else the head is cut from the top of the new picture.");
      if (ImGui::Button("Undo", ImVec2(160 * g_scale, 0))) at.new_render = Image(), at.new_thumb = Image();
    }
    if (!at.render.Empty() && at.new_render.rgba.empty() && ImGui::Button("Export PNG...", ImVec2(160 * g_scale, 0))) {
      const COMDLG_FILTERSPEC spec[] = {{L"PNG picture (*.png)", L"*.png"}};
      const std::wstring f = PickFile(true, L"Export the render", spec, 1, L"png", L"caw_render.png");
      if (!f.empty()) Status(SavePng(Utf8(f), at.render.image) ? "Exported " + Utf8(f) : "Could not write " + Utf8(f));
    }
    ImGui::EndGroup();
    ImGui::PopID();
    ImGui::SameLine(0, 24 * g_scale);
  }
  ImGui::NewLine();
  ImGui::Separator();
  if (GameRunning()) ImGui::TextColored(kWarn, "The game is running: close it before saving (it keeps the saves open).");
  ImGui::BeginDisabled(!pending || GameRunning());
  PushAccent();
  if (ImGui::Button("Save into the game's save", ImVec2(260 * g_scale, 0))) {
    std::string err;
    if (Save(c, err)) Status("Saved: " + c.label + "'s pictures replaced (" + PathStr(c.file.filename()) + "; the first version kept in Saves\\.backup).");
    else Status("Not saved: " + err + ".");
  }
  PopAccent();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("The picture's background should be transparent: the game draws it over its own backdrops.");
  ImGui::EndChild();
}

}  // namespace caw_page
}  // namespace mm
