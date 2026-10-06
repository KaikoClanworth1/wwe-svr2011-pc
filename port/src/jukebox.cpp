// WWE SmackDown vs. Raw 2011 - MY WWE -> JUKEBOX (see jukebox.h).
//
// The menu music: Play_Menu_Music (System.pck, bank E611314A) plays random
// container 37CCA250 - 19 children, one song each (sound/Music.pck streams),
// continuous, avoid-repeat 17, all weights 50. Once its bank is loaded, the
// container's playlist is an array of 19 {u32 child id, u8 weight, 3 pad}
// items (guest heap, physical view). The container picks an item, then plays
// the child with that item's id - so a song that is off has its item's id
// replaced by a song that is on, and the container only finds songs that are
// on (weights and the avoid-repeat state are left alone). The array is found
// once by a search of the physical heap (19 items, each one of the 19 ids,
// each weight 50), and checked before each use.
//
// What plays now: each song's sound node (vtable 820B14F0, id at +0xC) has a
// reference count at +4, one higher while it plays.
//
// Stopping, skipping and previewing post the game's own events from its
// world update (JukeboxUpdate): Stop_Menu_Music, then Play_Menu_Music a
// quarter of a second later on the game object the game last played the menu
// music on. A preview gives every item the chosen song for that Play, then
// the playlist is put back; a song that is off which still comes up (after a
// preview of it, with every song off) is stopped when it starts.
//
// Research: scratchpad pck.py / songs.py (the banks), jukebox tests jbx1-10.

#include "jukebox.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "graphics_page.h"

REXCVAR_DEFINE_STRING(jukebox_off, "", "UI",
                      "MY WWE -> JUKEBOX: the menu songs that are off (their numbers 1-19, comma-separated)");

