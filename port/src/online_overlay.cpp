// WWE SmackDown vs. Raw 2011 - the ONLINE overlay (online_overlay.h).

#include "online_overlay.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fstream>

#include <imgui.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/kernel/xam/module.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "generated/default/svr2011_init.h"
#include "json_lite.h"
#include "online_net.h"
#include "p2p.h"

// The game's own Xbox LIVE invite path (it restarts itself to join): off - the
// restarted game says "a sign-in change has occurred" (see the notes below).
REXCVAR_DEFINE_BOOL(online_invite_native, false, "Online",
                    "Invites accepted through the game's own Xbox LIVE path (restarts the game; experimental)");

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kTitleId = 0x5451085D;                   // (the game's)
constexpr uint32_t kLiveInviteAccepted = 0x02000002;        // XN_LIVE_INVITE_ACCEPTED (data: the user index)
constexpr uint32_t kSystemUi = 0x00000009;                  // XN_SYS_UI (data: shown)


std::string Base64(const uint8_t* d, size_t n) {
  static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = uint32_t(d[i]) << 16 | (i + 1 < n ? uint32_t(d[i + 1]) << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
    out += k[v >> 18 & 63], out += k[v >> 12 & 63];
    out += i + 1 < n ? k[v >> 6 & 63] : '=';
    out += i + 2 < n ? k[v & 63] : '=';
  }
  return out;
}

std::vector<uint8_t> Unbase64(const std::string& s) {
  std::vector<uint8_t> out;
  uint32_t v = 0;
  int bits = 0;
  for (char c : s) {
    int x = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
          : c == '+' ? 62 : c == '/' ? 63 : -1;
    if (x < 0) continue;
    v = v << 6 | uint32_t(x), bits += 6;
    if (bits >= 8) out.push_back(uint8_t(v >> (bits - 8))), bits -= 8;
  }
  return out;
}

void Put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (24 - 8 * i));
}
void Put64(uint8_t* p, uint64_t v) {
  Put32(p, uint32_t(v >> 32)), Put32(p + 4, uint32_t(v));
}

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(uint8_t(c)));
  return s;
}

// -- the server's list, polled in the background ------------------------------------

struct Friend {
  std::string name, status;  // status: playing / online / offline
  bool mutual = false;
};
struct Invite {
  std::string from, xuid, session, kind;
};
struct State {
  bool signed_in = false, answered = false;
  std::string error;
  std::vector<Friend> friends;
  std::vector<std::string> added_you;
  std::vector<Invite> invites;
};

rex::input::InputSystem* g_input = nullptr;
std::filesystem::path g_relaunch_file;  // UserData\relaunch.bin
rex::system::KernelState* g_kernel = nullptr;
ImFont* g_menu_font = nullptr;
ImFont* g_title_font = nullptr;

std::mutex g_mutex;
State g_state;
std::string g_message;  // the last action's outcome (shown on the overlay)
Clock::time_point g_message_at{};
std::set<std::string> g_invited;  // friends invited to the current session (its id + name)
std::condition_variable g_poll;
bool g_poll_now = false;

std::atomic<bool> g_toggle{false}, g_open{false}, g_wait_release{false};
std::atomic<uint16_t> g_guest_pad{0};  // (OnlineOverlayPad)

// Touches while it's open (a phone): taps, as fractions of the window, for the
// next frame; every touch is the overlay's then (not the on-screen pad's).
rex::ui::Window* g_window = nullptr;
std::mutex g_tap_mutex;
std::vector<ImVec2> g_taps;
struct TouchDown {
  float x, y;
};
std::map<uint32_t, TouchDown> g_downs;

class OverlayTouch final : public rex::ui::WindowInputListener {
 public:
  void OnTouchEvent(rex::ui::TouchEvent& e) override {
    using A = rex::ui::TouchEvent::Action;
    if (!g_open || !g_window) return;
    const float w = float(g_window->GetActualPhysicalWidth()), h = float(g_window->GetActualPhysicalHeight());
    if (w <= 0 || h <= 0) return;
    const float x = e.x() / w, y = e.y() / h;
    std::lock_guard lock(g_tap_mutex);
    if (e.action() == A::kDown) {
      g_downs[e.pointer_id()] = {x, y};
    } else if (e.action() == A::kUp) {
      auto it = g_downs.find(e.pointer_id());
      // (a tap: lifted near where it went down)
      if (it != g_downs.end() && std::abs(it->second.x - x) < 0.04f && std::abs(it->second.y - y) < 0.05f)
        g_taps.push_back(ImVec2(x, y));
      g_downs.erase(e.pointer_id());
    } else if (e.action() == A::kCancel) {
      g_downs.erase(e.pointer_id());
    }
    e.set_handled(true);
  }
};
OverlayTouch g_touch;
std::atomic<bool> g_from_game{false};  // opened by the game's INVITE FRIENDS (XN_SYS_UI sent)

