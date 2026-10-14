// The Superstar page's Simple mode: a step-by-step front end over the same
// project (g_star) for a first superstar. Each step is one card - a title, one
// "What is this?" line, only that step's controls - beside the 3D view; the mod
// is valid from the first step (the fighting style's superstar fills in what
// isn't chosen yet). The full page stays one click away and nothing is lost
// going back and forth.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>

#include "app.h"
#include "char_preview.h"
#include <cmath>
#include <memory>

#include "star_page_internal.h"
#include "svrfmt/png.h"
#include "tool_run.h"

namespace mm {
namespace star_page {

namespace {

struct StepInfo {
  const char* title;
  const char* what;  // the "What is this?" line (short, plain)
};
const StepInfo kSteps[kStepCount] = {
    {"Name and style", "Give your superstar a name and pick how they fight. The style gives them a full set of moves to start with."},
    {"Look", "This is what your superstar looks like: their body, face and clothes."},
    {"Attires", "Attires are outfits. A superstar can have up to 4, and you pick one before a match."},
    {"Music", "The song that plays when your superstar walks out to the ring."},
    {"Entrance", "How your superstar walks out: the moves on the way to the ring, the lights and the big screen video."},
    {"Moves", "Your superstar's best moves: the signature move and the finishers."},
    {"Taunts", "Taunts are the poses and gestures your superstar does to show off during a match."},
    {"Pictures", "The picture on the character select screen, and the pose your superstar strikes there."},
    {"Extras", "Fine-tuning: how strong your superstar is, how tall, and what the announcer calls them."},
    {"Finish", "Check everything, then save your superstar or put them in the game."},
};

// Which game superstar a model file is (pac\ch\chNNN.pac of the game folder), 0 if it isn't one.
int GameStarOf(const std::wstring& model) {
  const fs::path p(model);
  const std::wstring stem = p.stem().wstring();
  if (stem.size() < 3 || stem.rfind(L"ch", 0) != 0 || Lower(Utf8(p.parent_path().filename().wstring())) != "ch") return 0;
  for (size_t k = 2; k < stem.size(); ++k)
    if (!iswdigit(stem[k])) return 0;
  return _wtoi(stem.c_str() + 2);
}

std::wstring GameChPac(int id) { return (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(id) + L".pac")).wstring(); }

// A big selectable card: a title and a line under it.
bool Card(const char* id, const char* title, const char* line, bool on, float w, float h) {
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool hit = ImGui::InvisibleButton("##card", ImVec2(w, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = ImGui::GetColorU32(on ? ImGuiCol_HeaderActive : ImGui::IsItemHovered() ? ImGuiCol_HeaderHovered : ImGuiCol_FrameBg);
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 6 * g_scale);
  if (on) dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_Text), 6 * g_scale, 0, 2 * g_scale);
  const float pad = 10 * g_scale;
  dl->AddText(ImVec2(p.x + pad, p.y + pad), ImGui::GetColorU32(ImGuiCol_Text), title);
  dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
  dl->AddText(nullptr, 0, ImVec2(p.x + pad, p.y + pad + ImGui::GetTextLineHeightWithSpacing()),
              on ? IM_COL32(255, 255, 255, 220) : ImGui::GetColorU32(ImGuiCol_TextDisabled), line,
              nullptr, w - 2 * pad);
  dl->PopClipRect();
  ImGui::PopID();
  return hit;
}

// cards: as many as fit a row, at least 200 wide
float CardWidth() {
  const float avail = ImGui::GetContentRegionAvail().x, gap = 8 * g_scale;
  const int n = std::max(1, int((avail + gap) / (200 * g_scale + gap)));
  return std::min(280 * g_scale, (avail - gap * float(n - 1)) / float(n));
}
int CardsPerRow(float w) { return std::max(1, int((ImGui::GetContentRegionAvail().x + 8 * g_scale) / (w + 8 * g_scale))); }

// A button on the heading's title line, at the right (cursor kept below the heading).
bool HeadingButton(const char* label, float top_y, bool accent) {
  const ImVec2 after = ImGui::GetCursorPos();
  const float bw = ImGui::CalcTextSize(label).x + 40 * g_scale;
  ImGui::SetCursorPos(ImVec2(std::max(0.0f, ImGui::GetWindowWidth() - bw - 24 * g_scale), top_y));
  if (accent) PushAccent();
  const bool hit = ImGui::Button(label, ImVec2(bw, 0));
  if (accent) PopAccent();
  ImGui::SetCursorPos(after);
  return hit;
}

void StepTitle(int step) {
  ImGui::PushFont(nullptr);
  ImGui::SetWindowFontScale(1.45f);
  ImGui::TextUnformatted(kSteps[step].title);
  ImGui::SetWindowFontScale(1.0f);
  ImGui::PopFont();
  ImGui::TextColored(ImVec4(0.6f, 0.75f, 1, 1), "What is this?");
  ImGui::SameLine();
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(kSteps[step].what);
  ImGui::PopTextWrapPos();
  ImGui::Separator();
  ImGui::Spacing();
}

// A step's choice still the style's: said plainly.
void FromStyle(const char* what) {
  ImGui::TextDisabled("%s: from %s (your fighting style).", what, kStyles[g_star.style].name);
}

// ---- the steps

void StepName() {
  TextField("Name", g_star.name, sizeof g_star.name, "your superstar's name", 260);
  TextField("Short name", g_star.short_name, sizeof g_star.short_name, "optional: for small spaces", 260);
  ImGui::Spacing();
  ImGui::TextUnformatted("How do they fight?");
  const float w = CardWidth(), h = 80 * g_scale;
  const int per_row = CardsPerRow(w);
  for (int i = 0; i < kStyleCount; ++i) {
    if (i % per_row) ImGui::SameLine(0, 8 * g_scale);
    if (Card(kStyles[i].name, kStyles[i].name, kStyles[i].about, g_star.style == i, w, h) && g_star.style != i) {
      const std::wstring was = StyleChPac();
      g_star.style = i;
      g_star.ratings_set = false;  // (the new style's attributes)
      if (g_star.model.empty() || g_star.model == was) g_star.model = StyleChPac();  // (still the style's look: follows it)
      Touch();
    }
  }
}

char g_find[64] = "";
int g_look_mode = -1;  // the Look step's way in (-1: by the project)
void StepLook() {
  int& mode = g_look_mode;  // 0 a game superstar, 1 an older disc, 2 an FBX
  if (mode < 0) mode = g_star.fbx.empty() ? 0 : 2;  // (a project with an FBX opens on it)
  const float w = CardWidth(), h = 80 * g_scale;
  const int per_row = CardsPerRow(w);
  if (Card("game", "A superstar in the game", "Use the look of anyone on the roster.", mode == 0, w, h)) mode = 0;
  if (1 % per_row) ImGui::SameLine(0, 8 * g_scale);
  if (Card("disc", "From an older game", "Bring someone in from SvR 2006 - 2010 or WWE '12 - 2K14.", mode == 1, w, h)) mode = 1;
  if (2 % per_row) ImGui::SameLine(0, 8 * g_scale);
  if (Card("fbx", "Import a 3D model", "Your own model from Blender, Unity, VRChat or Mixamo (FBX).", mode == 2, w, h)) mode = 2;
  ImGui::Spacing();
  const int now = GameStarOf(g_star.model);
  if (const StarInfo* s = now ? StarById(now) : nullptr) ImGui::Text("Now: looks like %s.", s->name.c_str());
  else if (!g_star.model.empty()) ImGui::Text("Now: %s.", FileName(g_star.model).c_str());
  ImGui::Spacing();
  if (mode == 0) {
    ImGui::SetNextItemWidth(260 * g_scale);
    ImGui::InputTextWithHint("##find", "Find a superstar...", g_find, sizeof g_find);
    ImGui::BeginChild("roster", ImVec2(0, 0), false);
    const float tw = 96 * g_scale, th = 120 * g_scale;
    const int per_row = std::max(1, int((ImGui::GetContentRegionAvail().x + 6 * g_scale) / (tw + 6 * g_scale)));
    int n = 0;
    const std::string f = Lower(g_find);
    for (const auto& st : Stars()) {
      if (!f.empty() && Lower(st.name).find(f) == std::string::npos) continue;
      if (n++ % per_row) ImGui::SameLine(0, 6 * g_scale);
      ImGui::PushID(st.id);
      ImGui::BeginGroup();
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const bool on = now == st.id;
      if (ImGui::IsRectVisible(ImVec2(tw, th))) {  // (pictures decode on first sight: only the visible ones)
        if (Picture* pic = StarRender(st.id); pic && !pic->Empty()) pic->Draw(tw, tw);
        else ImGui::Dummy(ImVec2(tw, tw));
      } else {
        ImGui::Dummy(ImVec2(tw, tw));
      }
      ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
      ImGui::TextUnformatted(st.name.c_str());
      ImGui::PopTextWrapPos();
      ImGui::EndGroup();
      const ImVec2 q = ImGui::GetItemRectMax();
      if (on) ImGui::GetWindowDrawList()->AddRect(p, q, ImGui::GetColorU32(ImGuiCol_Text), 4 * g_scale, 0, 2 * g_scale);
      if (ImGui::IsItemClicked()) g_star.model = GameChPac(st.id), Touch();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Look like %s", st.name.c_str());
      ImGui::PopID();
    }
    ImGui::EndChild();
  } else if (mode == 2) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Pick a rigged model (one with a skeleton). The Mod Maker matches its bones to the game's, checks it, and builds your superstar from it.");
    ImGui::PopTextWrapPos();
    FbxRows(true);
  } else {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Pick the disc, list its superstars and convert one. The Mod Maker reads your disc images as they are.");
    ImGui::PopTextWrapPos();
    if (const std::wstring got = port_page::EmbeddedResult(); !got.empty()) {
      ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "Converted. What should your superstar take from it?");
      PushAccent();
      if (ImGui::Button("Just the look", ImVec2(180 * g_scale, 0))) TakeFromMod(got, false), port_page::ClearEmbeddedResult();
      PopAccent();
      ImGui::SameLine();
      if (ImGui::Button("Everything (look, music, pictures, moves)", ImVec2(0, 0))) TakeFromMod(got, true), port_page::ClearEmbeddedResult();
      ImGui::Spacing();
    }
    port_page::SuperstarFlow();
  }
  ImGui::Spacing();
  if (ImGui::SmallButton("Use a model file instead...")) {
    const std::wstring f = PickFile(false, L"A character model (ch.pac)", kPacFilter, 1);
    if (!f.empty()) g_star.model = f, Touch();
  }
}

