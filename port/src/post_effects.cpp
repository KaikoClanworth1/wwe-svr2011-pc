// WWE SmackDown vs. Raw 2011 - depth of field and motion blur switches.
//
// The game's post effects read one settings struct (pointer at 0x82EDE250):
// +4 depth of field on, +356 / +360 the radial blur, +236 the after-image
// (ghost trail). Each effect is a class with a Render(this) function; the
// camera code sets the fields freely, so the switch is applied around the
// renderers instead:
// - Depth of field: +4 reads as 0 while the DOF filter (sub_82715AD8), the
//   HDR / final scene pass (sub_82714B48, which then picks the non-DOF
//   technique) and the passes DOF can fold in (monotone sub_82714F90,
//   antialiasing sub_82716650, downscale sub_827163D0) render - the game's
//   own "no DOF" state - and is put back for the camera code. Far shots
//   no longer blur the wrestlers.
// - The game's soft filter (+340, "antialiasing": a blur over the finished
//   frame, made for 720p): in wide shots it smeared the wrestlers at any
//   higher resolution. Off by default (the native renderer has real
//   anti-aliasing); +340 reads as 0 around the same renderers.
// - Motion blur: the radial blur (sub_82710E08) and the after-image
//   (sub_827156B0) take their own disabled paths.

#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

REXCVAR_DEFINE_BOOL(depth_of_field, true, "GPU", "The game's depth of field (far shots blur what is out of focus)");
REXCVAR_DEFINE_BOOL(soft_filter, false, "GPU",
                    "The game's own soft filter over the frame (made for 720p; blurs wide shots at higher resolutions)");
REXCVAR_DEFINE_BOOL(motion_blur, true, "GPU", "The game's motion blur (radial blur and after-image trails)");

namespace {

constexpr uint32_t kPostSettings = 0x82EDE250;  // (pointer to the settings struct)

uint32_t Load32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// Runs the renderer with DOF (+4) and the soft filter (+340) reading as off
// when they are switched off, then puts them back (the camera code reads them).
template <void (*Original)(PPCContext&, uint8_t*)>
void WithoutDof(PPCContext& ctx, uint8_t* base) {
  const bool dof = REXCVAR_GET(depth_of_field), soft = REXCVAR_GET(soft_filter);
  const uint32_t settings = dof && soft ? 0 : Load32(base, kPostSettings);
  if (!settings) {
    Original(ctx, base);
    return;
  }
  const uint32_t saved_dof = Load32(base, settings + 4), saved_soft = Load32(base, settings + 340);
  if (!dof) Store32(base, settings + 4, 0);
  if (!soft) Store32(base, settings + 340, 0);
  Original(ctx, base);
  Store32(base, settings + 4, saved_dof);
  Store32(base, settings + 340, saved_soft);
}

}  // namespace

REX_EXTERN(__imp__sub_82715AD8);
REX_HOOK_RAW(sub_82715AD8) { WithoutDof<__imp__sub_82715AD8>(ctx, base); }  // DOF filter
REX_EXTERN(__imp__sub_82714B48);
REX_HOOK_RAW(sub_82714B48) { WithoutDof<__imp__sub_82714B48>(ctx, base); }  // HDR / final scene pass
REX_EXTERN(__imp__sub_82714F90);
REX_HOOK_RAW(sub_82714F90) { WithoutDof<__imp__sub_82714F90>(ctx, base); }  // monotone
REX_EXTERN(__imp__sub_82716650);
REX_HOOK_RAW(sub_82716650) {  // the soft filter ("antialiasing")
  if (!REXCVAR_GET(soft_filter)) return;
  WithoutDof<__imp__sub_82716650>(ctx, base);
}
REX_EXTERN(__imp__sub_827163D0);
REX_HOOK_RAW(sub_827163D0) { WithoutDof<__imp__sub_827163D0>(ctx, base); }  // downscale

// Radial blur: its own "off" path (returns at once).
REX_EXTERN(__imp__sub_82710E08);
REX_HOOK_RAW(sub_82710E08) {
  if (!REXCVAR_GET(motion_blur)) return;
  __imp__sub_82710E08(ctx, base);
}

// After-image: its own "off" path (clears its state, returns).
REX_EXTERN(__imp__sub_827156B0);
REX_HOOK_RAW(sub_827156B0) {
  if (!REXCVAR_GET(motion_blur)) {
    Store32(base, ctx.r3.u32 + 8, 0);
    return;
  }
  __imp__sub_827156B0(ctx, base);
}