// An invite accepted: what the game's XInviteGetAcceptedInfo gets.
struct Accepted {
  uint8_t info[60] = {};
  uint64_t inviter = 0;
  Clock::time_point at{};
};
std::optional<Accepted> g_accepted;
Clock::time_point g_relaunched_at{};  // (this copy: started by an invite's relaunch)

void Say(const std::string& m) {
  std::lock_guard lock(g_mutex);
  g_message = m;
  g_message_at = Clock::now();
}

bool SignedIn() {
  return rex::cvar::Query<bool>("online_enabled") && !rex::cvar::Query<std::string>("online_token").empty();
}

void Poll() {
  if (!SignedIn()) {
    std::lock_guard lock(g_mutex);
    g_state = State{};
    return;
  }
  // (?here=1: this game is running, signed in - its friends see it Online)
  auto r = net::ServerRequest("GET", "/api/friends?here=1");
  State s;
  s.signed_in = true;
  Json j;
  if (r && r->status == 200 && JsonReader{r->body}.Value(j) && j["friends"]) {
    s.answered = true;
    for (const Json& f : j["friends"]->items) s.friends.push_back({f.Str("name"), f.Str("status"),
                                                                   f["mutual"] && f["mutual"]->b});
    if (const Json* a = j["added_you"])
      for (const Json& n : a->items) s.added_you.push_back(n.s);
    if (const Json* a = j["invites"])
      for (const Json& v : a->items) s.invites.push_back({v.Str("from"), v.Str("xuid"), v.Str("session"), v.Str("kind")});
  } else {
    s.error = !r ? "THE SERVER DIDN'T ANSWER" : r->status == 404 ? "THIS SERVER HAS NO FRIENDS LIST YET"
                                                                 : "THE SERVER REFUSED (" + std::to_string(r->status) + ")";
  }
  std::lock_guard lock(g_mutex);
  g_state = std::move(s);
}

void PollThread() {
  for (;;) {
    Poll();
    std::unique_lock lock(g_mutex);
    g_poll.wait_for(lock, g_open ? std::chrono::seconds(5) : std::chrono::seconds(20), [] { return g_poll_now; });
    g_poll_now = false;
  }
}

void PollSoon() {
  std::lock_guard lock(g_mutex);
  g_poll_now = true;
  g_poll.notify_all();
}

// -- actions (each on a thread of its own) -------------------------------------------

void SendInvite(std::string name) {
  uint8_t info[60];
  uint32_t slots = 0;
  if (!P2PSessionInfo(info, &slots)) return Say("YOU'RE NOT IN A SESSION: CREATE OR JOIN ONE FIRST");
  const std::string key = Base64(info, 8) + name;
  const std::string kind = slots > 6 ? "Royal Rumble" : "Match";
  std::thread([name, key, kind, session = Base64(info, 60)] {
    auto r = net::ServerRequest("POST", "/api/invite",
                                "{\"to\": " + Quote(name) + ", \"session\": \"" + session + "\", \"kind\": " + Quote(kind) + "}",
                                {{"Content-Type", "application/json"}});
    if (r && r->status == 200) {
      {
        std::lock_guard lock(g_mutex);
        g_invited.insert(key);
      }
      REXLOG_INFO("online overlay: invited {}", name);
      Say("INVITE SENT TO " + Upper(name));
    } else {
      Json j;
      std::string error;
      if (r && JsonReader{r->body}.Value(j)) error = j.Str("error");
      Say(error.empty() ? "THE INVITE DIDN'T GO" : Upper(error));
    }
  }).detach();
}

void Decline(const std::string& from) {
  {
    std::lock_guard lock(g_mutex);
    auto& v = g_state.invites;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Invite& i) { return i.from == from; }), v.end());
  }
  std::thread([from] {
    net::ServerRequest("POST", "/api/invite", "{\"decline\": " + Quote(from) + "}", {{"Content-Type", "application/json"}});
  }).detach();
}

