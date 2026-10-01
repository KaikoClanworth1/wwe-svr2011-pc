// WWE SmackDown vs. Raw 2011 - roster rules the port lifts.
//
// Divas and male superstars in one match: character select only lets a pick
// join when it has the gender of the ones already picked. sub_82734CA0
// (match, slot) - may this character take this slot - asks sub_827336A0
// (match, -1, a, b) for the gender the picks so far share (0 male, 1 diva,
// 2 none / mixed) and refuses a pick of the other one; with a diva picked the
// select screen also switches to the Divas roster and greys the SUPERSTARS
// tile (sub_8244C4F8 tries both genders through sub_82734CA0). Answering
// "none" lets either gender join. The engine plays mixed matches already
// (Mixed Tag: men and divas in one match), and nothing refuses to start one.
// (Divas title matches keep their own rule: sub_824518A8.)

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

REXCVAR_DEFINE_BOOL(mixed_gender_matches, true, "Gameplay",
                    "Divas and male superstars may be picked for the same match");

REX_EXTERN(__imp__sub_827336A0);
REX_HOOK_RAW(sub_827336A0) {
  if (REXCVAR_GET(mixed_gender_matches)) {
    ctx.r3.u64 = 2;  // (no shared gender: anyone may join)
    return;
  }
  __imp__sub_827336A0(ctx, base);
}