namespace {

using Clock = std::chrono::steady_clock;

struct Track {
  uint32_t child;  // the container's child (the playlist item's id)
  uint32_t sound;  // its sound node
  const char* name;
  bool theme;      // an original entrance theme (else a menu theme)
  int seconds;
};
// In the playlist's order. Names: the songs have none in the game's data; the
// entrance themes are numbered as soundtrack rips number them.
constexpr int kTracks = 19;
const Track kTrack[kTracks] = {
    {0x0713FF93, 0x0927E21F, "ORIGINAL THEME 1", true, 191},  {0x160F5963, 0x283A4E50, "ORIGINAL THEME 2", true, 157},
    {0x198D1CB0, 0x0A7B33DC, "ORIGINAL THEME 3", true, 124},  {0x269E2C41, 0x23865753, "ORIGINAL THEME 4", true, 61},
    {0x0DCECCA9, 0x1F7F1E54, "ORIGINAL THEME 5", true, 150},  {0x3C815A28, 0x3977E15A, "ORIGINAL THEME 6", true, 145},
    {0x12E19B6E, 0x0E8ED2D6, "ORIGINAL THEME 7", true, 123},  {0x31476501, 0x19910944, "ORIGINAL THEME 8", true, 74},
    {0x2180AAEE, 0x1F399151, "ORIGINAL THEME 9", true, 295},  {0x044B76C8, 0x2214418E, "ORIGINAL THEME 10", true, 234},
    {0x24627889, 0x328B45A9, "MENU THEME 1", false, 294},     {0x2D76CFF6, 0x0547BAB3, "MENU THEME 2", false, 288},
    {0x29EF4C90, 0x044956DE, "MENU THEME 3", false, 234},     {0x3D428BE7, 0x272C4CBF, "MENU THEME 4", false, 294},
    {0x0674D3B8, 0x0E87BDAA, "MENU THEME 5", false, 295},     {0x118F4EA7, 0x3661FF4B, "MENU THEME 6", false, 188},
    {0x1E136E96, 0x1BA08EA4, "MENU THEME 7", false, 295},     {0x0BE7EF7C, 0x16609F20, "MENU THEME 8", false, 188},
    {0x2F173438, 0x27477986, "MENU THEME 9", false, 294},
};
constexpr uint32_t kSoundVtable = 0x820B14F0;
constexpr uint8_t kWeight = 50;

rex::memory::Memory* g_memory = nullptr;
std::mutex g_mutex;
std::array<bool, kTracks> g_on;  // under g_mutex
uint32_t g_list = 0;             // the playlist's items (guest address; 0: not found yet)
std::array<uint32_t, kTracks> g_sound_node{};
std::array<uint32_t, kTracks> g_sound_idle{};  // each node's count while it doesn't play
std::atomic<bool> g_searched{false};

// The events the page asks for (JukeboxUpdate posts them; frames to wait, -1: none).
uint32_t g_event_names = 0;           // guest "Play_Menu_Music\0Stop_Menu_Music\0"
std::atomic<uint32_t> g_music_object{0};
std::atomic<bool> g_have_object{false};
int g_stop_in = -1, g_play_in = -1, g_restore_in = -1;  // under g_mutex
int g_preview = -1;   // the song every item plays for a preview (under g_mutex)
int g_previewed = -1; // the song last previewed: left to play out even if off
int g_quiet = 0;      // frames since the last event the jukebox posted
std::atomic<int> g_now{-1};  // the song that started last and still plays (JukeboxUpdate)
std::array<uint32_t, kTracks> g_last_count{};

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void SetBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
uint8_t* Host(uint32_t guest) { return g_memory->TranslateVirtual<uint8_t*>(guest); }

int TrackOfChild(uint32_t id) {
  for (int i = 0; i < kTracks; ++i)
    if (kTrack[i].child == id) return i;
  return -1;
}

// 19 items of known ids with the songs' weight.
bool IsPlaylist(const uint8_t* p) {
  for (int i = 0; i < kTracks; ++i)
    if (p[8 * i + 4] != kWeight || TrackOfChild(Be32(p + 8 * i)) < 0) return false;
  return true;
}

// Calls fn(start, size) for each committed, readable region of guest memory in [lo, hi).
template <class F>
void ForEachRegion(uint64_t lo, uint64_t hi, F fn) {
  for (uint64_t at = lo; at < hi;) {
    auto* heap = g_memory->LookupHeap(uint32_t(at));
    rex::memory::HeapAllocationInfo info = {};
    const bool ok = heap && heap->QueryRegionInfo(uint32_t(at) & ~0xFFFu, &info) && info.region_size &&
                    (info.state & rex::memory::kMemoryAllocationCommit) &&
                    (info.protect & rex::memory::kMemoryProtectRead);
    if (!ok) {
      at = (at & ~0xFFFull) + (info.region_size ? info.region_size : 0x1000);
      continue;
    }
    const uint64_t end = std::min<uint64_t>(uint64_t(info.base_address) + info.region_size, hi);
    fn(uint32_t(at), uint32_t(end - at));
    at = end;
  }
}

// Finds the playlist and the sound nodes (the physical heap's 4 KB view,
// where the sound engine's memory is). Once the bank is loaded.
void Search() {
  const auto t0 = Clock::now();
  uint32_t list = 0;
  std::array<uint32_t, kTracks> nodes{};
  ForEachRegion(0xE0000000ull, 0x100000000ull, [&](uint32_t start, uint32_t size) {
    const uint8_t* p = Host(start);
    for (uint32_t o = 16; o + 8 * kTracks <= size; o += 4) {
      const uint8_t* q = p + o;
      if (q[4] == kWeight && q[12] == kWeight && q[20] == kWeight && IsPlaylist(q)) list = start + o;
      if (Be32(q - 12) == kSoundVtable)
        for (int i = 0; i < kTracks; ++i)
          if (Be32(q) == kTrack[i].sound) nodes[i] = start + o - 12;
    }
  });
  std::lock_guard lock(g_mutex);
  g_list = list;
  g_sound_node = nodes;
  for (int i = 0; i < kTracks; ++i) g_sound_idle[i] = nodes[i] ? Be32(Host(nodes[i]) + 4) : 0;
  const int found = int(std::count_if(nodes.begin(), nodes.end(), [](uint32_t n) { return n != 0; }));
  REXLOG_INFO("[svr2011] jukebox: menu playlist {:08X}, {} of {} songs' sound nodes ({} ms)", list, found, kTracks,
              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}

void LoadSettings() {
  std::array<bool, kTracks> on;
  on.fill(true);
  std::stringstream ss(rex::cvar::Query<std::string>("jukebox_off"));
  for (std::string n; std::getline(ss, n, ',');) {
    const int i = std::atoi(n.c_str()) - 1;
    if (i >= 0 && i < kTracks) on[i] = false;
  }
  std::lock_guard lock(g_mutex);
  g_on = on;
}

void SaveSettings() {
  std::string off;
  {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kTracks; ++i)
      if (!g_on[i]) off += (off.empty() ? "" : ",") + std::to_string(i + 1);
  }
  rex::cvar::SetFlagByName("jukebox_off", off);
  svr2011::SaveConfigSetting("jukebox_off", "\"" + off + "\"");
}

// The playlist for the songs that are on: each one keeps its own item, the
// others' items go to them in a shuffled round. False: every song is off.
bool Apply() {
  std::lock_guard lock(g_mutex);
  std::vector<int> on;
  if (g_preview >= 0) {
    on.push_back(g_preview);
  } else {
    for (int i = 0; i < kTracks; ++i)
      if (g_on[i]) on.push_back(i);
  }
  if (!g_list) return !on.empty();
  uint8_t* p = Host(g_list);
  if (!IsPlaylist(p)) {  // (the bank was unloaded: search again at the next song)
    g_list = 0;
    g_searched = false;
    return !on.empty();
  }
  if (on.empty()) {  // (each item its own song: one that comes up is stopped, JukeboxUpdate)
    for (int i = 0; i < kTracks; ++i) SetBe32(p + 8 * i, kTrack[i].child);
    return false;
  }
  static std::mt19937 rng{std::random_device{}()};
  std::vector<int> fill = on;
  std::shuffle(fill.begin(), fill.end(), rng);
  size_t next = 0;
  for (int i = 0; i < kTracks; ++i)
    SetBe32(p + 8 * i, g_preview < 0 && g_on[i] ? kTrack[i].child : kTrack[fill[next++ % fill.size()]].child);
  return true;
}

bool AnyOn() {
  std::lock_guard lock(g_mutex);
  return std::find(g_on.begin(), g_on.end(), true) != g_on.end();
}

// The song playing now (-1: none): the one that started last while it plays
// (a stopped one fades out a moment longer). Once a frame (JukeboxUpdate).
void TrackNowPlaying() {
  std::lock_guard lock(g_mutex);
  if (!g_list) {
    g_now = -1;
    return;
  }
  int now = g_now, any = -1;
  for (int i = 0; i < kTracks; ++i) {
    const uint32_t c = g_sound_node[i] ? Be32(Host(g_sound_node[i]) + 4) : 0;
    if (c > g_sound_idle[i]) {
      if (any < 0) any = i;
      if (c > g_last_count[i]) now = i;  // (started)
    }
    g_last_count[i] = c;
  }
  if (now >= 0 && (!g_sound_node[now] || g_last_count[now] <= g_sound_idle[now])) now = any;  // (it ended)
  if (now != g_now) REXLOG_INFO("[svr2011] jukebox: now playing {}", now >= 0 ? kTrack[now].name : "nothing");
  g_now = now;
}
int NowPlaying() { return g_now; }

// The page's requests (any thread).
void RequestSkip() {  // (the song stopped; another starts if any is on)
  const bool on = AnyOn();
  std::lock_guard lock(g_mutex);
  g_stop_in = 0;
  g_play_in = on ? 15 : -1;
}
void RequestPlay() {  // (the menus were silent)
  std::lock_guard lock(g_mutex);
  if (g_play_in < 0) g_play_in = 0;
}
void RequestPreview(int i) {
  std::lock_guard lock(g_mutex);
  g_preview = g_previewed = i;
  g_stop_in = 0;
  g_play_in = 15;
  g_restore_in = -1;
}

// Posts one of the menu music's events as the game does (sub_82BEC030(name,
// object, flags, callback, cookie)), through the port's hook of it.
void Post(PPCContext& ctx, uint8_t* base, bool play) {
  if (!g_event_names || !g_have_object) return;
  const auto saved = ctx;
  ctx.r3.u64 = g_event_names + (play ? 0 : 16);
  ctx.r4.u64 = g_music_object.load();
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;
  ctx.r7.u64 = 0;
  sub_82BEC030(ctx, base);
  ctx = saved;
  REXLOG_INFO("[svr2011] jukebox: {}", play ? "Play_Menu_Music" : "Stop_Menu_Music");
}

}  // namespace