void Accept(const Invite& invite) {
  const auto info = Unbase64(invite.session);
  if (info.size() != 60) return Say("THAT INVITE IS BROKEN");
  Decline(invite.from);  // (taken: off the list)
  if (!REXCVAR_GET(online_invite_native)) {
    // (the session first in the game's search, its private slots open to this game)
    P2PExpectInvite(info.data());
    const bool rumble = invite.kind == "Royal Rumble";
    Say(Upper(invite.from) + "'S SESSION: ONLINE > " + (rumble ? "ROYAL RUMBLE" : "MATCH") +
        " > PLAYER MATCH > CUSTOM MATCH > SEARCH");
    std::thread([info] { P2PReachHost(info.data()); }).detach();
    return;
  }
  Say("JOINING " + Upper(invite.from) + "'S SESSION...");
  std::thread([info, invite] {
    const bool there = P2PReachHost(info.data());
    {
      std::lock_guard lock(g_mutex);
      Accepted a;
      std::memcpy(a.info, info.data(), 60);
      a.inviter = std::strtoull(invite.xuid.c_str(), nullptr, 16);
      a.at = Clock::now();
      g_accepted = a;
    }
    if (!there) Say(Upper(invite.from) + " ISN'T ANSWERING - TRYING ANYWAY");
    REXLOG_INFO("online overlay: invite from {} accepted (host {})", invite.from, there ? "answered" : "silent");
    if (g_kernel) g_kernel->BroadcastNotification(kLiveInviteAccepted, 0);
  }).detach();
  g_toggle = g_open.load();  // (closed: the game takes over)
}

void AddFriend(const std::string& name) {
  std::thread([name] {
    auto r = net::ServerRequest("POST", "/api/friends", "{\"add\": " + Quote(name) + "}", {{"Content-Type", "application/json"}});
    Json j;
    if (r && r->status == 200) {
      Say(Upper(name) + " ADDED");
      PollSoon();
    } else if (r && JsonReader{r->body}.Value(j) && !j.Str("error").empty()) {
      Say(Upper(j.Str("error")));
    } else {
      Say("THE SERVER DIDN'T ANSWER");
    }
  }).detach();
}

// -- the overlay -----------------------------------------------------------------------

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 at, ImU32 colour, const char* text) {
  dl->AddText(font ? font : ImGui::GetFont(), size, at, colour, text);
}
ImVec2 TextSize(ImFont* font, float size, const char* text) {
  return (font ? font : ImGui::GetFont())->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
}

uint16_t PadButtons() {
  using namespace rex::input;
  X_INPUT_STATE s = {};
  if (!g_input || !g_input->HeldState(0, &s)) return 0;
  uint16_t b = s.gamepad.buttons;
  constexpr int kDead = 16000;
  if (s.gamepad.thumb_ly > kDead) b |= X_INPUT_GAMEPAD_DPAD_UP;
  if (s.gamepad.thumb_ly < -kDead) b |= X_INPUT_GAMEPAD_DPAD_DOWN;
  return b;
}

class OnlineOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit OnlineOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  enum Kind { kInviteRow, kFriendRow, kAddedRow, kAddRow };
  struct Row {
    Kind kind;
    int index;  // (in its list)
  };
  void Close();
  void Toast(ImGuiIO& io);

  int sel_ = 0, top_ = 0;
  std::string typed_;  // ADD FRIEND
  uint16_t prev_buttons_ = 0;
  bool wait_release_ = false, combo_down_ = false;
  Clock::time_point opened_at_{}, repeat_at_{};
  std::set<std::string> seen_invites_;  // (the notice: once each)
  std::string toast_;
  Clock::time_point toast_until_{};
};

void OnlineOverlay::Close() {
  g_open = false;
  g_wait_release = true;
  if (g_from_game.exchange(false) && g_kernel) g_kernel->BroadcastNotification(kSystemUi, 0);
}

void OnlineOverlay::Toast(ImGuiIO& io) {
  State s;
  {
    std::lock_guard lock(g_mutex);
    s.invites = g_state.invites;
  }
  for (const Invite& i : s.invites) {
    if (!seen_invites_.insert(i.from + i.session).second) continue;
    toast_ = Upper(i.from) + " INVITED YOU TO A " + Upper(i.kind.empty() ? "match" : i.kind) + "  -  " +
             Upper(rex::cvar::Query<std::string>("bind_online_menu")) + " OR BACK + RB";
    toast_until_ = Clock::now() + std::chrono::seconds(8);
  }
  if (g_open || Clock::now() > toast_until_ || toast_.empty()) return;
  const float sc = std::min(io.DisplaySize.x / 1280.0f, io.DisplaySize.y / 720.0f), size = 20 * sc;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 sz = TextSize(g_menu_font, size, toast_.c_str());
  const ImVec2 a(io.DisplaySize.x - sz.x - 40 * sc, 24 * sc), b(io.DisplaySize.x - 16 * sc, 24 * sc + sz.y + 18 * sc);
  dl->AddRectFilled(a, b, IM_COL32(8, 8, 12, 235), 8 * sc);
  dl->AddRect(a, b, IM_COL32(200, 30, 32, 255), 8 * sc, 0, 2 * sc);
  Text(dl, g_menu_font, size, ImVec2(a.x + 12 * sc, a.y + 9 * sc), IM_COL32(255, 255, 255, 255), toast_.c_str());
}

