// WWE SmackDown vs. Raw 2011 - CPU players in online Player Matches.
//
// Netplay is lockstep with only the humans' pads exchanged; CPU characters
// are left out of the exchange and the AI runs the same on every machine
// (its random numbers are seeded the same online). So a CPU needs only the
// SETUP to be the same everywhere. docs/ONLINE_CPU.md.
//
// The Player Match lobby ("OMSL": constructor sub_824E41F8, per frame
// sub_824E6510, state at screen+1168, the cursor's slot at screen+1060) keeps
// its slots in L = **0x82EDE924: 280-byte records at L+204+j*280 (+0 state,
// +4 net player, +12 name, +136 superstar id, +140 attire, +160.. 120 bytes
// the game unpacks for the panel: +172 the id, +174 / +242 the superstar's
// name), L+24 joined, L+28 slots. Clients copy a record whole when the host
// sends it (sub_828AC700(L, j, 0)).
//
// Here, the host presses Y on a VACANT slot (Y's own action - sub_824E4C30 -
// does nothing on the host): the record becomes a CPU - state 26 (past every
// stage, nobody can claim it), net player 0xFFFFFFFE (the tag), name "CPU",
// a superstar nobody else picked - and goes to every client. Y on it again:
// vacant again. Then on every machine:
//  - the start checks (sub_828AC668: joined == slots and every slot at the
//    stage) count the CPU slots as joined;
//  - the host's wait for every superstar block to be acknowledged
//    (sub_828AD2F8) counts the CPU slots' bits as set;
//  - the match slot made from the CPU's lobby slot (sub_824F6AE0(obj,
//    match slot, lobby slot)) gets controller CPU (slot[-8] = 1).
//
// Test aid: SVR2011_TEST_LOBBY_LOG=1 logs the records, picks and match slots.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "online.h"
#include "online_overlay.h"

REXCVAR_DEFINE_BOOL(online_cpu_slots, false, "Online",
                    "CPU players in online Player Matches: the host's Y on a vacant lobby slot (in progress)");

namespace {

constexpr uint32_t kLobbyPtr = 0x82EDE924;  // -> -> L
constexpr uint32_t kSlots = 204, kSlotSize = 280;
constexpr uint32_t kBlocks = 3568, kBlockSize = 15044;  // the slots' superstar blocks (+3564: acknowledged mask)
constexpr uint32_t kCpuState = 26, kCpuTag = 0xFFFFFFFE;
constexpr uint32_t kMatch = 0x82EDEB48;  // -> the match setup: slots at +432, 2116 bytes each
constexpr uint32_t kMatchSlots = 432, kMatchSlotSize = 2116;

// Superstars a CPU can be (the stock roster's: every game has them).
struct Star {
  uint16_t id;
  const char* name;
};
constexpr Star kStars[] = {
    {102, "TRIPLE H"}, {103, "UNDERTAKER"}, {104, "CHRIS JERICHO"}, {107, "KANE"},     {110, "EDGE"},
    {123, "REY MYSTERIO"}, {125, "BIG SHOW"}, {131, "CM PUNK"},       {139, "JOHN CENA"}, {145, "SHAWN MICHAELS"},
    {160, "BATISTA"}, {161, "RANDY ORTON"},
};

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

bool LogOn() {
  static const bool on = std::getenv("SVR2011_TEST_LOBBY_LOG") != nullptr;
  return on;
}

uint32_t Lobby(uint8_t* base) {
  const uint32_t p = Rd32(base + kLobbyPtr);
  return p ? Rd32(base + p) : 0;
}

std::string Hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; ++i) s += d[p[i] >> 4], s += d[p[i] & 15], s += (i % 4 == 3 ? " " : "");
  return s;
}

uint8_t* Record(uint8_t* base, uint32_t L, uint32_t j) { return base + L + kSlots + j * kSlotSize; }
bool IsCpu(const uint8_t* rec) { return Rd32(rec + 4) == kCpuTag && Rd32(rec) == kCpuState; }

uint32_t SlotCount(uint8_t* base, uint32_t L) { return std::min<uint32_t>(Rd32(base + L + 28), 6); }