namespace svr2011 {

void InstallJukebox(rex::memory::Memory* memory) {
  g_memory = memory;
  LoadSettings();
  static const char kNames[32] = "Play_Menu_Music\0Stop_Menu_Music";
  if ((g_event_names = memory->SystemHeapAlloc(32)) != 0)
    std::memcpy(memory->TranslateVirtual<char*>(g_event_names), kNames, 32);
}

bool JukeboxEvent(uint8_t* /*base*/, const char* e, uint32_t object) {
  if (!g_memory || std::strcmp(e, "Play_Menu_Music") != 0) return false;
  g_music_object = object;
  g_have_object = true;
  if (!g_searched.exchange(true)) Search();
  if (Apply()) return false;
  static int logged = 0;
  if (logged++ < 3) REXLOG_INFO("[svr2011] jukebox: every menu song is off - no menu music");
  return true;
}

void JukeboxUpdate(PPCContext& ctx, uint8_t* base) {
  if (!g_memory) return;
  bool stop = false, play = false, restore = false;
  {
    std::lock_guard lock(g_mutex);
    if (g_stop_in >= 0 && g_stop_in-- == 0) stop = true;
    if (g_play_in >= 0 && g_play_in-- == 0) play = true;
    if (g_restore_in >= 0 && g_restore_in-- == 0) restore = true;
  }
  TrackNowPlaying();
  ++g_quiet;
  if (stop || play) g_quiet = 0;
  if (stop) Post(ctx, base, false);
  if (play) {
    Post(ctx, base, true);  // (the hook applies the playlist: the preview's, or the songs that are on)
    std::lock_guard lock(g_mutex);
    if (g_preview >= 0) g_restore_in = 60;  // (once the container has chosen)
  }
  if (restore) {
    {
      std::lock_guard lock(g_mutex);
      g_preview = -1;
    }
    Apply();
  }
  // A song that is off coming up (the container's next one after a preview,
  // or with every song off): stopped, another started if any is on.
  static int check = 0;
  if (++check >= 30) {
    check = 0;
    const int now = NowPlaying();
    bool off = false, idle = false;
    {
      std::lock_guard lock(g_mutex);
      off = now >= 0 && !g_on[now] && now != g_previewed && g_preview < 0;
      idle = g_stop_in < 0 && g_play_in < 0 && g_restore_in < 0;
      if (now >= 0 && now != g_previewed) g_previewed = -1;
    }
    if (off && idle && g_quiet > 180) RequestSkip();  // (3 s after the last one: the stopped song fades)
  }
}

}  // namespace svr2011