void OnlineOverlay::OnDraw(ImGuiIO& io) {
  using namespace rex::input;
  // BACK + RB opens / closes it (from any screen; closed, the pad as the game
  // reads it - HeldState is only kept while a page holds the pad).
  {
    X_INPUT_STATE st = {};
    bool known = false;  // (just opened: no held state yet - the combo is still down)
    if (OnlineOverlayHoldsInput()) {
      known = g_input && g_input->HeldState(0, &st);
    } else {
      st.gamepad.buttons = g_guest_pad.load();
      known = true;
    }
    if (known) {
      const uint16_t b = st.gamepad.buttons;
      const bool combo = (b & X_INPUT_GAMEPAD_BACK) && (b & X_INPUT_GAMEPAD_RIGHT_SHOULDER);
      if (combo && !combo_down_) g_toggle = true;
      combo_down_ = combo;
    }
  }
  if (g_toggle.exchange(false)) {
    REXLOG_INFO("online overlay: {}", g_open ? "closed" : "opened");
    if (g_open) {
      Close();
    } else {
      g_open = true;
      wait_release_ = true;
      opened_at_ = Clock::now();
      sel_ = top_ = 0;
      PollSoon();
    }
  }
  Toast(io);
  if (!g_open) {
    if (g_wait_release && PadButtons() == 0) g_wait_release = false;
    return;
  }

  State st;
  std::string message;
  bool fresh_message;
  std::set<std::string> invited;
  {
    std::lock_guard lock(g_mutex);
    st = g_state;
    message = g_message;
    fresh_message = Clock::now() - g_message_at < std::chrono::seconds(15);
    invited = g_invited;
  }
  uint8_t info[60];
  uint32_t slots = 0;
  const bool in_session = P2PSessionInfo(info, &slots);
  const std::string session_key = in_session ? Base64(info, 8) : std::string();

  std::vector<Row> rows;
  for (int i = 0; i < int(st.invites.size()); ++i) rows.push_back({kInviteRow, i});
  for (int i = 0; i < int(st.friends.size()); ++i) rows.push_back({kFriendRow, i});
  for (int i = 0; i < int(st.added_you.size()); ++i) rows.push_back({kAddedRow, i});
  if (st.signed_in) rows.push_back({kAddRow, 0});
  const int n = int(rows.size());

  // Input: controller (edges, repeat for held directions), keys, the mouse.
  const uint16_t buttons = PadButtons();
  const auto now = Clock::now();
  if (wait_release_) {
    if (buttons == 0 && now - opened_at_ > std::chrono::milliseconds(150)) wait_release_ = false;
    prev_buttons_ = buttons;
  }
  const uint16_t pressed = wait_release_ ? 0 : uint16_t(buttons & ~prev_buttons_);
  constexpr uint16_t kDirs = X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN;
  uint16_t act = pressed;
  if (pressed & kDirs) {
    repeat_at_ = now + std::chrono::milliseconds(400);
  } else if (!wait_release_ && (buttons & kDirs) && now >= repeat_at_) {
    act |= buttons & kDirs;
    repeat_at_ = now + std::chrono::milliseconds(120);
  }
  if (!wait_release_) prev_buttons_ = buttons;
  auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
  int move = 0;
  if ((act & X_INPUT_GAMEPAD_DPAD_UP) || key(ImGuiKey_UpArrow)) move = -1;
  if ((act & X_INPUT_GAMEPAD_DPAD_DOWN) || key(ImGuiKey_DownArrow)) move = 1;
  bool choose = (pressed & X_INPUT_GAMEPAD_A) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
  bool other = (pressed & X_INPUT_GAMEPAD_X) != 0;  // (decline)
  const bool back = (pressed & (X_INPUT_GAMEPAD_B | X_INPUT_GAMEPAD_START)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
  if (n) sel_ = (sel_ + move + n) % n;
  sel_ = std::clamp(sel_, 0, std::max(0, n - 1));
  // ADD FRIEND: typing (the keyboard)
  const bool typing = n && rows[sel_].kind == kAddRow;
  if (typing) {
    for (ImWchar c : io.InputQueueCharacters) {
      if (typed_.size() < 15 && c < 128 && (std::isalnum(int(c)) || c == ' ' || c == '_' || c == '.' || c == '-'))
        typed_ += char(c);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true) && !typed_.empty()) typed_.pop_back();
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) && !typing) other = true;

  // Layout in the game's 1280 x 720 frame, letterboxed: a panel at the right.
  float fw = io.DisplaySize.x, fh = fw * 9.0f / 16.0f;
  if (fh > io.DisplaySize.y) fh = io.DisplaySize.y, fw = fh * 16.0f / 9.0f;
  const float ox = (io.DisplaySize.x - fw) * 0.5f, oy = (io.DisplaySize.y - fh) * 0.5f, s = fh / 720.0f;
  auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };
  const ImU32 kWhite = IM_COL32(255, 255, 255, 255), kGrey = IM_COL32(170, 170, 178, 255),
              kDim = IM_COL32(120, 120, 128, 255), kGreen = IM_COL32(110, 210, 90, 255),
              kGold = IM_COL32(255, 200, 60, 255), kRed = IM_COL32(200, 30, 32, 255);
  constexpr float L = 690, R = 1250, T = 30, B = 690, kRowH = 52, kListTop = 150;
  const int visible = int((B - 90 - kListTop) / kRowH);
  if (sel_ < top_) top_ = sel_;
  if (sel_ >= top_ + visible) top_ = sel_ - visible + 1;
  top_ = std::clamp(top_, 0, std::max(0, n - visible));

  // The mouse: a row under it is the selection; a click chooses it.
  const ImVec2 mouse = io.MousePos;
  for (int k = 0; k < visible && top_ + k < n; ++k) {
    const ImVec2 a = P(L + 16, kListTop + k * kRowH), b = P(R - 16, kListTop + k * kRowH + kRowH - 6);
    if (mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y) {
      if (io.MouseDelta.x != 0 || io.MouseDelta.y != 0 || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) sel_ = top_ + k;
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) choose = true;
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) other = true;
    }
  }

  // Taps (a phone): a row is chosen; outside the panel, it closes.
  bool tapped_out = false;
  {
    std::vector<ImVec2> taps;
    {
      std::lock_guard lock(g_tap_mutex);
      taps.swap(g_taps);
    }
    for (const ImVec2& t : taps) {
      const ImVec2 at(t.x * io.DisplaySize.x, t.y * io.DisplaySize.y);
      const ImVec2 pa = P(L, T), pb = P(R, B);
      if (at.x < pa.x || at.x > pb.x || at.y < pa.y || at.y > pb.y) {
        tapped_out = true;
        continue;
      }
      for (int k = 0; k < visible && top_ + k < n; ++k) {
        const ImVec2 a = P(L + 16, kListTop + k * kRowH), b = P(R - 16, kListTop + k * kRowH + kRowH - 6);
        if (at.x >= a.x && at.x < b.x && at.y >= a.y && at.y < b.y) sel_ = top_ + k, choose = true;
      }
    }
  }

  // Actions.
  if (n && (choose || other)) {
    const Row row = rows[sel_];
    if (row.kind == kInviteRow) {
      if (choose) Accept(st.invites[row.index]);
      else Decline(st.invites[row.index].from), Say("INVITE DECLINED");
    } else if (row.kind == kFriendRow && choose) {
      const Friend& f = st.friends[row.index];
      if (!in_session) Say("CREATE OR JOIN A SESSION FIRST, THEN INVITE");
      else if (f.status == "offline") Say(Upper(f.name) + " ISN'T ONLINE");
      else SendInvite(f.name);
    } else if (row.kind == kAddedRow && choose) {
      AddFriend(st.added_you[row.index]);
    } else if (row.kind == kAddRow && choose) {
      if (typed_.size() >= 3) AddFriend(typed_), typed_.clear();
      else Say("TYPE THE PLAYER'S NAME (KEYBOARD), THEN ENTER");
    }
  }

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
  dl->AddRectFilled(P(L, T), P(R, B), IM_COL32(8, 8, 12, 245), 10 * s);
  dl->AddRectFilledMultiColor(P(L + 2, T + 2), P(R - 2, T + 200), IM_COL32(40, 40, 48, 120), IM_COL32(40, 40, 48, 120),
                              IM_COL32(8, 8, 12, 0), IM_COL32(8, 8, 12, 0));
  dl->AddRect(P(L, T), P(R, B), IM_COL32(235, 235, 240, 255), 10 * s, 0, 2.5f * s);
  // header
  dl->AddRectFilled(P(L + 6, T + 6), P(R - 6, T + 44), IM_COL32(24, 24, 30, 255), 6 * s);
  dl->AddRectFilled(P(L + 6, T + 6), P(L + 12, T + 44), kRed);
  Text(dl, g_title_font, 32 * s, P(L + 24, T + 8), kWhite, "ONLINE");
  {
    const std::string who = st.signed_in ? Upper(rex::cvar::Query<std::string>("online_name")) : "NOT SIGNED IN";
    const ImVec2 sz = TextSize(g_menu_font, 20 * s, who.c_str());
    Text(dl, g_menu_font, 20 * s, ImVec2(P(R - 20, 0).x - sz.x, P(0, T + 15).y), kGrey, who.c_str());
  }
  // the session line
  {
    std::string line;
    ImU32 colour = kGrey;
    if (!st.signed_in) {
      line = "SIGN IN ON THE LAUNCHER'S ONLINE TAB";
    } else if (in_session) {
      line = std::string(slots > 6 ? "ROYAL RUMBLE" : "MATCH") + " SESSION: A INVITES AN ONLINE FRIEND";
      colour = kGold;
    } else {
      line = "CREATE OR JOIN A SESSION TO INVITE FRIENDS";
    }
    Text(dl, g_menu_font, 19 * s, P(L + 24, T + 62), colour, line.c_str());
    if (!st.error.empty()) Text(dl, g_menu_font, 19 * s, P(L + 24, T + 88), kRed, st.error.c_str());
    else if (st.signed_in && !st.answered) Text(dl, g_menu_font, 19 * s, P(L + 24, T + 88), kDim, "LOADING...");
  }

  // the list
  for (int k = 0; k < visible && top_ + k < n; ++k) {
    const int i = top_ + k;
    const Row row = rows[i];
    const float y = kListTop + k * kRowH;
    const bool on = i == sel_;
    dl->AddRectFilled(P(L + 16, y), P(R - 16, y + kRowH - 6), on ? IM_COL32(150, 20, 24, 255) : IM_COL32(26, 26, 32, 255),
                      6 * s);
    if (on) dl->AddRect(P(L + 16, y), P(R - 16, y + kRowH - 6), kWhite, 6 * s, 0, 1.5f * s);
    std::string name, right;
    ImU32 rc = kGrey;
    switch (row.kind) {
      case kInviteRow: {
        const Invite& v = st.invites[row.index];
        name = Upper(v.from);
        right = "INVITES YOU (" + Upper(v.kind.empty() ? "match" : v.kind) + ")";
        rc = kGold;
        break;
      }
      case kFriendRow: {
        const Friend& f = st.friends[row.index];
        name = Upper(f.name);
        if (f.status == "playing") right = "IN AN ONLINE MATCH", rc = kGreen;
        else if (f.status == "online") right = "ONLINE", rc = kGreen;
        else right = f.mutual ? "OFFLINE" : "OFFLINE (HASN'T ADDED YOU)", rc = kDim;
        if (in_session && invited.count(session_key + f.name)) right = "INVITED", rc = kGold;
        break;
      }
      case kAddedRow:
        name = Upper(st.added_you[row.index]);
        right = "ADDED YOU", rc = kGrey;
        break;
      case kAddRow:
        name = "ADD FRIEND: " + Upper(typed_) + (on && (int(ImGui::GetTime() * 2) & 1) ? "_" : "");
        rc = kDim;
        break;
    }
    Text(dl, g_menu_font, 22 * s, P(L + 30, y + 11), kWhite, name.c_str());
    const ImVec2 sz = TextSize(g_menu_font, 18 * s, right.c_str());
    Text(dl, g_menu_font, 18 * s, ImVec2(P(R - 30, 0).x - sz.x, P(0, y + 14).y), rc, right.c_str());
  }
  if (st.signed_in && st.answered && st.friends.empty() && st.invites.empty() && st.added_you.empty()) {
    Text(dl, g_menu_font, 19 * s, P(L + 24, kListTop + kRowH + 8), kDim, "NO FRIENDS YET: ADD ONE BELOW OR ON THE LAUNCHER");
  }
  if (top_ > 0) Text(dl, g_menu_font, 18 * s, P(R - 40, kListTop - 26), kGrey, "^");
  if (top_ + visible < n) Text(dl, g_menu_font, 18 * s, P(R - 40, B - 94), kGrey, "v");

  // the outcome of the last action, and the buttons
  if (fresh_message) Text(dl, g_menu_font, 19 * s, P(L + 24, B - 78), kGold, message.c_str());
  std::string help = "B CLOSE";
  if (n) {
    switch (rows[sel_].kind) {
      case kInviteRow: help = "A ACCEPT    X DECLINE    B CLOSE"; break;
      case kFriendRow: help = in_session ? "A INVITE    B CLOSE" : "B CLOSE"; break;
      case kAddedRow: help = "A ADD BACK    B CLOSE"; break;
      case kAddRow: help = "TYPE A NAME    ENTER ADD    ESC CLOSE"; break;
    }
  }
  if (rex::cvar::Query<bool>("touch_controls")) help = "TAP A ROW    TAP OUTSIDE TO CLOSE";  // (a phone)
  Text(dl, g_menu_font, 18 * s, P(L + 24, B - 44), kGrey, help.c_str());
  if (tapped_out || (back && !(typing && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !typed_.empty()))) Close();
  else if (back) typed_.clear();
}

