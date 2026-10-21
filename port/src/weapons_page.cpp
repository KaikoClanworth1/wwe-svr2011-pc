// WWE SmackDown vs. Raw 2011 - MY WWE -> OPTIONS -> WEAPONS (see weapons_page.h).

#include "weapons_page.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"

#include "graphics_page.h"
#include "p2p.h"

REXCVAR_DEFINE_STRING(extreme_weapons, "", "UI",
                      "MY WWE -> OPTIONS -> WEAPONS: the Extreme Rules weapons, <bgEtc weapon id>:<count 0-3>,... "
                      "(empty: the game's own set)");

namespace {

using Clock = std::chrono::steady_clock;

// The weapons the page offers (bgEtc.pac STG/WPON pack ids; the order is the
// online property's: 2 bits each - never reorder, only add at the end). The
// game's own Extreme Rules nine first, in its order (sub_82301148).
struct Weapon {
  int id;
  const char* name;
  int game;  // how many the game's own set has
  int kb;    // its pack in bgEtc.pac (KB): what loading it takes, roughly
};
constexpr Weapon kWeapons[] = {
    {87, "DRINK CUP", 1, 37},       {15, "TITLE BELT", 1, 83},    {60, "CRUTCH", 1, 42},        {89, "GUITAR", 1, 374},
    {11, "LADDER", 1, 135},         {88, "KENDO STICK", 1, 91},   {5, "TABLE", 1, 178},         {63, "TRASH CAN", 1, 157},
    {90, "MOP", 1, 221},            {4, "STEEL CHAIR", 0, 305},   {102, "SLEDGEHAMMER", 0, 59}, {65, "FIRE EXTINGUISHER", 0, 115},
    {27, "DUMBBELL", 0, 122},       {21, "BASEBALL BAT", 0, 198}, {84, "TIRE", 0, 216},
};
// The match's weapons keep the game's own shape: its nine Extreme Rules entries in the load list (one a weapon), nine
// weapons made from them - only their models change (the chosen ones, then the lightest chosen one again to make up
// nine: those stay hidden, never under the ring), within the KB of the game's nine. A longer list (22 models)
// crashed the load (lr 8215A008), so did weapons skipped or made twice from one entry (20 Oct).
int KbOf(uint32_t id) {
  for (const Weapon& w : kWeapons)
    if (uint32_t(w.id) == id) return w.kb;
  return 200;
}
constexpr int kCount = int(std::size(kWeapons));
static_assert(kCount <= 15, "the online property holds 15 weapons");
constexpr int kMaxEach = 3;
constexpr int kMaxTotal = 9;  // (the game's nine places)
constexpr uint32_t kProperty = 0x1000F002;
constexpr uint32_t kListCount = 72;

std::mutex g_mutex;
std::array<int, kCount> g_count;  // under g_mutex: this game's choice
bool g_custom = false;             // under g_mutex: not the game's own set

std::array<int, kCount> GameSet() {
  std::array<int, kCount> c{};
  for (int i = 0; i < kCount; ++i) c[i] = kWeapons[i].game;
  return c;
}

int Total(const std::array<int, kCount>& c) {
  int n = 0;
  for (int v : c) n += v;
  return n;
}

// The session property: bit 31 set (never 0), 2 bits a weapon.
uint32_t Encode(const std::array<int, kCount>& c) {
  uint32_t v = 0x80000000u;
  for (int i = 0; i < kCount; ++i) v |= uint32_t(std::clamp(c[i], 0, kMaxEach)) << (2 * i);
  return v;
}
std::array<int, kCount> Decode(uint32_t v) {
  std::array<int, kCount> c{};
  for (int i = 0; i < kCount; ++i) c[i] = int(v >> (2 * i) & 3);
  return c;
}

// The host's property for the sessions this game hosts (its choice, or the game's set).
void Publish() {
  std::array<int, kCount> c;
  {
    std::lock_guard lock(g_mutex);
    c = g_custom ? g_count : GameSet();
  }
  svr2011::P2PSetHostProperty(kProperty, Encode(c));
}

void LoadSettings() {
  std::array<int, kCount> c = GameSet();
  const std::string s = rex::cvar::Query<std::string>("extreme_weapons");
  if (!s.empty()) {
    c.fill(0);
    std::stringstream ss(s);
    for (std::string item; std::getline(ss, item, ',');) {
      const size_t colon = item.find(':');
      const int id = std::atoi(item.c_str()), n = colon == std::string::npos ? 1 : std::atoi(item.c_str() + colon + 1);
      for (int i = 0; i < kCount; ++i)
        if (kWeapons[i].id == id) c[i] = std::clamp(n, 0, kMaxEach);
    }
  }
  {
    std::lock_guard lock(g_mutex);
    g_count = c;
    g_custom = !s.empty() && c != GameSet();
  }
  Publish();
}

void SaveSettings() {
  std::string s;
  {
    std::lock_guard lock(g_mutex);
    g_custom = g_count != GameSet();
    if (g_custom)
      for (int i = 0; i < kCount; ++i) s += (s.empty() ? "" : ",") + std::to_string(kWeapons[i].id) + ":" + std::to_string(g_count[i]);
  }
  rex::cvar::SetFlagByName("extreme_weapons", s);
  svr2011::SaveConfigSetting("extreme_weapons", "\"" + s + "\"");
  Publish();
}

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// The game's Extreme Rules nine (sub_82301148 loads them, sub_8227B938 makes them).
constexpr uint32_t kGameNine[] = {87, 15, 60, 89, 11, 88, 5, 63, 90};
constexpr uint32_t kLiveSettings = 0x82E3DE00, kExtremeOption = 56;
constexpr uint32_t kObjects = 0x82DE0220, kObjectSlots = 72, kObjectSlot = 1152;
// sub_8227B938's code (its own calls of the weapon maker sub_821CF178; the
// placed ones go through sub_821D2070 and stay as they are).
constexpr uint32_t kMakerFirst = 0x8227B938, kMakerEnd = 0x8227C0A0;

// The set this match uses, when it isn't the game's (whose: for the log).
bool WantedSet(std::array<int, kCount>& want, const char*& whose) {
  // Online the session's (the host's; Ranked: the game's), offline this game's.
  whose = "this game's";
  const svr2011::P2PLobby lobby = svr2011::P2PMatchLobby();
  if (lobby.in) {
    const auto v = svr2011::P2PSessionProperty(kProperty);
    const bool ranked = svr2011::P2PRanked().value_or(false);
    if (ranked || !v) return false;
    want = Decode(*v), whose = lobby.host ? "this game's (hosting)" : "the host's";
  } else {
    std::lock_guard lock(g_mutex);
    if (!g_custom) return false;
    want = g_count;
  }
  return want != GameSet();
}

// This match's nine (ChooseExtremeWeapons): the chosen weapons first (g_real of them), then the make-up ones.
std::vector<uint32_t> g_nine;
size_t g_real = 0;

// The chosen weapons as nine models (empty: the game's set).
std::vector<uint32_t> NineOf(const std::array<int, kCount>& want, std::string& names, std::string& dropped, size_t* real_n = nullptr) {
  std::vector<uint32_t> real;
  for (int i = 0; i < kCount; ++i)
    for (int k = 0; k < want[i]; ++k) real.push_back(uint32_t(kWeapons[i].id));
  int room = 0;
  for (uint32_t id : kGameNine) room += KbOf(id);
  auto fits = [&] {
    if (real.size() > std::size(kGameNine)) return false;
    int kb = 0, light = KbOf(87);
    for (uint32_t id : real) kb += KbOf(id), light = std::min(light, KbOf(id));
    return kb + int(std::size(kGameNine) - real.size()) * light <= room;
  };
  while (!real.empty() && !fits()) {
    for (const Weapon& w : kWeapons)
      if (uint32_t(w.id) == real.back()) dropped += fmt::format("{}{}", dropped.empty() ? "" : ", ", w.name);
    real.pop_back();
  }
  for (int i = 0; i < kCount; ++i) {
    const int n = int(std::count(real.begin(), real.end(), uint32_t(kWeapons[i].id)));
    if (n) names += fmt::format("{}{} x{}", names.empty() ? "" : ", ", kWeapons[i].name, n);
  }
  if (real_n) *real_n = real.size();
  uint32_t light = 87;  // (the make-up: the lightest chosen one; none chosen - the drink cup)
  for (uint32_t id : real)
    if (KbOf(id) < KbOf(light) || light == 87) light = id;
  if (real.empty()) light = 87;
  while (real.size() < std::size(kGameNine)) real.push_back(light);
  return real;
}

// While sub_8227B938 makes the match's weapons: the chosen ones still to make.
bool g_making = false;
std::vector<uint32_t> g_plan;
int g_mapped = 0, g_skipped = 0;
// The chosen weapons made for this match (for the stock under the ring); empty: the game's.
std::vector<uint32_t> g_made;
// The stock under the ring: *0x82E3C06C -> {s32 count, ?, entries -> s32 model ids (-1: taken)}, a heap object the
// game frees with its entries at the match's end (sub_82B8E9B8). More chosen weapons than its places: a longer
// entries array of ours meanwhile, the game's own put back before that free.
constexpr uint32_t kUnderRing = 0x82E3C06C;
constexpr uint32_t kOurPlaces = 64;
uint32_t g_our_entries = 0;  // (guest: ours, allocated once)
uint32_t g_lent_stock = 0, g_lent_entries = 0, g_lent_count = 0;  // the game's, while ours are in

void GiveBackStock(uint8_t* base) {
  if (!g_lent_stock) return;
  if (Rd32(base + kUnderRing) == g_lent_stock && Rd32(base + g_lent_stock + 8) == g_our_entries) {
    Wr32(base + g_lent_stock, g_lent_count);
    Wr32(base + g_lent_stock + 8, g_lent_entries);
  }
  g_lent_stock = 0;
}

}  // namespace

