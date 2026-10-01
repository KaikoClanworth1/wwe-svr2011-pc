// Paint Tool pages - see paint_pages.h.
//
// Game side (docs/PAINT_TOOL_PAGES_PLAN.md):
//   Paint Tool file (.pt): 0x785FFC bytes, u32 magic 0x02A78E9A, u32 version
//   3, 20 slots of 0x604CC bytes at 8 + k * 0x604CC (slot +28 used flag), a
//   checksum per slot (slot +0x604C8, sub_827B3118) and the sum of them all
//   at the end (sub_827B35C8).
//   Storage jobs on it: sub_824AE6E8(st, 1, ...) open, sub_824AE830(st, 1,
//   buf, size) read, sub_82517CF8(st, 1, buf, size) write, sub_824AE790(st,
//   ...) close; only Paint Tool code calls them (the Paint Tool itself, the
//   Created Superstar logo list, the logo popup, Community Creations). The
//   caller polls st+32 (done), then st+36 (succeeded) / st+40 / st+44 (error).
//   Paint Tool manager (PTM) = sub_8286E4B8(*0x82EDE67C): +28 state (0 idle;
//   34 rebuilds the 20 thumbnails, 35 waits for them), +104 the file buffer
//   (0x785FFC bytes of file, then a scratch slot and u32 dirty[20]).
//   Grid menu (sub_827B4600 updates it): +776 state (0 idle; 8 waits for the
//   thumbnails, then redraws the cells), +56 row, +72 column.
//   Created Superstar logo list (sub_828D1BF8()): loaded by sub_828DC1A8(list,
//   42) and the steps of sub_828DBD90(list) (+16 step, 7 = done; it reads the
//   whole file through the jobs above), +28 logo count. Its picker (HEAD /
//   BODY -> TATTOOS -> PAINT TOOL DATA) takes the list's logos when it opens
//   (sub_827E7100(picker)); the picker's cursor widget is picker+28, whose
//   input sub_823D4FC0 runs while the picker has the controller.
#include "paint_pages.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/hook.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"