// -- the game restarting itself to join (online_overlay.h) ------------------------------

rex::kernel::xam::XamModule::LoaderData* LoaderData() {
  if (!g_kernel) return nullptr;
  auto xam = g_kernel->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
  return xam ? &xam->loader_data() : nullptr;
}

// relaunch.bin: "SVRL", u32 launch data size, the data, the invite (60-byte
// XSESSION_INFO + the inviter's XUID).
bool SaveRelaunch() {
  auto* loader = LoaderData();
  if (!loader || g_relaunch_file.empty()) return false;
  std::optional<Accepted> a;
  {
    std::lock_guard lock(g_mutex);
    a = g_accepted;
  }
  std::string out = "SVRL";
  const uint32_t size = loader->launch_data_present ? uint32_t(loader->launch_data.size()) : 0;
  out.append(reinterpret_cast<const char*>(&size), 4);
  out.append(reinterpret_cast<const char*>(loader->launch_data.data()), size);
  if (a) {
    out.append(reinterpret_cast<const char*>(a->info), 60);
    out.append(reinterpret_cast<const char*>(&a->inviter), 8);
  }
  {
    std::string hex;
    for (uint32_t i = 0; i < size; ++i) {
      char h[4];
      std::snprintf(h, sizeof h, "%02X", loader->launch_data[i]);
      hex += h;
      if (i % 4 == 3) hex += ' ';
    }
    REXLOG_INFO("online overlay: launch data {}", hex);
  }
  std::ofstream f(g_relaunch_file, std::ios::binary | std::ios::trunc);
  f.write(out.data(), std::streamsize(out.size()));
  return bool(f);
}