int CpuCount(uint8_t* base, uint32_t L) {
  int n = 0;
  for (uint32_t j = 0; j < SlotCount(base, L); ++j) n += IsCpu(Record(base, L, j));
  return n;
}

bool IsHost(PPCContext& ctx, uint8_t* base) {
  PPCContext c = ctx;
  sub_82573F18(c, base);
  return c.r3.u32 && Rd32(base + c.r3.u32 + 20) != 0;
}

uint8_t g_vacant[6][kSlotSize];

// The lobby's help bar (online.cpp's bar hook asks): Y's label for the
// cursor's slot (0 none, 1 ADD CPU, 2 REMOVE CPU) and whether the host is
// alone with CPUs (WAITING FOR ONE MORE PLAYER); kept fresh by the lobby's frame.
std::atomic<int> g_help_y{0};
std::atomic<bool> g_help_waiting{false};
std::atomic<int64_t> g_help_at{0};
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}  // a slot's record before it became a CPU (Y again: back to it)

// The lobby record j changed: send it to everyone (and the panel into the
// superstar block, unless the block was built and the panel made from it).
void SendRecord(PPCContext& ctx, uint8_t* base, uint32_t L, uint32_t j, bool panel_to_block = true) {
  PPCContext c = ctx;
  if (panel_to_block) {
    c.r3.u64 = L + kSlots + j * kSlotSize + 160, c.r4.u64 = L + kBlocks + j * kBlockSize;
    sub_828A5408(c, base);
    c = ctx;
  }
  c.r3.u64 = L, c.r4.u64 = j, c.r5.u64 = 0;
  sub_828AC700(c, base);
}

// The CPU's superstar block, built as the select screen builds a human's
// (sub_824C4F58) from the stock superstar's data (as an offline CPU's,
// sub_828B5BC0): online, every machine loads each person from the lobby's
// block. Then the panel is made from it (real stats). False: no scratch memory.
bool BuildBlock(PPCContext& ctx, uint8_t* base, uint32_t L, uint32_t j, uint32_t id) {
  const uint32_t tmp = svr2011::OnlineScratch();
  if (!tmp) return false;
  const uint32_t blk = L + kBlocks + j * kBlockSize, args = tmp + 1952 + 32;
  std::memset(base + blk, 0, kBlockSize - 4);
  PPCContext c = ctx;
  c.r3.u64 = blk;
  sub_824C4F08(c, base);
  std::memset(base + tmp, 0, 1952);
  Wr32(base + args, id), Wr32(base + args + 4, 1), Wr32(base + args + 8, 0);
  c = ctx;
  c.r3.u64 = tmp, c.r4.u64 = args;
  sub_828B5BC0(c, base);
  std::memcpy(base + blk, base + tmp, 1724);
  std::memcpy(base + blk + 14816, base + tmp + 1728, 224);
  c = ctx;
  c.r3.u64 = L + kSlots + j * kSlotSize + 160, c.r4.u64 = blk;
  sub_828A5380(c, base);
  if (LogOn()) REXLOG_INFO("online cpu: slot {} block built: {}", j, Hex(base + blk, 64));
  return true;
}

void MakeCpu(PPCContext& ctx, uint8_t* base, uint32_t L, uint32_t j) {
  uint8_t* rec = Record(base, L, j);
  std::memcpy(g_vacant[j], rec, kSlotSize);
  // a superstar nobody in the lobby has
  std::vector<const Star*> free;
  for (const Star& s : kStars) {
    bool taken = false;
    for (uint32_t k = 0; k < SlotCount(base, L); ++k)
      if (k != j && Rd32(Record(base, L, k)) >= 8 && Rd32(Record(base, L, k) + 136) == s.id) taken = true;
    if (!taken) free.push_back(&s);
  }
  static std::mt19937 rng{std::random_device{}()};
  const Star& s = *free[free.empty() ? 0 : rng() % free.size()];
  Wr32(rec, kCpuState);
  Wr32(rec + 4, kCpuTag);
  std::memset(rec + 12, 0, 16);
  std::memcpy(rec + 12, "CPU", 3);
  Wr32(rec + 136, s.id);
  Wr32(rec + 140, 0);  // (the first attire)
  std::memset(rec + 160, 0, 120);
  rec[172] = uint8_t(s.id >> 8), rec[173] = uint8_t(s.id);
  std::strncpy(reinterpret_cast<char*>(rec + 174), s.name, 40);
  std::strncpy(reinterpret_cast<char*>(rec + 242), s.name, 36);
  REXLOG_INFO("online cpu: slot {} is a CPU - {} (id {})", j, s.name, s.id);
  const bool built = BuildBlock(ctx, base, L, j, s.id);
  SendRecord(ctx, base, L, j, !built);
}