// ---------------------------------------------------------------------------
// The page.

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

// A controller button glyph (a coloured disc with its letter) and a label.
float Hint(ImDrawList* dl, float x, float cy, float s, const char* button, ImU32 colour, const char* label) {
  const float r = 13 * s;
  const bool bumper = button[1] != 0;  // LB / RB: a rounded bar
  const float bw = bumper ? 22 * s : r;
  if (bumper) {
    dl->AddRectFilled(ImVec2(x, cy - 10 * s), ImVec2(x + bw * 2, cy + 10 * s), colour, 5 * s);
  } else {
    dl->AddCircleFilled(ImVec2(x + r, cy), r, colour, 24);
  }
  const float gs = 16 * s;
  const ImVec2 gz = TextSize(g_menu_font, gs, button);
  Text(dl, g_menu_font, gs, ImVec2(x + bw - gz.x * 0.5f, cy - gz.y * 0.5f), IM_COL32(255, 255, 255, 255), button);
  const float ls = 18 * s;
  const ImVec2 lz = TextSize(g_menu_font, ls, label);
  Text(dl, g_menu_font, ls, ImVec2(x + bw * 2 + 8 * s, cy - lz.y * 0.5f), IM_COL32(230, 230, 235, 255), label);
  return x + bw * 2 + 8 * s + lz.x + 26 * s;
}

