// Pages still being built (plan: docs/MOD_MAKER_PLAN.md): Game assets,
// Animations, Icons & renders and the Manual. Each says what it will
// do until it does it.
#include "app.h"

namespace mm {

namespace {
void Soon(const char* title, const char* about, const char* plan) {
  Heading(title, about);
  ImGui::Spacing();
  ImGui::TextWrapped("%s", plan);
}
}  // namespace

namespace assets_page {
void Draw() {
  Soon("Game assets", "Everything in the game's pac files, read-only.",
       "Coming: a tree of the pac files (EPAC / EPK8, groups, entries, PACH, BPE, texture bundles) with a viewer for "
       "each kind - textures, models in 3D, animation banks, tables - and export of any entry.");
}
}  // namespace assets_page

namespace anims_page {
void Draw() {
  Soon("Animations", "A character playing the game's motions: taunts, stances, finishers.",
       "Coming: pick a superstar or a mod, a motion by name, play / pause / scrub; match moves through the game "
       "with a dummy opponent.");
}
}  // namespace anims_page

namespace icons_page {
void Draw() {
  Soon("Icons & renders", "The game's pictures: superstar renders and face icons, arena banners, VS screens, "
                           "loading screens, crowd signs.",
       "Coming: browse them all, export any, and send one to the mod page that uses it.");
}
}  // namespace icons_page

namespace help_page {
void Draw() {
  Heading("Manual", "How the Mod Maker and the .svrmod format work.");
  ImGui::TextWrapped("The manual is being written (docs/MOD_MAKER_MANUAL.md). Until then: every page explains itself at "
                     "the top, (?) marks have more, and the Problems list at the bottom right says what is missing "
                     "before a mod can be saved.");
}
}  // namespace help_page

}  // namespace mm