void MakeVacant(PPCContext& ctx, uint8_t* base, uint32_t L, uint32_t j) {
  uint8_t* rec = Record(base, L, j);
  std::memcpy(rec, g_vacant[j], kSlotSize);
  if (IsCpu(rec)) Wr32(rec, 0), Wr32(rec + 4, 0xFFFFFFFF);  // (never a CPU again)
  REXLOG_INFO("online cpu: slot {} vacant again", j);
  SendRecord(ctx, base, L, j);
}

}  // namespace

// The Player Match lobby's frame (sub_824E6510(screen)). Y pressed (pad 1's
// own buttons: the lobby's Y listener isn't called on the host) on the host
// with the cursor on a vacant slot: a CPU there; on a CPU: vacant again.
REX_EXTERN(__imp__sub_824E6510);
REX_HOOK_RAW(sub_824E6510) {
  const uint32_t screen = ctx.r3.u32;
  const uint32_t L = Lobby(base);
  if (screen && L) {
    const uint32_t state = Rd32(base + screen + 1168), j = Rd32(base + screen + 1060);
    {
      int y = 0;
      const bool host = REXCVAR_GET(online_cpu_slots) && (state == 11 || state == 22) && j < SlotCount(base, L) && IsHost(ctx, base);
      if (host) y = IsCpu(Record(base, L, j)) ? 2 : Rd32(Record(base, L, j)) < 5 ? 1 : 0;
      g_help_y = y;
      g_help_waiting = host && Rd32(base + L + 24) < 2 && CpuCount(base, L) > 0;
      g_help_at = NowMs();
    }
    static uint32_t last_state = 0xFFFFFFFF;
    if (LogOn() && state != last_state) REXLOG_INFO("online cpu: lobby state {} (cursor slot {})", state, j);
    last_state = state;
    static bool y_down = false;
    const bool y = (svr2011::OnlineRawPad() & 0x8000) != 0;  // (XINPUT_GAMEPAD_Y)
    const bool pressed = y && !y_down;
    y_down = y;
    if (pressed && REXCVAR_GET(online_cpu_slots)) {
      if (LogOn()) REXLOG_INFO("online cpu: Y (lobby state {}, cursor slot {})", state, j);
      if ((state == 11 || state == 22) && j < SlotCount(base, L) && IsHost(ctx, base)) {
        uint8_t* rec = Record(base, L, j);
        if (IsCpu(rec)) MakeVacant(ctx, base, L, j);
        else if (Rd32(rec) < 5) MakeCpu(ctx, base, L, j);
      }
    }
  }
  __imp__sub_824E6510(ctx, base);
}

// The lobby's start checks (sub_828AC668(L, stage): every slot joined and at
// the stage): the CPU slots count as joined.
REX_EXTERN(__imp__sub_828AC668);
REX_HOOK_RAW(sub_828AC668) {
  const uint32_t L = ctx.r3.u32;
  // (only with 2 humans or more: the network library won't start a match
  // with fewer consoles - a host alone with CPUs waits for a player)
  const int cpus = L && Rd32(base + L + 24) >= 2 ? CpuCount(base, L) : 0;
  if (!cpus) {
    __imp__sub_828AC668(ctx, base);
    return;
  }
  const uint32_t joined = Rd32(base + L + 24);
  Wr32(base + L + 24, joined + cpus);
  const uint32_t stage = ctx.r4.u32;
  __imp__sub_828AC668(ctx, base);
  Wr32(base + L + 24, joined);
  if (LogOn()) {
    static uint32_t last = 99;
    if (ctx.r3.u32 != last || stage != 0) {
      REXLOG_INFO("online cpu: start check stage {} with {} CPU(s) -> {}", stage, cpus, ctx.r3.u32);
      last = ctx.r3.u32;
    }
  }
}