void LoadRelaunch() {
  std::error_code ec;
  if (g_relaunch_file.empty() || !std::filesystem::exists(g_relaunch_file, ec)) return;
  const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(g_relaunch_file, ec);
  std::ifstream f(g_relaunch_file, std::ios::binary);
  std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  f.close();
  std::filesystem::remove(g_relaunch_file, ec);
  uint32_t size = 0;
  if (age > std::chrono::minutes(3) || d.size() < 8 || d.compare(0, 4, "SVRL") != 0) return;
  std::memcpy(&size, d.data() + 4, 4);
  if (8 + size > d.size()) return;
  if (auto* loader = LoaderData(); loader && size) {
    loader->launch_data.assign(d.begin() + 8, d.begin() + 8 + size);
    // An invite (word 0 = 2): word 2 is the player who took it - the game's
    // invite object never learns it here (-1), and the game would then wait
    // for "player -1" to sign in. It's player 1 (index 0).
    auto& v = loader->launch_data;
    if (size >= 12 && v[3] == 2 && v[8] == 0xFF && v[9] == 0xFF && v[10] == 0xFF && v[11] == 0xFF)
      v[8] = v[9] = v[10] = v[11] = 0;
    loader->launch_data_present = true;
  }
  if (d.size() >= 8 + size + 68) {
    Accepted a;
    std::memcpy(a.info, d.data() + 8 + size, 60);
    std::memcpy(&a.inviter, d.data() + 8 + size + 60, 8);
    a.at = Clock::now();
    {
      std::lock_guard lock(g_mutex);
      g_accepted = a;
      g_relaunched_at = a.at;
    }
    // (the host again, once P2P is up: so its packets get through)
    std::thread([a] {
      std::this_thread::sleep_for(std::chrono::seconds(15));
      P2PReachHost(a.info);
    }).detach();
  }
  REXLOG_INFO("online overlay: restarted to join an invite ({} bytes of launch data)", size);
}

