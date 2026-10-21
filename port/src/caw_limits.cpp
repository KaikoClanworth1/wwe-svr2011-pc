// WWE SmackDown vs. Raw 2011 - CREATE A SUPERSTAR without its limits
// (caw_no_limits, OFF by default until the editor's part heap is guarded - see
// the end of this comment; OPTIONS -> CAW LIMITS):
//
// - No REMAINING POINTS budget. Every part / layer costs a size class
//   (sub_828DC468: a class from the layer's sub-record, table 0x8206E0B8
//   {0,1,2,4,6,8,12,16}) against a budget of 48 (sub_828DC280); over it the
//   pickers grey items out ("... Remaining Points are maxed out", strings
//   6730-6735: sub_828D76F8's bits 0x4 / 0x8 / 0x10) and a saved costume
//   isn't applied (sub_828E1F10, bit 0x2). With the cost 0 nothing is ever
//   over (the meter reads 48 of 48).
// - Any number of parts of one kind: a category's maximum (sub_828EFE70(cat,
//   0), table 0x8206EFD8: 1-3 for clothing) becomes the block's room, so a
//   second top, jacket or pair of tights goes on as a new layer instead of
//   replacing the first. Kept as the game has them: the base slots (2, 4-10:
//   body, head, face, hair - one fixed slot each), the categories already at
//   64, the second maximum (k 1), and the real array limits (56 layers per
//   attire block + 8 base, 64 in the editor: "layers are maxed out").
//
// Nothing in the .cas or the online lobby block changes size: a CAW made so
// still fits both; only its layers are more / dearer than the game allowed.
//
// NOT SAFE YET (21 Oct tests): the budget and the counts are what keeps a CAW
// inside the editor's part heap. Opening a CAW in the editor whose ring
// attire block 0 held 31 hoodies (16 points each) or 16+ extra T-shirts (0
// points) crashed: the part loader (sub_828E3718 -> sub_826A1318) got no
// buffer and the decoder (sub_826AEF10) wrote to F7F7F7F8. 24 hoodies / 10
// extra shirts opened. Matches and the select screen took 50 hoodies. Before
// this is on by default: a guard by the parts' real sizes (the edit pac's
// models) or a bigger editor heap.
#include <cstdint>
#include <cstdlib>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

REXCVAR_DEFINE_BOOL(caw_no_limits, false, "Game",
                    "CREATE A SUPERSTAR without the REMAINING POINTS budget and with any number of parts of a kind "
                    "(OPTIONS -> CAW LIMITS)");

namespace {

bool NoLimits() {
  // (test aid: SVR2011_TEST_CAW_LIMITS=stock / off - the game's own rules whatever the setting)
  static const bool stock = [] {
    const char* v = std::getenv("SVR2011_TEST_CAW_LIMITS");
    return v && (*v == 's' || *v == '0' || *v == 'o');
  }();
  return !stock && REXCVAR_GET(caw_no_limits);
}

}  // namespace

// A layer's cost in REMAINING POINTS: sub_828DC468(layer sub-record) -> points.
REX_EXTERN(__imp__sub_828DC468);
REX_HOOK_RAW(sub_828DC468) {
  if (!NoLimits()) {
    __imp__sub_828DC468(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
}

// A category's maximum count: sub_828EFE70(category, k) -> the table's word k
// (0, 1: the two maxima; 2, 3: sort keys).
REX_EXTERN(__imp__sub_828EFE70);
REX_HOOK_RAW(sub_828EFE70) {
  const uint32_t cat = ctx.r3.u32, k = ctx.r4.u32;
  __imp__sub_828EFE70(ctx, base);
  if (k != 0 || !NoLimits()) return;
  const bool base_slot = cat == 2 || (cat >= 4 && cat <= 10);
  const uint32_t stock = ctx.r3.u32;
  if (base_slot || stock == 0 || stock >= 64) return;
  ctx.r3.u64 = 64;
  static bool told = false;
  if (!told) {
    told = true;
    REXLOG_INFO("[svr2011] caw limits: any number of parts of a kind (category {} max {} -> 64)", cat, stock);
  }
}