// The host's wait for every superstar block to be acknowledged by every
// machine (sub_828AD2F8(L): each block's mask at L+3564+k*15044 needs a bit
// per slot): the CPU slots' bits are set.
REX_EXTERN(__imp__sub_828AD2F8);
REX_HOOK_RAW(sub_828AD2F8) {
  const uint32_t L = ctx.r3.u32;
  if (L) {
    uint32_t bits = 0;
    for (uint32_t j = 0; j < SlotCount(base, L); ++j)
      if (IsCpu(Record(base, L, j))) bits |= 1u << j;
    if (bits)
      for (uint32_t k = 0; k < SlotCount(base, L); ++k) {
        uint8_t* mask = base + L + kBlocks - 4 + k * kBlockSize;
        Wr32(mask, Rd32(mask) | bits);
      }
  }
  __imp__sub_828AD2F8(ctx, base);
}

// A match slot made from a lobby slot (sub_824F6AE0(obj, match slot, lobby
// slot), on every machine): a CPU's is controlled by the CPU.
REX_EXTERN(__imp__sub_824F6AE0);
REX_HOOK_RAW(sub_824F6AE0) {
  const uint32_t to = ctx.r4.u32, from = ctx.r5.u32;
  __imp__sub_824F6AE0(ctx, base);
  const uint32_t L = Lobby(base), match = Rd32(base + kMatch);
  if (!L || !match || from >= 6 || to >= 6) return;
  uint8_t* slot = base + match + kMatchSlots + to * kMatchSlotSize;
  if (LogOn())
    REXLOG_INFO("online cpu: match slot {} from lobby slot {}: {}", to, from, Hex(slot - 8, 72));
  if (!IsCpu(Record(base, L, from))) return;
  // (joiners build the CPU's block too - the same stock data from the synced
  // id - so it doesn't hang on the host's block broadcast reaching them)
  if (!IsHost(ctx, base)) BuildBlock(ctx, base, L, from, Rd32(Record(base, L, from) + 136));
  // (+56 stays as the game set it, "use the copied block": the host built the
  // CPU's block (BuildBlock) and every machine has it - a cleared +56 left
  // sub_828B92A0 returning 0 to a caller that reads it on joiners)
  REXLOG_INFO("online cpu: match slot {} is the CPU (id {}): {}; block {}", to, Rd32(Record(base, L, from) + 136), Hex(slot - 8, 72),
              Hex(base + L + kBlocks + from * kBlockSize, 32));
}

// The match's people entries from the lobby (sub_824F64A8(obj): 30 entries
// at obj+96 + 20j - id, attire, two more, the controller: this machine's pad
// for its own slot, 4 for the others): a CPU slot's controller is -1, as the
// game's own COM entrants have (the match slot then stays a CPU's).
REX_EXTERN(__imp__sub_824F64A8);
REX_HOOK_RAW(sub_824F64A8) {
  const uint32_t obj = ctx.r3.u32;
  __imp__sub_824F64A8(ctx, base);
  const uint32_t L = Lobby(base);
  if (!L || !obj) return;
  for (uint32_t j = 0; j < SlotCount(base, L); ++j) {
    uint8_t* e = base + obj + 96 + 20 * j;
    if (LogOn()) REXLOG_INFO("online cpu: people entry {}: {}", j, Hex(e, 20));
    if (!IsCpu(Record(base, L, j))) continue;
    Wr32(e + 16, 0xFFFFFFFF);
    REXLOG_INFO("online cpu: people entry {} is a CPU (controller -1)", j);
  }
}

// The online select's pick to the host (sub_828ACCB8(L, ...)): its payload.
REX_EXTERN(__imp__sub_828ACCB8);
REX_HOOK_RAW(sub_828ACCB8) {
  if (LogOn())
    REXLOG_INFO("online cpu: pick sent (r4 {:08X}): {}", ctx.r4.u32,
                ctx.r4.u32 > 0x10000 ? Hex(base + ctx.r4.u32, 24) : std::string());
  __imp__sub_828ACCB8(ctx, base);
}

