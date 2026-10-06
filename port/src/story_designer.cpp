// WWE SmackDown vs. Raw 2011 - Story Designer rules the port lifts.
//
// Divas and male superstars together (mixed_gender_matches, roster.cpp - the
// same setting as for matches). Story Designer has three checks of its own,
// all on the record's gender (+208):
// - a scene's cast (sub_82650418(editor), from sub_826517F0, which shows the
//   message): 1 "Cross-gender attacks cannot be set", 2 "A male Superstar
//   has been set in a casting slot for Divas", 3 "A Diva has been set in a
//   casting slot for male Superstars" (0 OK, 4 an unknown attack type);
// - a story match's people (sub_8266AE90(editor), from sub_8266E588): 1
//   "This match type does not permit male Superstars and Divas to face off"
//   (2 Mixed Tag's team balance and 3 no rule are kept);
// - the superstar list for a slot (sub_82675B30(editor)): with a man in the
//   match it leaves the Divas out, with a Diva the men. It asks each person
//   already in the match for a gender through sub_828B6950(id) /
//   sub_828B69B0(record); answering "none" (2) there while it gathers them
//   leaves nobody out. Its select screen (sub_82452428) and the rule check
//   it calls (sub_826356B0) get the real genders.
// (Diva-only / male-only match types and titles keep their rules.)

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

namespace {

bool Mixed() { return rex::cvar::Query<bool>("mixed_gender_matches"); }

thread_local bool g_gathering = false;  // (sub_82675B30 collecting the match's people)

}  // namespace

REX_EXTERN(__imp__sub_82650418);
REX_HOOK_RAW(sub_82650418) {
  __imp__sub_82650418(ctx, base);
  if (Mixed() && ctx.r3.u32 >= 1 && ctx.r3.u32 <= 3) ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__sub_8266AE90);
REX_HOOK_RAW(sub_8266AE90) {
  __imp__sub_8266AE90(ctx, base);
  if (Mixed() && ctx.r3.u32 == 1) ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__sub_82675B30);
REX_HOOK_RAW(sub_82675B30) {
  g_gathering = Mixed();
  __imp__sub_82675B30(ctx, base);
  g_gathering = false;
}

REX_EXTERN(__imp__sub_828B6950);
REX_HOOK_RAW(sub_828B6950) {
  if (g_gathering) {
    ctx.r3.u64 = 2;
    return;
  }
  __imp__sub_828B6950(ctx, base);
}

REX_EXTERN(__imp__sub_828B69B0);
REX_HOOK_RAW(sub_828B69B0) {
  if (g_gathering) {
    ctx.r3.u64 = 2;
    return;
  }
  __imp__sub_828B69B0(ctx, base);
}

REX_EXTERN(__imp__sub_826356B0);
REX_HOOK_RAW(sub_826356B0) {
  const bool was = g_gathering;
  g_gathering = false;
  __imp__sub_826356B0(ctx, base);
  g_gathering = was;
}

REX_EXTERN(__imp__sub_82452428);
REX_HOOK_RAW(sub_82452428) {
  g_gathering = false;  // (the select screen: from here on, the real genders)
  __imp__sub_82452428(ctx, base);
}