std::string Length(int seconds) {
  char b[16];
  std::snprintf(b, sizeof b, "%d:%02d", seconds / 60, seconds % 60);
  return b;
}

// Bars that move with the music (`t` seconds; `live` false: flat).
void Equalizer(ImDrawList* dl, ImVec2 a, ImVec2 b, int bars, float t, bool live, ImU32 lo, ImU32 hi) {
  const float w = (b.x - a.x) / bars, h = b.y - a.y;
  for (int i = 0; i < bars; ++i) {
    float v = 0.08f;
    if (live) {
      v = 0.5f + 0.25f * std::sin(t * (3.1f + i * 0.73f) + i * 1.7f) + 0.2f * std::sin(t * (7.3f + i * 0.41f) + i);
      v = std::clamp(v, 0.08f, 1.0f);
    }
    const float x = a.x + i * w;
    dl->AddRectFilledMultiColor(ImVec2(x + w * 0.15f, b.y - h * v), ImVec2(x + w * 0.85f, b.y), hi, hi, lo, lo);
  }
}

class JukeboxPage final : public rex::ui::ImGuiDialog {
 public:
  explicit JukeboxPage(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  uint16_t PadButtons();
  void Set(int i, bool on);

  int sel_ = 0, top_ = 0;
  uint16_t prev_buttons_ = 0;
  bool wait_release_ = false;
  Clock::time_point repeat_at_{}, opened_at_{};
};

uint16_t JukeboxPage::PadButtons() {
  using namespace rex::input;
  X_INPUT_STATE s = {};
  if (!g_input || !g_input->HeldState(0, &s)) return 0;
  uint16_t b = s.gamepad.buttons;
  const int lx = s.gamepad.thumb_lx, ly = s.gamepad.thumb_ly;
  constexpr int kDead = 16000;
  if (ly > kDead) b |= X_INPUT_GAMEPAD_DPAD_UP;
  if (ly < -kDead) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (lx < -kDead) b |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (lx > kDead) b |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  return b;
}

// i < 0: every song. The song playing turned off stops (another starts); a
// song turned on while the menus were silent starts.
void JukeboxPage::Set(int i, bool on) {
  const int playing = NowPlaying();
  const bool was_on = AnyOn();
  {
    std::lock_guard lock(g_mutex);
    for (int k = 0; k < kTracks; ++k)
      if (i < 0 || k == i) g_on[k] = on;
    if (playing >= 0 && g_previewed == playing && !on && (i < 0 || i == playing)) g_previewed = -1;
  }
  Apply();
  SaveSettings();
  if (!on && playing >= 0 && (i < 0 || i == playing))
    RequestSkip();
  else if (on && !was_on)
    RequestPlay();
}

void JukeboxPage::OnDraw(ImGuiIO& io) {
  using namespace rex::input;
  constexpr int kVisible = 7;
  if (g_open_requested.exchange(false)) {
    sel_ = top_ = 0;
    wait_release_ = true;  // (the A that opened the page is still down)
    opened_at_ = Clock::now();
    g_open = true;
  }
  if (!g_open) {
    if (g_wait_release && (!g_input || PadButtons() == 0)) g_wait_release = false;
    return;
  }

  // Input: controller (edges, auto-repeat for held directions) and keys.
  const uint16_t buttons = PadButtons();
  if (wait_release_) {
    if (buttons == 0 && Clock::now() - opened_at_ > std::chrono::milliseconds(150)) wait_release_ = false;
    prev_buttons_ = buttons;
  }
  const uint16_t pressed = wait_release_ ? 0 : uint16_t(buttons & ~prev_buttons_);
  constexpr uint16_t kRepeat = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_LEFT_SHOULDER |
                               X_INPUT_GAMEPAD_RIGHT_SHOULDER;
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
  int move = 0;
  if ((act & X_INPUT_GAMEPAD_DPAD_UP) || key(ImGuiKey_UpArrow)) move = -1;
  if ((act & X_INPUT_GAMEPAD_DPAD_DOWN) || key(ImGuiKey_DownArrow)) move = 1;
  if ((act & X_INPUT_GAMEPAD_LEFT_SHOULDER) || key(ImGuiKey_PageUp)) move = -kVisible;
  if ((act & X_INPUT_GAMEPAD_RIGHT_SHOULDER) || key(ImGuiKey_PageDown)) move = kVisible;
  if (move) {
    sel_ = std::abs(move) == 1 ? (sel_ + move + kTracks) % kTracks : std::clamp(sel_ + move, 0, kTracks - 1);
  }
  std::array<bool, kTracks> on;
  {
    std::lock_guard lock(g_mutex);
    on = g_on;
  }
  if ((pressed & X_INPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
    Set(sel_, !on[sel_]);
  }
  if ((pressed & X_INPUT_GAMEPAD_X) || ImGui::IsKeyPressed(ImGuiKey_P, false)) RequestPreview(sel_);
  if (pressed & X_INPUT_GAMEPAD_Y) Set(-1, std::find(on.begin(), on.end(), false) != on.end());  // (all on, or all off)
  {
    std::lock_guard lock(g_mutex);
    on = g_on;
  }
  const bool back = (pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_START | X_INPUT_GAMEPAD_BACK)) ||
                    ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  if (sel_ < top_) top_ = sel_;
  if (sel_ >= top_ + kVisible) top_ = sel_ - kVisible + 1;
  top_ = std::clamp(top_, 0, kTracks - kVisible);
  const int playing = NowPlaying();
  const float t = float(std::fmod(std::chrono::duration<double>(now.time_since_epoch()).count(), 3600.0));

  // Layout in the game's 1280 x 720 frame, letterboxed into the window.
  float fw = io.DisplaySize.x, fh = fw * 9.0f / 16.0f;
  if (fh > io.DisplaySize.y) fh = io.DisplaySize.y, fw = fh * 16.0f / 9.0f;
  const float ox = (io.DisplaySize.x - fw) * 0.5f, oy = (io.DisplaySize.y - fh) * 0.5f;
  const float s = fh / 720.0f;
  auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };
  const ImU32 kWhite = IM_COL32(255, 255, 255, 255), kGrey = IM_COL32(170, 170, 178, 255),
              kDim = IM_COL32(120, 120, 128, 255), kGreen = IM_COL32(90, 200, 90, 255),
              kGold = IM_COL32(255, 200, 60, 255);

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
  // Panel: dark glass with a thin white rim, as the game's.
  dl->AddRectFilled(P(60, 40), P(1220, 684), IM_COL32(8, 8, 12, 250), 10 * s);
  dl->AddRectFilledMultiColor(P(62, 42), P(1218, 260), IM_COL32(40, 40, 48, 120), IM_COL32(40, 40, 48, 120),
                              IM_COL32(8, 8, 12, 0), IM_COL32(8, 8, 12, 0));
  dl->AddRect(P(60, 40), P(1220, 684), IM_COL32(235, 235, 240, 255), 10 * s, 0, 2.5f * s);
  // Header: the dotted strip, the white-rimmed tab with the title, the page name.
  dl->AddRectFilled(P(66, 46), P(1214, 80), IM_COL32(24, 24, 30, 255), 6 * s);
  for (float x = 76; x < 1206; x += 7) {
    for (float y = 52; y < 76; y += 7) dl->AddCircleFilled(P(x, y), 1.2f * s, IM_COL32(70, 70, 80, 255));
  }
  dl->AddQuadFilled(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(12, 12, 16, 255));
  dl->AddQuad(P(164, 46), P(606, 46), P(588, 80), P(152, 80), IM_COL32(235, 235, 240, 255), 2 * s);
  {
    const char* title = "MY WWE";
    const float ts = 34 * s;
    const ImVec2 sz = TextSize(g_title_font, ts, title);
    Text(dl, g_title_font, ts, ImVec2(P(378, 0).x - sz.x * 0.5f, P(0, 63).y - sz.y * 0.5f), kWhite, title);
    const float ps = 22 * s;
    const ImVec2 psz = TextSize(g_menu_font, ps, "JUKEBOX");
    Text(dl, g_menu_font, ps, ImVec2(P(634, 0).x, P(0, 63).y - psz.y * 0.5f), kWhite, "JUKEBOX");
  }