// The stock under the ring is freed (the match's end): the game's own entries back in it first.
REX_EXTERN(__imp__sub_82B8E9B8);
REX_HOOK_RAW(sub_82B8E9B8) {
  GiveBackStock(base);
  __imp__sub_82B8E9B8(ctx, base);
}

// The weapon maker (sub_821CF178(-1, model, 0, 0) -> the object): while the
// match's weapons are made, each of the game's Extreme Rules nine becomes the
// next chosen weapon (none left: not made).
REX_EXTERN(__imp__sub_821CF178);
REX_HOOK_RAW(sub_821CF178) {
  const uint32_t lr = uint32_t(ctx.lr);
  if (g_making && lr >= kMakerFirst && lr < kMakerEnd &&
      std::find(std::begin(kGameNine), std::end(kGameNine), ctx.r4.u32) != std::end(kGameNine)) {
    if (g_plan.empty()) {
      ++g_skipped;
      ctx.r3.u64 = 0;
      return;
    }
    ctx.r4.u64 = g_plan.front();
    if (size_t(g_mapped) < g_real) g_made.push_back(g_plan.front());  // (the make-up ones stay hidden)
    g_plan.erase(g_plan.begin());
    ++g_mapped;
  }
  __imp__sub_821CF178(ctx, base);
}