// A new copy of the game, with this one's command line, folder and settings.
bool StartNewCopy() {
#ifdef _WIN32
  wchar_t exe[MAX_PATH];
  if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
  std::wstring cmd = GetCommandLineW();
  SetEnvironmentVariableW(L"SVR2011_RELAUNCHED", L"1");  // (inherited)
  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
#else
  return false;  // (Android: not yet - the activity would have to restart itself)
#endif
}

}  // namespace

void InstallOnlineOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, rex::input::InputSystem* input,
                          rex::system::KernelState* kernel, const std::filesystem::path& user_data) {
  g_window = window;
  if (window) {  // (before the on-screen pad's listener: 900)
    window->app_context().CallInUIThread([window] { window->AddInputListener(&g_touch, 950); });
  }
  g_input = input;
  g_kernel = kernel;
  g_relaunch_file = user_data / "relaunch.bin";
  LoadRelaunch();
  new OnlineOverlay(drawer);  // lives for the whole run
  rex::ui::RegisterBind("bind_online_menu", "F1", "Online menu (friends, invites)", [] { g_toggle = true; });
  std::thread(PollThread).detach();
}

void SetOnlineOverlayFonts(ImFont* menu, ImFont* title) {
  g_menu_font = menu;
  g_title_font = title;
}

void ToggleOnlineOverlay() { g_toggle = true; }

bool OnlineOverlayHoldsInput() { return g_open.load() || g_wait_release.load(); }

void OnlineOverlayPad(uint16_t buttons) { g_guest_pad = buttons; }

}  // namespace svr2011