  // Summary: songs on, and a bar.
  const int n_on = int(std::count(on.begin(), on.end(), true));
  {
    const float ss = 21 * s, cy = P(0, 108).y;
    char buf[64];
    std::snprintf(buf, sizeof buf, "%d OF %d SONGS ON", n_on, kTracks);
    Text(dl, g_menu_font, ss, ImVec2(P(84, 0).x, cy - TextSize(g_menu_font, ss, buf).y * 0.5f), kWhite, buf);
    const char* note = n_on ? "THE MENUS PLAY THE SONGS THAT ARE ON" : "NO MENU MUSIC";
    const ImVec2 nz = TextSize(g_menu_font, 17 * s, note);
    Text(dl, g_menu_font, 17 * s, ImVec2(P(1196, 0).x - nz.x, cy - nz.y * 0.5f), n_on ? kGrey : kGold, note);
    const ImVec2 a = P(330, 102), b(P(1196, 0).x - nz.x - 24 * s, P(0, 114).y);
    dl->AddRectFilled(a, b, IM_COL32(40, 40, 48, 255), 4 * s);
    if (n_on) {
      dl->AddRectFilledMultiColor(a, ImVec2(a.x + (b.x - a.x) * n_on / kTracks, b.y), IM_COL32(200, 30, 32, 255),
                                  IM_COL32(240, 70, 60, 255), IM_COL32(240, 70, 60, 255), IM_COL32(200, 30, 32, 255));
    }
  }

