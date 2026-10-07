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

namespace help_page {
void Draw() {
  Heading("Manual", "How the Mod Maker and the .svrmod format work.");
  ImGui::TextWrapped("The manual is being written (docs/MOD_MAKER_MANUAL.md). Until then: every page explains itself at "
                     "the top, (?) marks have more, and the Problems list at the bottom right says what is missing "
                     "before a mod can be saved.");
}
}  // namespace help_page

}  // namespace mm