// A popup to pick a game superstar by name (true when picked).
bool RosterPopup(const char* id, int& picked) {
  bool hit = false;
  if (ImGui::BeginPopup(id)) {
    static char find[64] = "";
    if (ImGui::IsWindowAppearing()) find[0] = 0, ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(240 * g_scale);
    ImGui::InputTextWithHint("##f", "Find a superstar...", find, sizeof find);
    ImGui::BeginChild("names", ImVec2(240 * g_scale, 300 * g_scale));
    const std::string f = Lower(find);
    for (const auto& st : Stars()) {
      if (!f.empty() && Lower(st.name).find(f) == std::string::npos) continue;
      if (ImGui::Selectable((st.name + "##" + std::to_string(st.id)).c_str())) picked = st.id, hit = true, ImGui::CloseCurrentPopup();
    }
    ImGui::EndChild();
    ImGui::EndPopup();
  }
  return hit;
}

std::string LookName(const std::wstring& pac) {
  const int id = GameStarOf(pac);
  if (const StarInfo* s = id ? StarById(id) : nullptr) return s->name + "'s";
  return FileName(pac);
}

void StepAttires() {
  const std::vector<int> own = g_star.model.empty() ? std::vector<int>() : ModelAttires(g_star.model);
  auto own_has = [&](int n) {  // (the game counts them in order from 1)
    for (int k = 1; k <= n; ++k)
      if (std::find(own.begin(), own.end(), k) == own.end()) return false;
    return true;
  };
  ImGui::TextUnformatted("Attire 1 is your superstar's look (the Look step).");
  ImGui::Spacing();
  for (int a = 1; a < 4; ++a) {
    ImGui::PushID(a);
    const bool from_model = g_star.attires[a].empty() && own_has(a + 1);
    const std::string now = !g_star.attires[a].empty() ? LookName(g_star.attires[a]) + " outfit"
                            : from_model                ? std::string("the look's own attire ") + std::to_string(a + 1)
                                                        : std::string("none");
    ImGui::Text("Attire %d:", a + 1);
    ImGui::SameLine(90 * g_scale);
    ImGui::TextUnformatted(now.c_str());
    ImGui::Indent(90 * g_scale);
    if (ImGui::SmallButton("Another superstar's...")) ImGui::OpenPopup("roster");
    int picked = 0;
    if (RosterPopup("roster", picked)) g_star.attires[a] = GameChPac(picked), Touch();
    ImGui::SameLine();
    if (ImGui::SmallButton("A model file...")) {
      const std::wstring f = PickFile(false, L"A character model (ch.pac): its first attire", kPacFilter, 1);
      if (!f.empty()) g_star.attires[a] = f, Touch();
    }
    if (!g_star.attires[a].empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Remove")) g_star.attires[a].clear(), Touch();
    }
    if (!g_star.attires[a].empty() || from_model) {
      ImGui::SameLine();
      if (ImGui::SmallButton("See it")) g_view_attire = a + 1;
      ImGui::SetNextItemWidth(220 * g_scale);
      if (ImGui::InputTextWithHint("##name", ("its name (ATTIRE " + std::to_string(a + 1) + ")").c_str(), g_star.attire_names[a],
                                   sizeof g_star.attire_names[a]))
        Touch();
    }
    ImGui::Unindent(90 * g_scale);
    ImGui::Spacing();
    ImGui::PopID();
    if (g_star.attires[a].empty() && !from_model) break;  // (in order: the next one after this)
  }
  ImGui::Separator();
  GearRows(true);
}