namespace svr2011 {

void ExtremeWeaponsMaking(uint8_t* base, bool begin, PPCContext& ctx) {
  if (begin) {
    GiveBackStock(base);  // (a new match: none of ours left in the game's stock)
    g_making = false;
    g_made.clear();
    std::array<int, kCount> want;
    const char* whose;
    if (!base[kLiveSettings + kExtremeOption] || !WantedSet(want, whose)) return;
    if (g_nine.size() != std::size(kGameNine)) return;
    g_plan = g_nine;
    g_made.clear();
    g_mapped = g_skipped = 0;
    g_making = true;
    return;
  }
  (void)ctx;
  if (!g_making) return;
  g_making = false;
  if (g_mapped != int(std::size(kGameNine)) || g_skipped)
    REXLOG_WARN("[svr2011] weapons: the game made {} of its Extreme Rules weapons (nine expected; {} not made)",
                g_mapped + g_skipped, g_skipped);
  REXLOG_INFO("[svr2011] weapons: the match's Extreme Rules weapons made - {} chosen, {} make-up ones (hidden)",
              std::min<size_t>(size_t(g_mapped), g_real), g_mapped > int(g_real) ? g_mapped - int(g_real) : 0);
  g_plan.clear();
}

int ExtremeWeaponsPlacedModel(int i, int model) {
  // (WEAPONS EVERYWHERE's table, ladder, trash can and guitar lying about: the chosen ones, in turn)
  if (!g_making && g_real && (model == 5 || model == 11 || model == 63 || model == 89)) {
    static int turn = 0;
    if (i == 0) turn = 0;
    return int(g_nine[size_t(turn++) % g_real]);
  }
  return model;
}

void ExtremeWeaponsPlaced(uint8_t* base, rex::memory::Memory* memory) {
  {  // (for reports, every match: its weapons and prizes as placed - slot:model@x,y,z; a ladder match's prize hangs high)
    std::string s;
    for (uint32_t i = 0; i < kObjectSlots; ++i) {
      const uint32_t o = Rd32(base + kObjects + i * 4);
      if (!o) continue;
      float p[3];
      for (int k = 0; k < 3; ++k) {
        const uint32_t v = Rd32(base + o + 288 + uint32_t(k) * 4);
        std::memcpy(&p[k], &v, 4);
      }
      s += fmt::format(" {}:{}@{:.0f},{:.0f},{:.0f}", i, Rd32(base + o + 80), p[0], p[1], p[2]);
    }
    REXLOG_INFO("[svr2011] weapons: placed -{}", s);
  }
  if (g_made.empty()) return;
  const uint32_t stock = Rd32(base + kUnderRing);
  if (!stock) return;
  const int32_t n = int32_t(Rd32(base + stock));
  const uint32_t entries = Rd32(base + stock + 8);
  if (n <= 0 || n > 64 || !entries) return;
  // The game's nine in it (by model id) become the chosen ones made, in turn; the chosen ones left over take the
  // places of the game's beyond them (none: -1, as a weapon already taken).
  size_t next = 0;
  int changed = 0;
  std::string was, now;
  for (int32_t i = 0; i < n; ++i) {
    uint8_t* e = base + entries + uint32_t(i) * 4;
    const uint32_t model = Rd32(e);
    was += fmt::format(" {}", int32_t(model));
    if (std::find(std::begin(kGameNine), std::end(kGameNine), model) != std::end(kGameNine)) {
      Wr32(e, next < g_made.size() ? g_made[next++] : 0xFFFFFFFFu);
      ++changed;
    }
    now += fmt::format(" {}", int32_t(Rd32(e)));
  }
  // (the chosen ones left over: our longer array, the game's places first)
  int added = 0;
  if (next < g_made.size() && memory && !g_lent_stock) {
    if (!g_our_entries) g_our_entries = memory->SystemHeapAlloc(kOurPlaces * 4);
    if (g_our_entries) {
      uint32_t m = 0;
      for (int32_t i = 0; i < n && m < kOurPlaces; ++i) Wr32(base + g_our_entries + 4 * m++, Rd32(base + entries + uint32_t(i) * 4));
      for (; next < g_made.size() && m < kOurPlaces; ++next, ++added) {
        Wr32(base + g_our_entries + 4 * m++, g_made[next]);
        now += fmt::format(" {}", g_made[next]);
      }
      g_lent_stock = stock, g_lent_entries = entries, g_lent_count = uint32_t(n);
      Wr32(base + stock + 8, g_our_entries);
      Wr32(base + stock, m);
    }
  }
  REXLOG_INFO("[svr2011] weapons: under the ring ({} places{}):{} ->{}{}", n, added ? fmt::format(" + {}", added) : "", was,
              now, next < g_made.size() ? fmt::format(" ({} chosen ones not in it)", g_made.size() - next) : "");
}

void ChooseExtremeWeapons(uint8_t* base, uint32_t list) {
  g_nine.clear(), g_real = 0;
  if (!list) return;
  std::array<int, kCount> want;
  const char* whose;
  if (!WantedSet(want, whose)) {
    if (P2PMatchLobby().in) REXLOG_INFO("[svr2011] weapons: online - the game's Extreme Rules weapons");
    return;
  }
  // The game's nine, in its order, are a run in the list: the chosen nine in their places.
  uint8_t* l = base + list;
  const uint32_t n = std::min(Rd32(l), kListCount);
  std::vector<uint32_t> ids(n);
  for (uint32_t i = 0; i < n; ++i) ids[i] = Rd32(l + 4 + 4 * i);
  const auto run = std::search(ids.begin(), ids.end(), std::begin(kGameNine), std::end(kGameNine));
  if (run == ids.end()) {
    REXLOG_WARN("[svr2011] weapons: the Extreme Rules weapons not found in the match's list ({} ids) - the game's kept", n);
    return;
  }
  std::string names, dropped;
  const std::vector<uint32_t> nine = NineOf(want, names, dropped, &g_real);
  const uint32_t at = uint32_t(run - ids.begin());
  for (size_t k = 0; k < nine.size(); ++k) Wr32(l + 4 + 4 * (at + uint32_t(k)), nine[k]);
  g_nine = nine;
  if (!dropped.empty())
    REXLOG_WARN("[svr2011] weapons: left out - {} (no room: the match has the game's nine weapons' worth)", dropped);
  REXLOG_INFO("[svr2011] weapons: Extreme Rules weapons, {} choice: {}", whose, names.empty() ? "none" : names);
}

}  // namespace svr2011