// Test aid (SVR2011_TEST_LOBBY_LOG): the match start's connection check
// (sub_82891FA8(net, i): -1 = go on; else "Your opponent has left") and its parts.
REX_EXTERN(__imp__sub_82891FA8);
REX_HOOK_RAW(sub_82891FA8) {
  const uint32_t i = ctx.r4.u32;
  __imp__sub_82891FA8(ctx, base);
  if (LogOn()) {
    static int n = 0;
    if (n++ < 30) {
      const uint32_t table = Rd32(base + 0x82EDE930);
      REXLOG_INFO("online cpu: connection check {} -> {} (table {:08X}: {})", i, int32_t(ctx.r3.u32), table,
                  table ? Hex(base + table, 6 * 20) : std::string());
    }
  }
}
REX_EXTERN(__imp__sub_82990FD8);
REX_HOOK_RAW(sub_82990FD8) {
  const uint32_t a = ctx.r4.u32;
  __imp__sub_82990FD8(ctx, base);
}
// The network library's sync (sub_82991010(gss, entry)): logged with CPU slots.
REX_EXTERN(__imp__sub_82991010);
REX_HOOK_RAW(sub_82991010) {
  const uint32_t gss = ctx.r3.u32, e = ctx.r4.u32;
  __imp__sub_82991010(ctx, base);
  const uint32_t L = Lobby(base);
  if (L && CpuCount(base, L)) {
    static int n = 0;
    if (n++ < 40)
      REXLOG_INFO("online cpu: gss sync (gss {:08X}, entry {:08X}: {}) -> {}", gss, e, e ? Hex(base + e, 20) : std::string(),
                  int32_t(ctx.r3.u32));
  }
}
REX_EXTERN(__imp__sub_82891380);
REX_HOOK_RAW(sub_82891380) {
  const uint32_t i = ctx.r4.u32;
  __imp__sub_82891380(ctx, base);
  if (LogOn()) {
    static int n = 0;
    if (n++ < 30) REXLOG_INFO("online cpu: net check {} -> {}", i, int32_t(ctx.r3.u32));
  }
}

// The match's net table (sub_824E5398: *0x82EDE914, +2236 / +2240 the number
// of entries - the lobby's slots - one per human's net player number, +2232
// this machine's): with CPU slots there are fewer humans, so fewer entries
// (else the match waits for players that never connect: "Your opponent has
// left the game session"). The humans' numbers stay 0..n-1.
REX_EXTERN(__imp__sub_824E5398);
REX_HOOK_RAW(sub_824E5398) {
  __imp__sub_824E5398(ctx, base);
  const uint32_t L = Lobby(base), net = Rd32(base + 0x82EDE914);
  if (!L || !net) return;
  const int cpus = CpuCount(base, L);
  if (!cpus) return;
  const uint32_t entries = Rd32(base + net + 2236);
  if (entries <= uint32_t(cpus)) return;
  // (only +2236, the entry count: +2240 stays the lobby's slot count (its
  // reader, sub_82893080, loops over lobby slots) and +2232 stays this
  // machine's LOBBY SLOT - every reader maps it to an entry through the
  // entries' +4 (sub_82892CF0); an entry index there made a joiner not in
  // slot 0 take the CPU for itself and leave the match)
  Wr32(base + net + 2236, entries - cpus);
  REXLOG_INFO("online cpu: net table {} -> {} entries, this machine slot {}: {}", entries, entries - cpus,
              Rd32(base + net + 2232), Hex(base + net, 72 * 3));
}

namespace svr2011 {

// (online.cpp's help bar hook) - fresh only while the lobby runs.
void OnlineCpuHelp(int* y, bool* waiting) {
  const bool fresh = NowMs() - g_help_at.load() < 500;
  *y = fresh ? g_help_y.load() : 0;
  *waiting = fresh && g_help_waiting.load();
}

}  // namespace svr2011
