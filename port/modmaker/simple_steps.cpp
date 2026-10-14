// The Simple modes' step framework (simple_steps.h).
#include "simple_steps.h"

#include <algorithm>

namespace mm {
namespace simple {

bool Card(const char* id, const char* title, const char* line, bool on, float w, float h) {
  ImGui::PushID(id);
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const bool hit = ImGui::InvisibleButton("##card", ImVec2(w, h));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 bg = ImGui::GetColorU32(on ? ImGuiCol_HeaderActive : ImGui::IsItemHovered() ? ImGuiCol_HeaderHovered : ImGuiCol_FrameBg);
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 6 * g_scale);
  if (on) dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(ImGuiCol_Text), 6 * g_scale, 0, 2 * g_scale);
  const float pad = 10 * g_scale;
  dl->AddText(ImVec2(p.x + pad, p.y + pad), ImGui::GetColorU32(ImGuiCol_Text), title);
  dl->PushClipRect(p, ImVec2(p.x + w, p.y + h), true);
  dl->AddText(nullptr, 0, ImVec2(p.x + pad, p.y + pad + ImGui::GetTextLineHeightWithSpacing()),
              on ? IM_COL32(255, 255, 255, 220) : ImGui::GetColorU32(ImGuiCol_TextDisabled), line, nullptr, w - 2 * pad);
  dl->PopClipRect();
  ImGui::PopID();
  return hit;
}

float CardWidth() {
  const float avail = ImGui::GetContentRegionAvail().x, gap = 8 * g_scale;
  const int n = std::max(1, int((avail + gap) / (200 * g_scale + gap)));
  return std::min(280 * g_scale, (avail - gap * float(n - 1)) / float(n));
}
int CardsPerRow(float w) { return std::max(1, int((ImGui::GetContentRegionAvail().x + 8 * g_scale) / (w + 8 * g_scale))); }

bool HeadingButton(const char* label, float top_y, bool accent) {
  const ImVec2 after = ImGui::GetCursorPos();
  const float bw = ImGui::CalcTextSize(label).x + 40 * g_scale;
  ImGui::SetCursorPos(ImVec2(std::max(0.0f, ImGui::GetWindowWidth() - bw - 24 * g_scale), top_y));
  if (accent) PushAccent();
  const bool hit = ImGui::Button(label, ImVec2(bw, 0));
  if (accent) PopAccent();
  ImGui::SetCursorPos(after);
  return hit;
}

void StepTitle(const Step& s) {
  ImGui::PushFont(nullptr);
  ImGui::SetWindowFontScale(1.45f);
  ImGui::TextUnformatted(s.title);
  ImGui::SetWindowFontScale(1.0f);
  ImGui::PopFont();
  ImGui::TextColored(ImVec4(0.6f, 0.75f, 1, 1), "What is this?");
  ImGui::SameLine();
  ImGui::PushTextWrapPos(0);
  ImGui::TextUnformatted(s.what);
  ImGui::PopTextWrapPos();
  ImGui::Separator();
  ImGui::Spacing();
}

void StepList(const Step* steps, int count, int& step, const bool* has_error, float width, float bottom) {
  ImGui::BeginChild("steps", ImVec2(width * g_scale, -bottom * g_scale), true);
  for (int i = 0; i < count; ++i) {
    const bool on = i == step;
    const bool err = has_error && has_error[i];
    const bool done = i < step && !err;
    std::string label = std::to_string(i + 1) + ".  " + steps[i].title;
    if (err) label += "  !";
    label += "##step" + std::to_string(i);
    if (ImGui::Selectable(label.c_str(), on, 0, ImVec2(0, 30 * g_scale))) step = i;
    if (done) {  // a tick at the right (the font has no check mark)
      const ImVec2 mx = ImGui::GetItemRectMax(), mn = ImGui::GetItemRectMin();
      const float cy = (mn.y + mx.y) * 0.5f, x = mx.x - 22 * g_scale, k = g_scale;
      const ImVec2 pts[3] = {ImVec2(x, cy), ImVec2(x + 5 * k, cy + 5 * k), ImVec2(x + 14 * k, cy - 6 * k)};
      ImGui::GetWindowDrawList()->AddPolyline(pts, 3, IM_COL32(110, 220, 130, 255), 0, 2.5f * k);
    }
  }
  ImGui::EndChild();
}