namespace {

constexpr uint32_t kSlots = 20, kSlot = 0x604CC, kFile = 0x785FFC;
constexpr uint32_t kMagic = 0x02A78E9A, kVersion = 3;
constexpr uint32_t kUsed = 28, kSum = 0x604C8;  // slot-relative
constexpr uint32_t kContainer = 0x82EDE67C;     // -> sub_8286E4B8 -> PTM
constexpr uint32_t kPtmState = 28, kPtmFile = 104;
constexpr uint32_t kMenuState = 776;
constexpr uint32_t kPtmRebuildThumbs = 34, kMenuRefresh = 8;
constexpr uint32_t kStDone = 32, kStOk = 36, kStFail = 40, kStError = 44, kStExtra = 52;

rex::memory::Memory* g_memory = nullptr;
rex::input::InputSystem* g_input = nullptr;
std::filesystem::path g_dir;  // Saves\.paint
std::filesystem::path g_pt;   // Saves\00PaintTool.pt

std::atomic<int> g_page{0};
std::mutex g_mutex;
std::set<uint32_t> g_faked;          // storage objects with a job done here
std::vector<uint8_t> g_page1;        // page 1 as it was when the grid left it
bool g_page1_valid = false;
uint16_t g_prev_buttons = 0;
std::atomic<int64_t> g_grid_seen{0};    // ms; the grid's last update (label)
std::atomic<int64_t> g_picker_seen{0};  // ms; the picker's last input (label)
uint32_t g_picker = 0, g_picker_vtable = 0;  // the open Superstar logo picker
uint32_t g_picker_list = 0;                  // its list while it reloads
uint16_t g_picker_prev_buttons = 0;

uint32_t Rd32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void Wr32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The game's slot checksum (sub_827B3118; launcher pt_slot_sum): every field
// of the slot added as the type the game reads it as. `b` = the slot's start
// minus 8 (offsets as the launcher counts them).
uint32_t SlotSum(const uint8_t* b) {
  constexpr uint32_t kCanvas = 52, kTga = 0x40034, kDds = 0x50448, kStamp = 0x604C8;
  const uint8_t *t = b + kTga, *d = b + kDds, *e = b + kStamp;
  static const int words[] = {8, 12, 16, 28, 32, 36, 40, 44, 48};
  static const int tb[] = {0, 1, 2, 7, 16, 17}, th[] = {3, 5, 8, 10, 12, 14, 18};
  uint32_t s = 0;
  for (int i = 20; i < 28; i++) s += uint32_t(int32_t(int8_t(b[i])));
  for (int w : words) s += Rd32(b + w);
  for (int i = 0; i < 256 * 256; i++) s += Rd32(b + kCanvas + 4 * i);
  for (int i : tb) s += t[i];
  for (int i : th) s += Rd16(t + i);
  for (int i = 0; i < 256; i++) s += Rd32(t + 20 + 4 * i);
  for (int i = 0; i < 256 * 256; i++) s += t[0x414 + i];
  for (int i = 0; i < 32; i++) s += Rd32(d + 4 * i);
  for (int j = 0; j < 2048; j++) {
    const uint8_t* q = d + 128 + 32 * j;
    s += Rd16(q) + Rd16(q + 2) + Rd32(q + 4) + Rd16(q + 8) + Rd16(q + 10) + Rd32(q + 12) +
         Rd16(q + 16) + Rd16(q + 18) + Rd32(q + 20) + Rd16(q + 24) + Rd16(q + 26) + Rd32(q + 28);
  }
  s += Rd16(e);
  for (int i = 2; i < 8; i++) s += e[i];
  return s;
}

uint8_t* Slot(uint8_t* f, uint32_t k) { return f + 8 + k * kSlot; }

std::filesystem::path SlotFile(int page, uint32_t k) {
  char name[32];
  std::snprintf(name, sizeof(name), "p%02d_s%02u.bin", page + 1, k + 1);
  return g_dir / name;
}

// Builds page `page` (1..9) as a complete, valid Paint Tool file at `f`.
void BuildPage(int page, uint8_t* f) {
  std::memset(f, 0, kFile);
  Wr32(f, kMagic);
  Wr32(f + 4, kVersion);
  uint32_t total = kMagic + kVersion, used = 0;
  for (uint32_t k = 0; k < kSlots; ++k) {
    uint8_t* s = Slot(f, k);
    std::ifstream in(SlotFile(page, k), std::ios::binary);
    if (in && in.read(reinterpret_cast<char*>(s), kSlot) && Rd32(s + kUsed)) {
      ++used;
    } else {
      // An empty slot as the game makes one.
      std::memset(s, 0, kSlot);
      s[3] = 3;
      s[12] = 1;
    }
    const uint32_t sum = SlotSum(s - 8);
    Wr32(s + kSum, sum);
    total += sum;
  }
  Wr32(f + kFile - 4, total);
  REXLOG_INFO("paint pages: page {} loaded ({} logos)", page + 1, used);
}

// Stores page `page` (1..9) from a Paint Tool file image.
void SavePage(int page, const uint8_t* f) {
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  uint32_t used = 0;
  for (uint32_t k = 0; k < kSlots; ++k) {
    const uint8_t* s = f + 8 + k * kSlot;
    const auto file = SlotFile(page, k);
    if (!Rd32(s + kUsed)) {
      std::filesystem::remove(file, ec);
      continue;
    }
    ++used;
    const auto tmp = file.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary);
      out.write(reinterpret_cast<const char*>(s), kSlot);
      if (!out) {
        REXLOG_WARN("paint pages: could not write {}", tmp);
        continue;
      }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) REXLOG_WARN("paint pages: could not write {}: {}", file.string(), ec.message());
  }
  REXLOG_INFO("paint pages: page {} saved ({} logos)", page + 1, used);
}

// Page 1 into `f`: the copy taken when the grid left it, else the file.
bool LoadPage1(uint8_t* f) {
  if (g_page1_valid && g_page1.size() == kFile) {
    std::memcpy(f, g_page1.data(), kFile);
    return true;
  }
  std::ifstream in(g_pt, std::ios::binary);
  std::vector<uint8_t> data(kFile);
  if (!in.read(reinterpret_cast<char*>(data.data()), kFile) || Rd32(data.data()) != kMagic) {
    REXLOG_WARN("paint pages: could not read {}", g_pt.string());
    return false;
  }
  std::memcpy(f, data.data(), kFile);
  return true;
}

// A storage job finished here: done and succeeded, as the game's own jobs
// report it.
void Complete(uint8_t* base, uint32_t st) {
  Wr32(base + st + kStError, 0);
  Wr32(base + st + kStFail, 0);
  Wr32(base + st + kStExtra, 0);
  Wr32(base + st + kStOk, 1);
  Wr32(base + st + kStDone, 1);
}