// ---------------------------------------------------------------------------
// The page (the BACKGROUNDS page's look).

namespace {

rex::input::InputSystem* g_input = nullptr;
ImFont* g_menu_font = nullptr;
ImFont* g_title_font = nullptr;
std::atomic<bool> g_open_requested{false};
std::atomic<bool> g_open{false};
std::atomic<bool> g_wait_release{false};

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text) {
  dl->AddText(font ? font : ImGui::GetFont(), size, at, colour, text);
}
ImVec2 TextSize(ImFont* font, float size, const char* text) {
  return (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

float Hint(ImDrawList* dl, float x, float cy, float s, const char* button, ImU32 colour, const char* label) {
  const float r = 13 * s;
  dl->AddCircleFilled(ImVec2(x + r, cy), r, colour, 24);
  const float gs = 16 * s;
  const ImVec2 gz = TextSize(g_menu_font, gs, button);
  Text(dl, g_menu_font, gs, ImVec2(x + r - gz.x * 0.5f, cy - gz.y * 0.5f), IM_COL32(255, 255, 255, 255), button);
  const float ls = 18 * s;
  const ImVec2 lz = TextSize(g_menu_font, ls, label);
  Text(dl, g_menu_font, ls, ImVec2(x + r * 2 + 8 * s, cy - lz.y * 0.5f), IM_COL32(230, 230, 235, 255), label);
  return x + r * 2 + 8 * s + lz.x + 26 * s;
}

class WeaponsPage final : public rex::ui::ImGuiDialog {
 public:
  explicit WeaponsPage(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  uint16_t PadButtons();
  void Change(int i, int by);
  void GameSetBack();

  int sel_ = 0, top_ = 0;
  uint16_t prev_buttons_ = 0;
  bool wait_release_ = false;
  Clock::time_point repeat_at_{}, opened_at_{};
};

uint16_t WeaponsPage::PadButtons() {
  using namespace rex::input;
  X_INPUT_STATE s = {};
  if (!g_input || !g_input->HeldState(0, &s)) return 0;
  uint16_t b = s.gamepad.buttons;
  constexpr int kDead = 16000;
  if (s.gamepad.thumb_ly > kDead) b |= X_INPUT_GAMEPAD_DPAD_UP;
  if (s.gamepad.thumb_ly < -kDead) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (s.gamepad.thumb_lx > kDead) b |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (s.gamepad.thumb_lx < -kDead) b |= X_INPUT_GAMEPAD_DPAD_LEFT;
  return b;
}

// One more (by +1, wrapping 3 -> 0 for A) or one less.
void WeaponsPage::Change(int i, int by) {
  {
    std::lock_guard lock(g_mutex);
    int& c = g_count[size_t(i)];
    if (by > 0) {
      const int next = c >= kMaxEach ? 0 : c + 1;
      if (next > c && Total(g_count) >= kMaxTotal) return;
      c = next;
    } else {
      c = std::max(0, c - 1);
    }
  }
  SaveSettings();
}

void WeaponsPage::GameSetBack() {
  {
    std::lock_guard lock(g_mutex);
    g_count = GameSet();
  }
  SaveSettings();
}

void WeaponsPage::OnDraw(ImGuiIO& io) {
  using namespace rex::input;
  constexpr int kVisible = 7;
  if (g_open_requested.exchange(false)) {
    sel_ = top_ = 0;
    wait_release_ = true;  // (the A that opened the page is still down)
    opened_at_ = Clock::now();
    g_open = true;
    REXLOG_INFO("[svr2011] weapons: page open");
  }
  if (!g_open) {
    if (g_wait_release && (!g_input || PadButtons() == 0)) g_wait_release = false;
    return;
  }

  const uint16_t buttons = PadButtons();
  if (wait_release_) {
    if (buttons == 0 && Clock::now() - opened_at_ > std::chrono::milliseconds(150)) wait_release_ = false;
    prev_buttons_ = buttons;
  }
  const uint16_t pressed = wait_release_ ? 0 : uint16_t(buttons & ~prev_buttons_);
  constexpr uint16_t kRepeat = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN;
  uint16_t act = pressed;
  const auto now = Clock::now();
  if (pressed & kRepeat) {
    repeat_at_ = now + std::chrono::milliseconds(400);
  } else if (!wait_release_ && (buttons & kRepeat) && now >= repeat_at_) {
    act |= buttons & kRepeat;
    repeat_at_ = now + std::chrono::milliseconds(110);
  }
  if (!wait_release_) prev_buttons_ = buttons;
  auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
  if ((act & X_INPUT_GAMEPAD_DPAD_UP) || key(ImGuiKey_UpArrow)) sel_ = (sel_ + kCount - 1) % kCount;
  if ((act & X_INPUT_GAMEPAD_DPAD_DOWN) || key(ImGuiKey_DownArrow)) sel_ = (sel_ + 1) % kCount;
  sel_ = std::clamp(sel_, 0, kCount - 1);
  bool more = (pressed & (X_INPUT_GAMEPAD_A)) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
              ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false);
  const bool right = (pressed & X_INPUT_GAMEPAD_DPAD_RIGHT) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
  bool less = (pressed & X_INPUT_GAMEPAD_DPAD_LEFT) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false);
  bool game = (pressed & X_INPUT_GAMEPAD_Y) || ImGui::IsKeyPressed(ImGuiKey_Y, false);
  bool back = (pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_BACK)) ||
              ImGui::IsKeyPressed(ImGuiKey_Escape, false);

  float fw = io.DisplaySize.x, fh = fw * 9.0f / 16.0f;
  if (fh > io.DisplaySize.y) fh = io.DisplaySize.y, fw = fh * 16.0f / 9.0f;
  const float ox = (io.DisplaySize.x - fw) * 0.5f, oy = (io.DisplaySize.y - fh) * 0.5f;
  const float s = fh / 720.0f;
  auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };
  auto inside = [&](ImVec2 a, ImVec2 b) {
    const ImVec2 m = io.MousePos;
    return m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
  };
  const bool tap = !wait_release_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const ImU32 kWhite = IM_COL32(255, 255, 255, 255), kGrey = IM_COL32(170, 170, 178, 255),
              kDim = IM_COL32(120, 120, 128, 255), kGreen = IM_COL32(90, 200, 90, 255),
              kGold = IM_COL32(255, 200, 60, 255);
  const float row_h = 62, gap = 6, list_top = 134;
  if (tap) {
    for (int r = 0; r < kVisible && top_ + r < kCount; ++r) {
      const float y = list_top + r * (row_h + gap);
      if (inside(P(80, y), P(744, y + row_h))) {
        sel_ = top_ + r;
        more = true;
      }
    }
  }
  if (more || right) Change(sel_, +1);
  if (less) Change(sel_, -1);
  if (game) GameSetBack();
  std::array<int, kCount> count;
  bool custom;
  {
    std::lock_guard lock(g_mutex);
    count = g_count;
    custom = g_custom;
  }
  if (sel_ < top_) top_ = sel_;
  if (sel_ >= top_ + kVisible) top_ = sel_ - kVisible + 1;
  top_ = std::clamp(top_, 0, std::max(0, kCount - kVisible));
  const int total = Total(count);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
  dl->AddRectFilled(P(60, 40), P(1220, 684), IM_COL32(8, 8, 12, 235), 10 * s);
  dl->AddRectFilledMultiColor(P(62, 42), P(1218, 260), IM_COL32(40, 40, 48, 120), IM_COL32(40, 40, 48, 120),
                              IM_COL32(8, 8, 12, 0), IM_COL32(8, 8, 12, 0));
  dl->AddRect(P(60, 40), P(1220, 684), IM_COL32(235, 235, 240, 255), 10 * s, 0, 2.5f * s);
  dl->AddRectFilled(P(66, 46), P(1214, 80), IM_COL32(24, 24, 30, 255), 6 * s);
  for (float x = 76; x < 1206; x += 7)
    for (float y = 52; y < 76; y += 7) dl->AddCircleFilled(P(x, y), 1.2f * s, IM_COL32(70, 70, 80, 255));
  dl->AddQuadFilled(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(12, 12, 16, 255));
  dl->AddQuad(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(235, 235, 240, 255), 2 * s);
  {
    const char* title = "OPTIONS";
    const float ts = 34 * s;
    const ImVec2 sz = TextSize(g_title_font, ts, title);
    Text(dl, g_title_font, ts, ImVec2(P(378, 0).x - sz.x * 0.5f, P(0, 63).y - sz.y * 0.5f), kWhite, title);
    const float ps = 22 * s;
    const ImVec2 psz = TextSize(g_menu_font, ps, "WEAPONS");
    Text(dl, g_menu_font, ps, ImVec2(P(634, 0).x, P(0, 63).y - psz.y * 0.5f), kWhite, "WEAPONS");
  }
  {
    const float ss = 21 * s, cy = P(0, 108).y;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d WEAPONS IN EXTREME RULES MATCHES", total);
    Text(dl, g_menu_font, ss, ImVec2(P(84, 0).x, cy - TextSize(g_menu_font, ss, buf).y * 0.5f), kWhite, buf);
    const char* note = custom ? "YOUR SET" : "THE GAME'S SET";
    const ImVec2 nz = TextSize(g_menu_font, 17 * s, note);
    Text(dl, g_menu_font, 17 * s, ImVec2(P(1196, 0).x - nz.x, cy - nz.y * 0.5f), custom ? kGold : kGrey, note);
  }
  for (int r = 0; r < kVisible && top_ + r < kCount; ++r) {
    const int i = top_ + r;
    const float y = list_top + r * (row_h + gap);
    const bool sel = i == sel_;
    const ImVec2 a = P(80, y), b = P(744, y + row_h);
    if (sel) {
      dl->AddRectFilledMultiColor(a, b, IM_COL32(150, 18, 20, 255), IM_COL32(95, 8, 10, 255), IM_COL32(95, 8, 10, 255),
                                  IM_COL32(150, 18, 20, 255));
      dl->AddRect(a, b, IM_COL32(230, 60, 60, 255), 3 * s, 0, 1.5f * s);
    } else {
      dl->AddRectFilled(a, b, IM_COL32(34, 34, 40, 235), 3 * s);
    }
    const float ns = 21 * s, ds = 16 * s;
    Text(dl, g_menu_font, ns, P(100, y + 7), count[i] || sel ? kWhite : kGrey, kWeapons[i].name);
    char sub[64];
    std::snprintf(sub, sizeof sub, "%s", kWeapons[i].game ? "In the game's Extreme Rules set" : "Not in the game's set");
    Text(dl, g_menu_font, ds, P(100, y + 35), sel ? IM_COL32(235, 205, 205, 255) : kDim, sub);
    for (int k = 0; k < kMaxEach; ++k) {  // (the count: three pips)
      const ImVec2 c = P(640 + k * 30, y + row_h * 0.5f);
      if (k < count[i]) dl->AddCircleFilled(c, 10 * s, kGreen, 20);
      else dl->AddCircle(c, 10 * s, IM_COL32(110, 110, 118, 255), 20, 2 * s);
    }
    if (!count[i]) {
      const ImVec2 oz = TextSize(g_menu_font, ds, "NONE");
      Text(dl, g_menu_font, ds, ImVec2(P(616, 0).x - oz.x, P(0, y + 22).y), kDim, "NONE");
    }
  }
  {
    const float tp = list_top, h = kVisible * (row_h + gap) - gap;
    dl->AddRectFilled(P(750, tp), P(754, tp + h), IM_COL32(40, 40, 48, 255), 2 * s);
    const float bh = h * std::min(1.0f, float(kVisible) / float(kCount));
    const float by = kCount > kVisible ? tp + (h - bh) * top_ / float(kCount - kVisible) : tp;
    dl->AddRectFilled(P(750, by), P(754, by + bh), IM_COL32(200, 200, 208, 255), 2 * s);
  }
  const ImVec2 da = P(766, list_top), db = P(1200, list_top + kVisible * (row_h + gap) - gap);
  dl->AddRectFilled(da, db, IM_COL32(24, 24, 30, 245), 6 * s);
  dl->AddRect(da, db, IM_COL32(70, 70, 80, 255), 6 * s, 0, 1.0f * s);
  {
    auto line = [&](float y, float size, const char* text, ImU32 colour) { Text(dl, g_menu_font, size, P(790, y), colour, text); };
    line(list_top + 14, 16 * s, "EXTREME RULES", kDim);
    line(list_top + 52, 24 * s, kWeapons[sel_].name, kWhite);
    char buf[64];
    std::snprintf(buf, sizeof buf, count[sel_] ? "%d IN EACH MATCH" : "NOT IN THE MATCHES", count[sel_]);
    line(list_top + 84, 17 * s, buf, count[sel_] ? kGreen : kGold);
    line(list_top + 140, 16 * s, "The weapons of Extreme Rules matches:", kGrey);
    line(list_top + 164, 16 * s, "under the ring, and lying about in", kGrey);
    line(list_top + 188, 16 * s, "WEAPONS EVERYWHERE. Chairs and the", kGrey);
    line(list_top + 212, 16 * s, "steps are there in every match.", kGrey);
    line(list_top + 268, 16 * s, "Online the host's set is used;", kDim);
    line(list_top + 292, 16 * s, "Ranked matches keep the game's.", kDim);
    std::snprintf(buf, sizeof buf, "UP TO %d WEAPONS IN ALL", kMaxTotal);
    line(list_top + 340, 16 * s, buf, kDim);
    // (the room: the chosen ones share what the game's nine take - heavier picks leave the last ones out)
    std::string names, dropped;
    NineOf(count, names, dropped);
    if (!dropped.empty()) {
      const size_t n = size_t(std::count(dropped.begin(), dropped.end(), ',')) + 1;
      std::snprintf(buf, sizeof buf, "NO ROOM FOR %zu: %s", n, dropped.substr(0, dropped.find(',')).c_str());
      line(list_top + 372, 16 * s, buf, kGold);
      line(list_top + 396, 16 * s, "The weapons share the room of the", kDim);
      line(list_top + 420, 16 * s, "game's nine; heavy ones take more.", kDim);
    } else {
      line(list_top + 372, 16 * s, "They take no more room than the", kDim);
      line(list_top + 396, 16 * s, "game's nine: with big custom arenas", kDim);
      line(list_top + 420, 16 * s, "keep the set light.", kDim);
    }
  }
  {
    const float cy = P(0, 660).y;
    float x = P(84, 0).x;
    const float x_b = x;
    x = Hint(dl, x, cy, s, "B", IM_COL32(200, 40, 40, 255), "BACK");
    const float x_a = x;
    x = Hint(dl, x, cy, s, "A", IM_COL32(60, 160, 60, 255), "ONE MORE");
    const float x_l = x;
    x = Hint(dl, x, cy, s, "<", IM_COL32(90, 90, 100, 255), "ONE LESS");
    const float x_y = x;
    x = Hint(dl, x, cy, s, "Y", IM_COL32(215, 170, 20, 255), "THE GAME'S SET");
    if (tap && io.MousePos.y > cy - 16 * s && io.MousePos.y < cy + 16 * s) {
      if (io.MousePos.x >= x_b && io.MousePos.x < x_a) back = true;
      else if (io.MousePos.x >= x_a && io.MousePos.x < x_l) Change(sel_, +1);
      else if (io.MousePos.x >= x_l && io.MousePos.x < x_y) Change(sel_, -1);
      else if (io.MousePos.x >= x_y && io.MousePos.x < x) GameSetBack();
    }
  }
  if (back) {
    std::string s2;
    for (int i = 0; i < kCount; ++i)
      if (count[i]) s2 += fmt::format("{}{} x{}", s2.empty() ? "" : ", ", kWeapons[i].name, count[i]);
    REXLOG_INFO("[svr2011] weapons: page closed - {}: {}", custom ? "your set" : "the game's set", s2.empty() ? "none" : s2);
    g_wait_release = true;
    g_open = false;
  }
}

}  // namespace

namespace svr2011 {

void InstallWeaponsPage(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input) {
  g_input = input;
  LoadSettings();
  new WeaponsPage(drawer);  // lives for the whole run
}

void SetWeaponsPageFonts(ImFont* menu, ImFont* title) {
  g_menu_font = menu;
  g_title_font = title;
}

void OpenWeaponsPage() { g_open_requested = true; }

bool WeaponsPageHoldsInput() { return g_open.load() || g_wait_release.load(); }

}  // namespace svr2011