void BottomBar(int& step, int count) {
  ImGui::BeginDisabled(step == 0);
  if (ImGui::Button("Back", ImVec2(140 * g_scale, 36 * g_scale))) --step;
  ImGui::EndDisabled();
  ImGui::SameLine();
  const float bar_w = std::max(100.0f, ImGui::GetContentRegionAvail().x - 160 * g_scale);
  ImGui::BeginGroup();
  ImGui::Text("Step %d of %d", step + 1, count);
  ImGui::ProgressBar(float(step + 1) / float(count), ImVec2(bar_w - 8 * g_scale, 10 * g_scale), "");
  ImGui::EndGroup();
  ImGui::SameLine(ImGui::GetWindowWidth() - 160 * g_scale);
  if (step + 1 < count) {
    PushAccent();
    if (ImGui::Button("Next", ImVec2(140 * g_scale, 36 * g_scale))) ++step;
    PopAccent();
  }
}

std::string Plain(std::string t) {
  for (size_t a; (a = t.find(" (entry ")) != std::string::npos;) {
    const size_t b = t.find(')', a);
    if (b == std::string::npos) break;
    t.erase(a, b - a + 1);
  }
  return t;
}

void ProblemList(const std::vector<Problem>& problems, int& step, int finish) {
  if (problems.empty()) {
    ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.5f, 1), "Everything looks good.");
    return;
  }
  ImGui::TextUnformatted("Things to check");
  for (size_t k = 0; k < problems.size(); ++k) {
    const Problem& p = problems[k];
    ImGui::PushID(int(k));
    ImGui::TextColored(p.error ? ImVec4(1, 0.45f, 0.4f, 1) : ImVec4(1, 0.8f, 0.35f, 1), p.error ? "Must fix:" : "Tip:");
    ImGui::SameLine();
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 90 * g_scale + ImGui::GetCursorPosX());
    ImGui::TextUnformatted(Plain(p.text).c_str());
    ImGui::PopTextWrapPos();
    if (!p.fixes.empty()) {  // Fix it: the recommended fix, done here (and said); the others beside it
      ImGui::SameLine();
      PushAccent();
      const bool fix = ImGui::SmallButton("Fix it");
      PopAccent();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.fixes[0].label.c_str());
      if (fix) {
        const FixAction f = p.fixes[0];
        RunFix(f);
        ImGui::PopID();
        break;  // (the list changes)
      }
      if (p.fixes.size() > 1) {
        ImGui::Indent();
        Problem rest = p;
        rest.fixes.erase(rest.fixes.begin());
        ImGui::TextDisabled("or:");
        ImGui::SameLine();
        const bool pressed = FixButtons(rest, false);
        ImGui::Unindent();
        if (pressed) {
          ImGui::PopID();
          break;
        }
      }
    } else if (p.step >= 0 && p.step != finish && !p.note) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Fix it")) step = p.step;
    }
    ImGui::PopID();
  }
}

void SummaryRow(const Step* steps, int at, const std::string& text, int& step) {
  ImGui::PushID(at);
  ImGui::TextDisabled("%-14s", steps[at].title);
  ImGui::SameLine(150 * g_scale);
  ImGui::TextUnformatted(text.c_str());
  ImGui::SameLine();
  if (ImGui::SmallButton("Change")) step = at;
  ImGui::PopID();
}

void ErrorSteps(const std::vector<Problem>& problems, int count, bool* out) {
  for (int i = 0; i < count; ++i) out[i] = false;
  for (const auto& p : problems)
    if (p.error && p.step >= 0 && p.step < count) out[p.step] = true;
}

bool GamePicker(const char* id, std::string& key, const std::function<bool(const disc_list::Game&)>& supported) {
  ImGui::PushID(id);
  const float w = CardWidth(), h = 70 * g_scale;
  const int per_row = CardsPerRow(w);
  bool changed = false;
  int n = 0;
  for (const auto& g : disc_list::Games()) {
    if (!supported(g)) continue;
    const bool have = !disc_list::PathFor(g.key).empty();
    const std::string line = std::string(g.xbox ? "Xbox 360" : "PS2") + (have ? " - your disc is set" : " - no disc yet: click to add it");
    if (n++ % per_row) ImGui::SameLine(0, 8 * g_scale);
    if (Card(g.key, g.name, line.c_str(), key == g.key, w, h)) {
      if (!have) GoTo(PageId::kDiscs);
      else if (key != g.key) key = g.key, changed = true;
    }
  }
  if (!n) ImGui::TextDisabled("No game here can be read yet.");
  ImGui::PopID();
  return changed;
}

}  // namespace simple
}  // namespace mm