uint32_t Ptm(PPCContext& ctx, uint8_t* base) {
  const uint32_t container = Rd32(base + kContainer);
  if (!container) return 0;
  const auto saved = ctx;
  ctx.r3.u64 = container;
  sub_8286E4B8(ctx, base);
  const uint32_t ptm = ctx.r3.u32;
  ctx = saved;
  return ptm;
}

bool AnyDirty(PPCContext& ctx, uint8_t* base, uint32_t file) {
  const auto saved = ctx;
  ctx.r3.u64 = file;
  sub_827B2F78(ctx, base);
  const bool dirty = ctx.r3.u32 != 0;
  ctx = saved;
  return dirty;
}

// The buttons held on any controller (the game reads them the same way).
uint16_t PadButtons() {
  uint16_t buttons = 0;
  for (uint32_t user = 0; g_input && user < 4; ++user) {
    rex::input::X_INPUT_STATE s = {};
    if (g_input->GetState(user, &s) == 0) buttons |= s.gamepad.buttons;
  }
  return buttons;
}

// Saves the page the grid shows now (copies and deletes in the grid are only
// saved when the player leaves the Paint Tool): the game's checksums
// (sub_827B35C8), then the page's store - page 1 is the game's own file,
// written as the launcher's Paint Tool tab writes it - and no slot left dirty
// (sub_827B3028).
bool SaveNow(PPCContext& ctx, uint8_t* base, uint32_t file) {
  const auto saved = ctx;
  ctx.r3.u64 = file;
  ctx.r4.u64 = 1;
  sub_827B35C8(ctx, base);
  ctx = saved;
  const uint8_t* f = base + file;
  if (g_page.load() != 0) {
    SavePage(g_page.load(), f);
  } else {
    const auto tmp = g_pt.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary);
      out.write(reinterpret_cast<const char*>(f), kFile);
      if (!out) {
        REXLOG_WARN("paint pages: could not write {}", tmp);
        return false;
      }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, g_pt, ec);
    if (ec) {
      REXLOG_WARN("paint pages: could not write {}: {}", g_pt.string(), ec.message());
      return false;
    }
    REXLOG_INFO("paint pages: page 1 saved");
  }
  for (uint32_t k = 0; k < kSlots; ++k) {
    ctx.r3.u64 = file;
    ctx.r4.u64 = k;
    ctx.r5.u64 = 0;
    sub_827B3028(ctx, base);
    ctx = saved;
  }
  return true;
}

// LB / RB in the idle grid: shows the previous / next page.
void SwitchPage(PPCContext& ctx, uint8_t* base, uint32_t menu, int delta) {
  const uint32_t ptm = Ptm(ctx, base);
  const uint32_t state = ptm ? Rd32(base + ptm + kPtmState) : 0xFFFFFFFF;
  const uint32_t file = ptm ? Rd32(base + ptm + kPtmFile) : 0;
  const uint32_t magic = file ? Rd32(base + file) : 0;
  const bool dirty = file && AnyDirty(ctx, base, file);
  if (!ptm || state != 0 || magic != kMagic) {
    REXLOG_INFO("paint pages: no page switch now (manager {:08X} state {} file {:08X} {:08X})", ptm, state,
                file, magic);
    return;
  }
  if (dirty) {
    std::lock_guard lock(g_mutex);
    if (!SaveNow(ctx, base, file)) return;
  }
  const int from = g_page.load(), to = (from + delta + svr2011::kPaintPages) % svr2011::kPaintPages;
  uint8_t* f = base + file;
  {
    std::lock_guard lock(g_mutex);
    if (from == 0) {
      g_page1.assign(f, f + kFile);
      g_page1_valid = true;
    }
    if (to == 0) {
      if (!LoadPage1(f)) return;
    } else {
      BuildPage(to, f);
    }
  }
  g_page = to;
  // The game's own refresh: rebuild the 20 thumbnails, then the cells.
  Wr32(base + ptm + kPtmState, kPtmRebuildThumbs);
  Wr32(base + menu + kMenuState, kMenuRefresh);
  REXLOG_INFO("paint pages: page {} of {}", to + 1, svr2011::kPaintPages);
}

bool OurJob(PPCContext& ctx) { return g_page.load() != 0 && ctx.r4.u32 == 1; }