void StepMusic() {
  FileRow("Pick a song...", g_star.song, g_star.music > 0 ? "the game's theme below" : "the style's", kSoundFilter, 1,
          "An MP3, WAV, OGG or other sound file.");
  if (!g_star.song.empty() && g_star.music) g_star.music = 0;  // (a song file picked: it wins)
  ImGui::TextDisabled("or");
  StockThemeRow();
  if (g_star.song.empty() && !g_star.music) FromStyle("Music");
  ImGui::Spacing();
  ThemeGainRow();
  if (!g_star.song.empty()) ImGui::TextDisabled("Tip: \"Match the game\" makes it as loud as the game's own songs.");
}

// The entrance's walk to the ring on the model: tools/entrance_ring.py turns entrance n's own track bank (its
// pac\evt Nyujyo pac, or a mod's move pack) into a YMKs bank keyed as motion n, x = segment (4 = the ring part).
int g_ring_shown = -1;  // the entrance the preview shows
void WatchEntrance(int n) {
  const fs::path work = fs::path(g_game) / L"Mods" / L".convert" / L"entrance_ring";
  std::error_code ec;
  fs::create_directories(work, ec);
  const std::vector<std::wstring> args = {std::to_wstring(n), L"--game-pac", (fs::path(g_game) / L"pac").wstring(), L"--mods",
                                          (fs::path(g_game) / L"Mods").wstring(), L"--out", work.wstring()};
  const bool started = RunToolJson(L"entrance_ring.py", args, [n, work](int code, const std::string& text) {
    svrfmt::Bytes b;
    if (code != 0 || !svrfmt::ReadFile(PathStr(work / (L"entrance_" + std::to_wstring(n) + L".ymks")), b)) {
      Status(code == 2 ? "That entrance has no walk to show." : "The entrance couldn't be read (see the log).");
      Log("entrance_ring: " + text);
      return;
    }
    auto bank = std::make_shared<const std::vector<uint8_t>>(std::move(b));
    std::string err;
    char_preview::SetDummy(L"");
    if (!char_preview::SetMotion(bank, n, 4, &err)) return Status("The entrance can't be shown: " + err + ".");
    char_preview::Play().loop = true;
    g_ring_shown = n;
    Status("The entrance's walk to the ring.");
  });
  if (!started) Status("The entrance preview needs the Mod Maker tools (Python): see the log.");
}