  // The list.
  const float row_h = 62, gap = 6, list_top = 134;
  for (int r = 0; r < kVisible && top_ + r < kTracks; ++r) {
    const int i = top_ + r;
    const Track& tr = kTrack[i];
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
    // The switch.
    const ImVec2 sa = P(94, y + 18), sb = P(150, y + 44);
    dl->AddRectFilled(sa, sb, on[i] ? kGreen : IM_COL32(70, 70, 78, 255), 13 * s);
    const float kx = on[i] ? sb.x - 13 * s : sa.x + 13 * s;
    dl->AddCircleFilled(ImVec2(kx, (sa.y + sb.y) * 0.5f), 10 * s, kWhite, 20);
    // Number, name, kind and length.
    const float ns = 21 * s, ds = 16 * s;
    char num[8];
    std::snprintf(num, sizeof num, "%02d", i + 1);
    Text(dl, g_menu_font, ns, P(166, y + 7), on[i] ? kGrey : kDim, num);
    Text(dl, g_menu_font, ns, P(206, y + 7), on[i] || sel ? kWhite : kGrey, tr.name);
    const std::string sub = std::string(tr.theme ? "ENTRANCE THEME" : "MENU THEME") + "  -  " + Length(tr.seconds);
    Text(dl, g_menu_font, ds, P(206, y + 35), sel ? IM_COL32(235, 205, 205, 255) : kDim, sub.c_str());
    if (i == playing) {
      Equalizer(dl, P(660, y + 14), P(700, y + 48), 4, t, true, IM_COL32(200, 30, 32, 255), kGold);
    } else if (!on[i]) {
      const ImVec2 oz = TextSize(g_menu_font, ds, "OFF");
      Text(dl, g_menu_font, ds, ImVec2(P(726, 0).x - oz.x, P(0, y + 22).y), kDim, "OFF");
    }
  }
  // Scroll bar.
  {
    const float tp = list_top, h = kVisible * (row_h + gap) - gap;
    dl->AddRectFilled(P(750, tp), P(754, tp + h), IM_COL32(40, 40, 48, 255), 2 * s);
    const float bh = h * kVisible / kTracks, by = tp + (h - bh) * top_ / float(kTracks - kVisible);
    dl->AddRectFilled(P(750, by), P(754, by + bh), IM_COL32(200, 200, 208, 255), 2 * s);
  }

