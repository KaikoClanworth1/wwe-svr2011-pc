// WWE SmackDown vs. Raw 2011 - CPU players in online Player Matches (work in
// progress: the lobby's records, logged for the design).
//
// The Player Match lobby (screen class "V2": constructor sub_824ED430,
// per-frame sub_824EF138, state at screen+1168, 11 = in the lobby) keeps its
// slots in L = **0x82EDE924: 280-byte records at L+204+j*280 (+204 state,
// +208 net player, +216 name, +340.. the superstar pick, 512 = none); L+24
// joined, L+28 slots. A slot record goes to every client whole
// (sub_828AC700(L, slot, 0), id 12800+slot). At the match's start the host
// builds 30 entrants at L+0x2D49C + 16k {type (7 = COM), lobby slot,
// superstar, occupied} (sub_828A4750), sends them (id 14128) and waits for
// every client's answer (14129); every machine then makes its people from
// them (sub_824F03B0 -> sub_828BB290: the match slots). docs/ONLINE_CPU.md.
//
// Test aid: SVR2011_TEST_LOBBY_LOG=1 logs the lobby's slot records when they
// change, the entrants and the start check.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

namespace {

constexpr uint32_t kLobbyPtr = 0x82EDE924;  // -> -> L
constexpr uint32_t kSlots = 204, kSlotSize = 280;
constexpr uint32_t kEntrants = 0x2D49C, kEntrantSize = 16;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

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

// The lobby's slot records when they change.
void LogSlots(uint8_t* base, const char* why) {
  const uint32_t L = Lobby(base);
  if (!L) return;
  static uint8_t last[8][kSlotSize] = {};
  const uint32_t n = std::min<uint32_t>(Rd32(base + L + 28), 8);
  for (uint32_t j = 0; j < n; ++j) {
    const uint8_t* r = base + L + kSlots + j * kSlotSize;
    if (!std::memcmp(last[j], r, kSlotSize)) continue;
    std::memcpy(last[j], r, kSlotSize);
    REXLOG_INFO("online cpu: {} slot {}/{} (joined {}, people {}, allow {} {}): {}", why, j, n, Rd32(base + L + 24),
                Rd32(base + L + 36), Rd32(base + L + 0x2D494), Rd32(base + L + 0x2D490), Hex(r, kSlotSize));
  }
}

}  // namespace

// The Player Match lobby's frame (screen in r3).
REX_EXTERN(__imp__sub_824EF138);
REX_HOOK_RAW(sub_824EF138) {
  const uint32_t screen = ctx.r3.u32;
  if (LogOn() && screen) {
    static uint32_t frames = 0, last_state = 0xFFFFFFFF;
    if (++frames % 30 == 0) LogSlots(base, "lobby");
    const uint32_t state = Rd32(base + screen + 1168);
    if (state != last_state) {
      REXLOG_INFO("online cpu: lobby screen {:08X} state {} (list {:08X})", screen, state, Rd32(base + screen + 1776));
      last_state = state;
    }
  }
  __imp__sub_824EF138(ctx, base);
}

// The online select's pick to the host (sub_828ACCB8(L, ...)): its payload.
REX_EXTERN(__imp__sub_828ACCB8);
REX_HOOK_RAW(sub_828ACCB8) {
  if (LogOn())
    REXLOG_INFO("online cpu: pick sent (L {:08X}, r4 {:08X}, r5 {:08X}, r6 {:08X}): {}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                ctx.r6.u32, ctx.r4.u32 > 0x10000 ? Hex(base + ctx.r4.u32, 24) : std::string());
  __imp__sub_828ACCB8(ctx, base);
}

// The host stores a player's pick (vt+272, sub_828AE250).
REX_EXTERN(__imp__sub_828AE250);
REX_HOOK_RAW(sub_828AE250) {
  if (LogOn()) REXLOG_INFO("online cpu: host stores a pick (r4 {:08X}, r5 {:08X}, r6 {:08X})", ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  __imp__sub_828AE250(ctx, base);
  if (LogOn()) LogSlots(base, "picked");
}

// The select's pick step (sub_824C3C28(flow)): when it runs.
REX_EXTERN(__imp__sub_824C3C28);
REX_HOOK_RAW(sub_824C3C28) {
  if (LogOn()) {
    static int n = 0;
    if (n++ < 40) REXLOG_INFO("online cpu: select pick step (flow {:08X})", ctx.r3.u32);
  }
  __imp__sub_824C3C28(ctx, base);
}

// The host builds the match's entrants from the lobby (L in r3).
REX_EXTERN(__imp__sub_828A4750);
REX_HOOK_RAW(sub_828A4750) {
  const uint32_t L = ctx.r3.u32;
  __imp__sub_828A4750(ctx, base);
  if (!LogOn() || !L) return;
  LogSlots(base, "entrants");
  for (uint32_t k = 0; k < 30; ++k) {
    const uint8_t* e = base + L + kEntrants + k * kEntrantSize;
    if (Rd32(e) || Rd32(e + 8)) REXLOG_INFO("online cpu: entrant {}: {}", k, Hex(e, kEntrantSize));
  }
}

// An entrant's COM data (sub_828A4130(L, k, ...)): accepted or not.
REX_EXTERN(__imp__sub_828A4130);
REX_HOOK_RAW(sub_828A4130) {
  const uint32_t k = ctx.r4.u32;
  __imp__sub_828A4130(ctx, base);
  if (LogOn()) REXLOG_INFO("online cpu: entrant {} COM data -> {}", k, ctx.r3.u32);
}

// The host's "start the match" check (START in the lobby).
REX_EXTERN(__imp__sub_824EEA88);
REX_HOOK_RAW(sub_824EEA88) {
  __imp__sub_824EEA88(ctx, base);
  if (LogOn()) {
    static uint32_t last = 2;
    if (ctx.r3.u32 != last) {
      REXLOG_INFO("online cpu: start check -> {}", ctx.r3.u32);
      last = ctx.r3.u32;
    }
  }
}