void StepEntrance() {
  if (EntrancePicker("Entrance", g_star.entrance, "The style's", 360)) Touch();
  if (g_star.entrance < 0) FromStyle("Entrance");
  {
    const int n = g_star.entrance >= 0 ? g_star.entrance : kStyles[g_star.style].template_id;  // (the style's: its superstar's)
    ImGui::BeginDisabled(Busy());
    if (ImGui::Button("Watch the walk to the ring", ImVec2(240 * g_scale, 0))) WatchEntrance(n);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Your superstar does the entrance's ring part in the 3D view (the lights, pyro and video only show in the game).");
    ImGui::SameLine();
    if (ImGui::Button("Watch the whole entrance", ImVec2(240 * g_scale, 0))) OpenFullPreview(n);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("A window with the whole entrance in the practice arena: the entrance's camera, your theme and big screen video.");
  }
  ImGui::Spacing();
  ImGui::TextUnformatted("Big screen video (optional)");
  FileRow("Pick a video file...", g_star.movie, "the style's", kBinkFilter, 1, "A video made for the game's big screen (.bik).");
  ImGui::SameLine();
  BinkFromVideoButton("Make one from any video...", g_star.movie, g_star.name);
}

// A move-set slot as a row: its move (yours or the style's), Change..., Watch it, the style's again.
void SlotRow(int slot, const char* label, bool two_people) {
  ImGui::PushID(slot);
  const std::vector<int> base = ProfileMoves(kStyles[g_star.style].template_id);
  const auto mine = g_star.moveset.find(slot);
  const int was = slot < int(base.size()) ? base[size_t(slot)] : 0;
  const int now = mine != g_star.moveset.end() ? mine->second : was;
  ImGui::TextUnformatted(label);
  ImGui::SameLine(150 * g_scale);
  ImGui::TextUnformatted(now ? MoveName(now).c_str() : "-");
  if (mine == g_star.moveset.end()) ImGui::SameLine(), ImGui::TextDisabled("(the style's)");
  else MoveNoteTag(slot);
  ImGui::Indent(150 * g_scale);
  if (ImGui::SmallButton("Change...")) g_pick_slot = slot, g_pick_find[0] = 0, g_pick_any = false;
  ImGui::SameLine();
  ImGui::BeginDisabled(!now || !anims_page::HasMotion(now) || Busy());
  if (ImGui::SmallButton("Watch it")) {
    char_preview::SetDummy(two_people ? StyleChPac() : std::wstring());
    anims_page::WatchMove(now);
    g_watching = true;
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && now && !anims_page::HasMotion(now))
    ImGui::SetTooltip(anims_page::MovesIndexed() ? "This move can only be watched in the game." : "Reading the game's moves ...");
  if (mine != g_star.moveset.end()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Use the style's")) g_star.moveset.erase(slot), g_star.moveset_note.erase(slot), Touch();
  }
  ImGui::Unindent(150 * g_scale);
  ImGui::Spacing();
  ImGui::PopID();
}