  // Now playing: a turning record and bars.
  const ImVec2 da = P(766, list_top), db = P(1200, list_top + kVisible * (row_h + gap) - gap);
  dl->AddRectFilled(da, db, IM_COL32(24, 24, 30, 245), 6 * s);
  dl->AddRect(da, db, IM_COL32(70, 70, 80, 255), 6 * s, 0, 1.0f * s);
  {
    const float cx = 983;
    Text(dl, g_menu_font, 16 * s, P(790, list_top + 14), kDim, "NOW PLAYING");
    const ImVec2 c = P(cx, 280);
    const float R = 104 * s;
    dl->AddCircleFilled(c, R, IM_COL32(14, 14, 16, 255), 64);
    for (float g = 0.42f; g < 0.97f; g += 0.07f) dl->AddCircle(c, R * g, IM_COL32(40, 40, 46, 255), 64, 1.0f * s);
    const float spin = playing >= 0 ? t * 3.4f : 0.6f;
    for (int k = 0; k < 2; ++k) {  // (the light on the grooves)
      const float ang = spin + k * 3.14159265f;
      dl->PathArcTo(c, R * 0.8f, ang - 0.35f, ang + 0.35f, 12);
      dl->PathStroke(IM_COL32(90, 90, 100, 180), 0, 3.0f * s);
    }
    dl->AddCircleFilled(c, R * 0.34f, playing >= 0 ? IM_COL32(200, 22, 26, 255) : IM_COL32(80, 80, 88, 255), 40);
    dl->AddCircleFilled(c, R * 0.05f, IM_COL32(14, 14, 16, 255), 16);
    dl->AddLine(ImVec2(c.x + std::cos(spin) * R * 0.12f, c.y + std::sin(spin) * R * 0.12f),
                ImVec2(c.x + std::cos(spin) * R * 0.3f, c.y + std::sin(spin) * R * 0.3f), IM_COL32(255, 255, 255, 160),
                2.0f * s);
    auto centred = [&](float y, float size, const char* text, ImU32 colour) {
      const ImVec2 z = TextSize(g_menu_font, size, text);
      Text(dl, g_menu_font, size, ImVec2(P(cx, 0).x - z.x * 0.5f, P(0, y).y), colour, text);
    };
    if (playing >= 0) {
      const Track& tr = kTrack[playing];
      centred(398, 27 * s, tr.name, kWhite);
      const std::string sub = std::string(tr.theme ? "ENTRANCE THEME" : "MENU THEME") + "  -  " + Length(tr.seconds);
      centred(432, 17 * s, sub.c_str(), kGold);
    } else {
      centred(398, 22 * s, n_on ? "-" : "THE MENUS ARE SILENT", kGrey);
      centred(432, 17 * s, n_on ? "The menus play one of the songs that are on." : "Turn a song on to hear it.", kDim);
    }
    Equalizer(dl, P(800, 470), P(1166, 590), 24, t, playing >= 0, IM_COL32(150, 18, 20, 255),
              IM_COL32(255, 120, 60, 255));
    centred(604, 15 * s, "X plays the chosen song now.", kDim);
  }

  // Footer: the buttons.
  {
    const float cy = P(0, 660).y;
    float x = P(84, 0).x;
    x = Hint(dl, x, cy, s, "B", IM_COL32(200, 40, 40, 255), "BACK");
    x = Hint(dl, x, cy, s, "A", IM_COL32(60, 160, 60, 255), on[sel_] ? "TURN OFF" : "TURN ON");
    x = Hint(dl, x, cy, s, "X", IM_COL32(40, 100, 200, 255), "PREVIEW");
    x = Hint(dl, x, cy, s, "Y", IM_COL32(215, 170, 20, 255),
             std::find(on.begin(), on.end(), false) != on.end() ? "ALL ON" : "ALL OFF");
    x = Hint(dl, x, cy, s, "LB", IM_COL32(90, 90, 100, 255), "");
    Hint(dl, x - 26 * s, cy, s, "RB", IM_COL32(90, 90, 100, 255), "PAGE");
  }

  if (back) {
    g_wait_release = true;
    g_open = false;
  }
}

}  // namespace

namespace svr2011 {

void InstallJukeboxPage(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input) {
  g_input = input;
  new JukeboxPage(drawer);  // lives for the whole run
}

void SetJukeboxPageFonts(ImFont* menu, ImFont* title) {
  g_menu_font = menu;
  g_title_font = title;
}

void OpenJukeboxPage() { g_open_requested = true; }

bool JukeboxPageHoldsInput() { return g_open.load() || g_wait_release.load(); }

}  // namespace svr2011