// The game's INVITE FRIENDS (the lobby's X): XamShowGameInviteUI (its import
// thunk) opens the overlay instead of the Guide's invite screen.
REX_HOOK_RAW(sub_82904DB0) {
  (void)base;
  if (!svr2011::OnlineOverlayHoldsInput()) {
    svr2011::g_from_game = true;
    if (svr2011::g_kernel) svr2011::g_kernel->BroadcastNotification(svr2011::kSystemUi, 1);
    svr2011::ToggleOnlineOverlay();
  }
  ctx.r3.u64 = 0;  // ERROR_SUCCESS
}

// XInviteGetAcceptedInfo(user, XINVITE_INFO*) (the game's; it asks XLIVEBASE
// 0x58023): an invite accepted on the overlay - the inviter, this player, the
// title, the session (XSESSION_INFO) and "from a game invite".
REX_HOOK_RAW(sub_8298BA80) {
  std::optional<svr2011::Accepted> a;
  {
    std::lock_guard lock(svr2011::g_mutex);
    if (svr2011::g_accepted && svr2011::Clock::now() - svr2011::g_accepted->at > std::chrono::minutes(5))
      svr2011::g_accepted.reset();
    a = svr2011::g_accepted;
  }
  if (!a || !ctx.r4.u32) {
    __imp__sub_8298BA80(ctx, base);
    return;
  }
  uint8_t* out = base + ctx.r4.u32;
  std::memset(out, 0, 84);
  svr2011::Put64(out, a->inviter);
  svr2011::Put64(out + 8, std::strtoull(rex::cvar::Query<std::string>("online_xuid").c_str(), nullptr, 16));
  svr2011::Put32(out + 16, svr2011::kTitleId);
  std::memcpy(out + 20, a->info, 60);
  svr2011::Put32(out + 80, 1);
  REXLOG_INFO("online overlay: the game took the invite (XInviteGetAcceptedInfo)");
  ctx.r3.u64 = 0;
}

// The game launching a title (sub_8215ACE8: XamLoaderLaunchTitle(path, flags)):
// with a path - itself, to join an invite from its boot - a new copy of the
// game is started first (the launch data and the invite saved for it).
REX_HOOK_RAW(sub_8215ACE8) {
  bool invite = false;
  {
    std::lock_guard lock(svr2011::g_mutex);
    invite = svr2011::g_accepted.has_value();
  }
  if (ctx.r3.u32 && invite && REXCVAR_GET(online_invite_native)) {
    const char* path = reinterpret_cast<const char*>(base + ctx.r3.u32);
    const bool saved = svr2011::SaveRelaunch();
    const bool started = saved && svr2011::StartNewCopy();
    REXLOG_INFO("online overlay: the game relaunches itself ({}): {}", path,
                started ? "a new copy started" : saved ? "couldn't start a new copy" : "couldn't save its launch data");
  }
  __imp__sub_8215ACE8(ctx, base);
}

// XamShowSigninUI (its import thunk). The SDK's says "sign-in changed"
// (XN_SYS_SIGNINCHANGED) every time; booting into an invite, the game takes
// that as the invited player signing out ("returned to the Title Screen
// because a sign-in change has occurred"). For a few minutes after an
// invite's relaunch the player is simply still signed in: the UI closes.
REX_HOOK_RAW(sub_82904D30) {
  bool joining = false;
  {
    std::lock_guard lock(svr2011::g_mutex);
    joining = svr2011::g_accepted && svr2011::g_relaunched_at != svr2011::Clock::time_point{} &&
              svr2011::Clock::now() - svr2011::g_relaunched_at < std::chrono::minutes(5);
  }
  if (!joining) {
    __imp__sub_82904D30(ctx, base);
    return;
  }
  REXLOG_INFO("online overlay: sign-in UI while joining an invite: still signed in");
  if (svr2011::g_kernel) svr2011::g_kernel->BroadcastNotification(svr2011::kSystemUi, 0);
  ctx.r3.u64 = 0;  // ERROR_SUCCESS
}

// XamUserGetSigninState (its import thunk), asked once at boot by the game's
// launch-data check (sub_82574760: an invite relaunch - launch data word 0 =
// 2, word 2 = the player): only "not signed in" (0) lets it go on with the
// invite (it then waits for that player's sign-in); signed in already, it
// says "returned to the Title Screen because a sign-in change has occurred".
REX_HOOK_RAW(sub_82904AA8) {
  bool joining = false;
  if (ctx.lr == 0x8257478C) {  // (the check's call)
    std::lock_guard lock(svr2011::g_mutex);
    joining = svr2011::g_accepted && svr2011::g_relaunched_at != svr2011::Clock::time_point{};
  }
  if (!joining) {
    __imp__sub_82904AA8(ctx, base);
    return;
  }
  REXLOG_INFO("online overlay: the game's invite check at boot (user {})", ctx.r3.u32);
  ctx.r3.u64 = 0;
}