void StepMoves() {
  SlotRow(0x1E / 2, "Signature move", true);
  SlotRow(0x20 / 2, "Signature move 2", true);
  SlotRow(0x22 / 2, "Finisher", true);
  SlotRow(0x24 / 2, "Finisher 2", true);
  MovePicker();
  ImGui::Spacing();
  ImGui::TextDisabled("All the other moves come from %s. The full editor's Moveset changes any of them.", kStyles[g_star.style].name);
}

void StepTaunts() {
  for (int k = 0; k < 4; ++k) SlotRow((0x12 + 2 * k) / 2, ("Taunt " + std::to_string(k + 1)).c_str(), false);
  MovePicker();
}

void StepPictures() {
  ImGui::TextUnformatted("Pose on the select screen");
  ImGui::SetNextItemWidth(300 * g_scale);
  const std::string cur = g_star.pose < 0 ? "The style's" : PoseLabel(g_star.pose);
  if (ImGui::BeginCombo("##pose", cur.c_str(), ImGuiComboFlags_HeightLarge)) {
    if (ImGui::Selectable("The style's", g_star.pose < 0)) g_star.pose = -1, Touch();
    for (const auto& c : Poses())
      if (ImGui::Selectable((c.label + "##" + std::to_string(c.id)).c_str(), g_star.pose == c.id)) g_star.pose = c.id, Touch(), PreviewPose(c.id);
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(g_star.pose < 0 || Busy());
  if (ImGui::SmallButton("Show it")) PreviewPose(g_star.pose);
  ImGui::EndDisabled();
  ImGui::Spacing();
  ImGui::TextUnformatted("The select screen picture");
  ImGui::PushTextWrapPos(0);
  ImGui::TextDisabled("Turn and zoom the 3D view, play the pose, then take it - or use a picture of your own (a PNG with a see-through background works best).");
  ImGui::PopTextWrapPos();
  PushAccent();
  if (ImGui::Button("Take it from the 3D view", ImVec2(220 * g_scale, 0))) {
    std::vector<uint8_t> rgba;
    if (char_preview::Capture(768, rgba)) {
      Image img;
      img.w = img.h = 768;
      img.rgba = std::move(rgba);
      g_star.picture_src = img;
      SetPicture(img);
      Status("The select picture is the 3D view's.");
    } else {
      Status("The 3D view has nothing to take yet.");
    }
  }
  PopAccent();
  ImGui::SameLine();
  if (ImGui::Button("Use my own picture...", ImVec2(200 * g_scale, 0))) {
    const std::wstring f = PickFile(false, L"The select screen picture", kPictureFilter, 1);
    Image img;
    if (!f.empty() && LoadPicture(f, img)) g_star.picture_src = img, SetPicture(img);
  }
  if (!g_star.picture.Empty()) {
    ImGui::Spacing();
    g_star.picture.Draw(180 * g_scale, 180 * g_scale);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled("How the select screen shows it.");
    if (ImGui::Checkbox("Game look", &g_star.game_look) && !g_star.picture_src.rgba.empty()) SetPicture(g_star.picture_src);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Moves the colours toward the game's own pictures.");
    if (ImGui::SmallButton("Remove")) g_star.picture.Clear(), g_star.picture_small = Image(), Touch();
    ImGui::EndGroup();
  }
}

void StepExtras() {
  ImGui::Text("Strength - overall %d", OverallRating(g_star.ratings));
  for (int k = 0; k < 7; ++k) {
    ImGui::SetNextItemWidth(260 * g_scale);
    if (ImGui::SliderInt(kAttributes[k], &g_star.ratings[k], 1, 99)) g_star.ratings_set = true, Touch();
  }
  ImGui::Spacing();
  ImGui::SetNextItemWidth(260 * g_scale);
  if (ImGui::SliderFloat("Height", &g_star.height, 0.8f, 1.25f, "x%.2f")) Touch();
  if (const float tall = char_preview::ModelHeight(); tall > 0) {
    ImGui::SameLine();
    ImGui::TextDisabled("about %d cm", int(std::lround(tall * 10 * StarScale(kStyles[g_star.style].template_id) * g_star.height)));
  }
  ImGui::SetNextItemWidth(260 * g_scale);
  if (ImGui::BeginCombo("Announcer calls them", g_star.call < 0 ? "The Superstar" : kNickNames[g_star.call])) {
    if (ImGui::Selectable("The Superstar##d", g_star.call < 0)) g_star.call = -1, Touch();
    for (int i = 0; i < 84; ++i)
      if (ImGui::Selectable((std::string(kNickNames[i]) + "##n" + std::to_string(i)).c_str(), g_star.call == i)) g_star.call = i, Touch();
    ImGui::EndCombo();
  }
  ImGui::TextDisabled("More (abilities, hometown, the announcer's own name clips) in the full editor.");
}

// A problem's text without the file-level details a first-timer doesn't need ("(entry 00013902)").
std::string Plain(std::string t) {
  for (size_t a; (a = t.find(" (entry ")) != std::string::npos;) {
    const size_t b = t.find(')', a);
    if (b == std::string::npos) break;
    t.erase(a, b - a + 1);
  }
  return t;
}

void StepFinish() {
  ImGui::TextUnformatted("Your superstar");
  ImGui::Indent();
  auto row = [](int step, const std::string& text) {
    ImGui::PushID(step);
    ImGui::TextDisabled("%-14s", kSteps[step].title);
    ImGui::SameLine(150 * g_scale);
    ImGui::TextUnformatted(text.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Change")) g_star.simple_step = step;
    ImGui::PopID();
  };
  row(kStepName, std::string(g_star.name[0] ? g_star.name : "(no name yet)") + ", " + kStyles[g_star.style].name);
  const int now = GameStarOf(g_star.model);
  const StarInfo* s = now ? StarById(now) : nullptr;
  row(kStepLook, s ? "looks like " + s->name : g_star.model.empty() ? std::string("(none yet)") : FileName(g_star.model));
  {
    int extra = 0;
    for (int a = 1; a < 4; ++a) extra += !g_star.attires[a].empty();
    const int own = g_star.model.empty() ? 0 : int(ModelAttires(g_star.model).size());
    row(kStepAttires, std::to_string(std::max(own, 1 + extra)) + " attire" + (std::max(own, 1 + extra) == 1 ? "" : "s"));
  }
  row(kStepMusic, !g_star.song.empty() ? FileName(g_star.song) : g_star.music > 0 ? "the game's theme: " + StockThemeName(g_star.music) : std::string("the style's"));
  row(kStepEntrance, g_star.entrance < 0 ? std::string("the style's") : "entrance " + std::to_string(g_star.entrance));
  {
    int moves = 0, taunts = 0;
    for (const auto& [slot, id] : g_star.moveset) (slot * 2 >= 0x12 && slot * 2 <= 0x18 ? taunts : moves) += 1;
    row(kStepMoves, moves ? std::to_string(moves) + " of your own, the rest the style's" : std::string("the style's"));
    row(kStepTaunts, taunts ? std::to_string(taunts) + " of your own" : std::string("the style's"));
  }
  row(kStepPictures, g_star.picture.Empty() ? std::string("none yet (a silhouette)") : std::string("your picture") + (g_star.pose >= 0 ? ", your pose" : ""));
  ImGui::Unindent();
  ImGui::Spacing();
  std::vector<Problem> problems;
  Problems(problems);
  if (problems.empty()) {
    ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "Everything looks good.");
  } else {
    ImGui::TextUnformatted("Things to check");
    for (size_t k = 0; k < problems.size(); ++k) {
      const Problem& p = problems[k];
      ImGui::PushID(int(k));
      ImGui::TextColored(p.error ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(1, 0.8f, 0.35f, 1), p.error ? "Must fix:" : "Tip:");
      ImGui::SameLine();
      ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 90 * g_scale + ImGui::GetCursorPosX());
      ImGui::TextUnformatted(Plain(p.text).c_str());
      ImGui::PopTextWrapPos();
      if (p.step >= 0 && p.step != kStepFinish) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Fix it")) g_star.simple_step = p.step;
      }
      ImGui::PopID();
    }
  }
  ImGui::Spacing();
  ImGui::Spacing();
  switch (ModButtons(problems, true, (ImGui::GetContentRegionAvail().x - 16 * g_scale) / 3 / g_scale)) {
    case 1: SaveMod(); break;
    case 2: Install(false); break;
    case 3: Install(true); break;
  }
}

}  // namespace