// Whether page `page` has any logo (without loading it).
bool PageHasLogos(int page) {
  std::error_code ec;
  if (page != 0) {
    for (uint32_t k = 0; k < kSlots; ++k)
      if (std::filesystem::exists(SlotFile(page, k), ec)) return true;
    return false;
  }
  if (g_page1_valid && g_page1.size() == kFile) {
    for (uint32_t k = 0; k < kSlots; ++k)
      if (Rd32(g_page1.data() + 8 + k * kSlot + kUsed)) return true;
    return false;
  }
  std::ifstream in(g_pt, std::ios::binary);
  for (uint32_t k = 0; in && k < kSlots; ++k) {
    uint8_t used[4] = {};
    in.seekg(8 + k * kSlot + kUsed);
    if (in.read(reinterpret_cast<char*>(used), 4) && Rd32(used)) return true;
  }
  return false;
}

uint32_t CallR3(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t r3,
                uint32_t r4 = 0) {
  const auto saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  fn(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

}  // namespace

namespace svr2011 {

int PaintPage() { return g_page.load(); }

void InstallPaintPages(rex::memory::Memory* memory, rex::input::InputSystem* input,
                       const std::filesystem::path& saves) {
  g_memory = memory;
  g_input = input;
  g_dir = saves / ".paint";
  g_pt = saves / "00PaintTool.pt";
  std::error_code ec;
  std::filesystem::create_directories(g_dir, ec);
  REXLOG_INFO("paint pages: {} pages of {} Paint Tool logos (pages 2-{} in {})", kPaintPages, kSlots,
              kPaintPages, g_dir.string());
}

}  // namespace svr2011

// -- Storage jobs on pages 2-10 ----------------------------------------------

// Open: sub_824AE6E8(st, 1, ...).
REX_EXTERN(__imp__sub_824AE6E8);
REX_HOOK_RAW(sub_824AE6E8) {
  if (!OurJob(ctx)) {
    {
      std::lock_guard lock(g_mutex);
      if (ctx.r4.u32 == 1) g_page1_valid = false;  // page 1 may change
    }
    __imp__sub_824AE6E8(ctx, base);
    return;
  }
  const uint32_t st = ctx.r3.u32;
  {
    std::lock_guard lock(g_mutex);
    g_faked.insert(st);
  }
  Complete(base, st);
}

// Read: sub_824AE830(st, 1, buf, size).
REX_EXTERN(__imp__sub_824AE830);
REX_HOOK_RAW(sub_824AE830) {
  const uint32_t st = ctx.r3.u32, buf = ctx.r5.u32, size = ctx.r6.u32;
  if (!OurJob(ctx) || size != kFile) {
    __imp__sub_824AE830(ctx, base);
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    BuildPage(g_page.load(), base + buf);
  }
  Complete(base, st);
}

// Write: sub_82517CF8(st, 1, buf, size).
REX_EXTERN(__imp__sub_82517CF8);
REX_HOOK_RAW(sub_82517CF8) {
  const uint32_t st = ctx.r3.u32, buf = ctx.r5.u32, size = ctx.r6.u32;
  if (!OurJob(ctx) || size != kFile) {
    __imp__sub_82517CF8(ctx, base);
    return;
  }
  {
    std::lock_guard lock(g_mutex);
    SavePage(g_page.load(), base + buf);
  }
  Complete(base, st);
}

// Close: sub_824AE790(st, ...) - ours if the open was.
REX_EXTERN(__imp__sub_824AE790);
REX_HOOK_RAW(sub_824AE790) {
  const uint32_t st = ctx.r3.u32;
  bool ours;
  {
    std::lock_guard lock(g_mutex);
    ours = g_faked.erase(st) != 0;
  }
  if (!ours) {
    __imp__sub_824AE790(ctx, base);
    return;
  }
  Complete(base, st);
}

// -- The grid ---------------------------------------------------------------

// Grid menu update: sub_827B4600(menu).
REX_EXTERN(__imp__sub_827B4600);
REX_HOOK_RAW(sub_827B4600) {
  const uint32_t menu = ctx.r3.u32;
  const uint16_t buttons = PadButtons();
  const uint16_t pressed = uint16_t(buttons & ~g_prev_buttons);
  g_prev_buttons = buttons;
  if (Rd32(base + menu + kMenuState) == 0) {
    g_grid_seen = NowMs();
    using namespace rex::input;
    if (pressed & X_INPUT_GAMEPAD_LEFT_SHOULDER) SwitchPage(ctx, base, menu, -1);
    else if (pressed & X_INPUT_GAMEPAD_RIGHT_SHOULDER) SwitchPage(ctx, base, menu, +1);
  }
  __imp__sub_827B4600(ctx, base);
}

// -- The Created Superstar logo picker --------------------------------------

// The picker opens (and takes the list's logos): sub_827E7100(picker).
REX_EXTERN(__imp__sub_827E7100);
REX_HOOK_RAW(sub_827E7100) {
  g_picker = ctx.r3.u32;
  g_picker_vtable = Rd32(base + g_picker);
  __imp__sub_827E7100(ctx, base);
}

// The picker's cursor input: LB / RB load the previous / next page that has
// logos into the list (the game's own loader), then the picker opens again.
REX_EXTERN(__imp__sub_823D4FC0);
REX_HOOK_RAW(sub_823D4FC0) {
  const uint32_t widget = ctx.r3.u32;
  const uint32_t picker = widget - 28;
  if (!g_picker || picker != g_picker || Rd32(base + picker) != g_picker_vtable) {
    __imp__sub_823D4FC0(ctx, base);
    return;
  }
  g_picker_seen = NowMs();
  if (g_picker_list) {
    // Reloading: run the loader's steps until it is done, then reopen.
    CallR3(ctx, base, sub_828DBD90, g_picker_list);
    if (Rd32(base + g_picker_list + 16) == 7) {
      REXLOG_INFO("paint pages: Superstar logo list holds page {} ({} logos)", g_page.load() + 1,
                  Rd32(base + g_picker_list + 28));
      g_picker_list = 0;
      CallR3(ctx, base, sub_827E7100, picker);
    }
    return;  // no cursor moves meanwhile
  }
  const uint16_t buttons = PadButtons();
  const uint16_t pressed = uint16_t(buttons & ~g_picker_prev_buttons);
  g_picker_prev_buttons = buttons;
  using namespace rex::input;
  const int delta = (pressed & X_INPUT_GAMEPAD_LEFT_SHOULDER)    ? -1
                    : (pressed & X_INPUT_GAMEPAD_RIGHT_SHOULDER) ? +1
                                                                 : 0;
  if (delta) {
    const uint32_t list = CallR3(ctx, base, sub_828D1BF8, 0);
    if (list && Rd32(base + list + 16) == 7) {
      int to = g_page.load();
      for (int i = 0; i < svr2011::kPaintPages; ++i) {
        to = (to + delta + svr2011::kPaintPages) % svr2011::kPaintPages;
        if (to == g_page.load() || PageHasLogos(to)) break;
      }
      if (to != g_page.load()) {
        g_page = to;
        CallR3(ctx, base, sub_828DC1A8, list, 42);
        g_picker_list = list;
        REXLOG_INFO("paint pages: Superstar logo picker loads page {}", to + 1);
        return;
      }
    }
  }
  __imp__sub_823D4FC0(ctx, base);
}

// -- The page label ---------------------------------------------------------

namespace {

class PageLabel final : public rex::ui::ImGuiDialog {
 public:
  explicit PageLabel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const int64_t now = NowMs();
    const bool grid = now - g_grid_seen.load() < 250, picker = now - g_picker_seen.load() < 250;
    if (!grid && !picker) return;
    // The game's 16:9 picture, centred in the window; the label sits on the
    // top edge of the grid, centred, or right of the picker's title.
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    const float gw = std::min(w, h * 16.0f / 9.0f), gh = gw * 9.0f / 16.0f;
    const float x0 = (w - gw) * 0.5f, y0 = (h - gh) * 0.5f;
    const float scale = gh / 720.0f;
    char text[48];
    std::snprintf(text, sizeof(text), "LB   PAGE %d / %d   RB", g_page.load() + 1, svr2011::kPaintPages);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const float size = 28.0f * scale;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 c = grid ? ImVec2(x0 + gw * 0.5f, y0 + gh * 0.088f)
                          : ImVec2(x0 + gw * 0.375f + ts.x * 0.5f, y0 + gh * 0.151f);
    const ImVec2 pad(14.0f * scale, 6.0f * scale);
    const ImVec2 a(c.x - ts.x * 0.5f - pad.x, c.y - ts.y * 0.5f - pad.y);
    const ImVec2 b(c.x + ts.x * 0.5f + pad.x, c.y + ts.y * 0.5f + pad.y);
    dl->AddRectFilled(a, b, IM_COL32(10, 12, 18, 210), 6.0f * scale);
    dl->AddRect(a, b, IM_COL32(220, 220, 225, 255), 6.0f * scale, 0, 2.0f * scale);
    dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), text);
  }
};

}  // namespace

namespace svr2011 {

void InstallPaintPagesOverlay(rex::ui::ImGuiDrawer* drawer) { new PageLabel(drawer); }

}  // namespace svr2011