void TestSimpleLook(int mode) { g_look_mode = mode; }
std::wstring g_test_capture;  // --simple-capture <png>: the 3D view taken as the select picture, saved, quit
void TestSimpleCapture(const std::wstring& png) { g_test_capture = png; }
int g_test_ring = -1;  // --simple-ring <entrance>: its walk to the ring in the preview (with --simple-capture: taken mid-walk)
void TestSimpleRing(int n) { g_test_ring = n; }

void SimpleDraw() {
  int& step = g_star.simple_step;
  step = std::clamp(step, 0, int(kStepCount) - 1);
  if (g_star.model.empty()) g_star.model = StyleChPac();  // (valid from the first step: the style's look)
  if (!g_star.ratings_set) {
    for (int k = 0; k < 7; ++k) g_star.ratings[k] = StarRating(kStyles[g_star.style].template_id, k);
    g_star.ratings_set = true;
  }
  const float top = ImGui::GetCursorPosY();
  Heading("Your superstar", "Step by step: each step has one thing to choose. Everything you skip comes from your fighting style.");
  if (HeadingButton("Full editor", top, false)) g_star.simple = false;
  std::vector<Problem> problems;
  Problems(problems);
  bool step_has_error[kStepCount] = {};
  for (const auto& p : problems)
    if (p.error && p.step >= 0 && p.step < kStepCount) step_has_error[p.step] = true;

  // the steps (left)
  ImGui::BeginChild("steps", ImVec2(180 * g_scale, -52 * g_scale), true);
  for (int i = 0; i < kStepCount; ++i) {
    const bool on = i == step;
    const bool done = i < step && !step_has_error[i];
    std::string label = std::to_string(i + 1) + ".  " + kSteps[i].title;
    if (step_has_error[i]) label += "  !";
    if (ImGui::Selectable(label.c_str(), on, 0, ImVec2(0, 30 * g_scale))) step = i;
    if (done) {  // a tick at the right (the font has no check mark)
      const ImVec2 mx = ImGui::GetItemRectMax(), mn = ImGui::GetItemRectMin();
      const float cy = (mn.y + mx.y) * 0.5f, x = mx.x - 22 * g_scale, k = g_scale;
      const ImVec2 pts[3] = {ImVec2(x, cy), ImVec2(x + 5 * k, cy + 5 * k), ImVec2(x + 14 * k, cy - 6 * k)};
      ImGui::GetWindowDrawList()->AddPolyline(pts, 3, IM_COL32(110, 220, 130, 255), 0, 2.5f * k);
    }
  }
  ImGui::EndChild();
  ImGui::SameLine();

  // the step's card (middle)
  ImGui::BeginChild("card", ImVec2(-330 * g_scale, -52 * g_scale), true);
  StepTitle(step);
  switch (step) {
    case kStepName: StepName(); break;
    case kStepLook: StepLook(); break;
    case kStepAttires: StepAttires(); break;
    case kStepMusic: StepMusic(); break;
    case kStepEntrance: StepEntrance(); break;
    case kStepMoves: StepMoves(); break;
    case kStepTaunts: StepTaunts(); break;
    case kStepPictures: StepPictures(); break;
    case kStepExtras: StepExtras(); break;
    default: StepFinish(); break;
  }
  ImGui::EndChild();
  ImGui::SameLine();

  static int capture_frames = 0;
  static bool ring_asked = false;
  if (g_test_ring >= 0 && !ring_asked && char_preview::ModelHeight() > 0 && !Busy()) ring_asked = true, WatchEntrance(g_test_ring);
  if (g_test_ring >= 0 && g_ring_shown >= 0 && char_preview::Play().key < 300) char_preview::Play().key = 300;  // (mid-walk)
  if (!g_test_capture.empty() && char_preview::ModelHeight() > 0 && (g_test_ring < 0 || g_ring_shown >= 0) && ++capture_frames > 30) {
    std::vector<uint8_t> rgba;
    Image img;
    img.w = img.h = 512;
    if (char_preview::Capture(512, rgba)) {
      img.rgba = std::move(rgba);
      SetPicture(img);
      svrfmt::SavePng(Utf8(g_test_capture), img);
      if (!g_star.picture.Empty()) svrfmt::SavePng(Utf8(g_test_capture) + ".render.png", g_star.picture.image);
    }
    g_test_capture.clear();
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  }
  // the 3D view (right): drag to turn, wheel to zoom
  ImGui::BeginChild("view", ImVec2(0, -52 * g_scale), true);
  Preview();  // (its footer: Reset view, Blood, Damage)
  ImGui::EndChild();

  // Back / Next (bottom)
  ImGui::BeginDisabled(step == 0);
  if (ImGui::Button("Back", ImVec2(140 * g_scale, 36 * g_scale))) --step;
  ImGui::EndDisabled();
  ImGui::SameLine();
  const float bar_w = std::max(100.0f, ImGui::GetContentRegionAvail().x - 160 * g_scale);
  ImGui::BeginGroup();
  ImGui::Text("Step %d of %d", step + 1, int(kStepCount));
  ImGui::ProgressBar(float(step + 1) / float(kStepCount), ImVec2(bar_w - 8 * g_scale, 10 * g_scale), "");
  ImGui::EndGroup();
  ImGui::SameLine(ImGui::GetWindowWidth() - 160 * g_scale);
  if (step + 1 < kStepCount) {
    PushAccent();
    if (ImGui::Button("Next", ImVec2(140 * g_scale, 36 * g_scale))) ++step;
    PopAccent();
  }
}

}  // namespace star_page
}  // namespace mm
