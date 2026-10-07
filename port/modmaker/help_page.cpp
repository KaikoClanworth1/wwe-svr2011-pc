// Manual page: docs/MOD_MAKER_MANUAL.md and docs/SVRMOD_FORMAT.md, built
// into the exe (CMake writes manual.inc from them) and drawn with a small
// Markdown reader: headings, paragraphs, bullets, tables, code blocks.
#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

#include "app.h"

#include <shellapi.h>

namespace mm {
namespace help_page {

namespace {

const char* const kManual =
#include "manual.inc"
    ;
const char* const kFormat =
#include "format.inc"
    ;

int g_doc = 0;
char g_find[64] = "";

// `code` and **bold** inline: drawn as runs.
void Inline(const std::string& text) {
  size_t at = 0;
  bool first = true;
  auto run = [&](const std::string& s, bool code, bool bold) {
    if (s.empty()) return;
    if (!first) ImGui::SameLine(0, 0);
    first = false;
    if (code) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.8f, 0.55f, 1));
      ImGui::TextUnformatted(s.c_str());
      ImGui::PopStyleColor();
    } else if (bold) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
      ImGui::TextUnformatted(s.c_str());
      ImGui::PopStyleColor();
    } else {
      ImGui::TextUnformatted(s.c_str());
    }
  };
  // split on ` and ** (a simple state machine); wrapping is per paragraph, so long
  // paragraphs are wrapped by words here
  std::string plain;
  bool code = false, bold = false;
  const float wrap_x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
  auto flush = [&] {
    // word wrap the plain run; the spaces at its ends stay (they separate the runs)
    const bool lead = !plain.empty() && plain.front() == ' ', trail = plain.size() > 1 && plain.back() == ' ';
    std::istringstream ws(plain);
    std::string word;
    std::string line = lead ? " " : "";
    bool any = false;
    while (ws >> word) {
      const std::string cand = !any ? line + word : line + " " + word;
      const float x = first ? ImGui::GetCursorPosX() : ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
      if (any && x + ImGui::CalcTextSize(cand.c_str()).x > wrap_x) {
        run(line + " ", code, bold);
        first = true;  // (a new line)
        line = word;
      } else {
        line = cand;
      }
      any = true;
    }
    if (!line.empty()) run(line + (trail ? " " : ""), code, bold);
    plain.clear();
  };
  while (at < text.size()) {
    if (text[at] == '`') { flush(); code = !code; ++at; continue; }
    if (text.compare(at, 2, "**") == 0) { flush(); bold = !bold; at += 2; continue; }
    plain += text[at++];
  }
  flush();
  if (first) ImGui::NewLine();
}

void Table(const std::vector<std::string>& rows) {
  // rows: "| a | b |" lines; the second row is the separator
  std::vector<std::vector<std::string>> cells;
  for (size_t r = 0; r < rows.size(); ++r) {
    if (r == 1) continue;
    std::vector<std::string> c;
    std::string s = rows[r];
    size_t at = s.find('|') + 1;
    while (at < s.size()) {
      size_t e = s.find('|', at);
      if (e == std::string::npos) break;
      std::string v = s.substr(at, e - at);
      v.erase(0, v.find_first_not_of(' '));
      while (!v.empty() && v.back() == ' ') v.pop_back();
      c.push_back(v);
      at = e + 1;
    }
    cells.push_back(c);
  }
  if (cells.empty()) return;
  const int cols = int(cells[0].size());
  if (ImGui::BeginTable("t", cols, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
    const float avail = ImGui::GetContentRegionAvail().x;
    for (int c = 0; c < cols; ++c) {
      float w = 0;
      for (const auto& row : cells)
        if (c < int(row.size())) w = std::max(w, ImGui::CalcTextSize(row[size_t(c)].c_str()).x);
      w = std::min(w + 16 * g_scale, avail * 0.38f);
      if (c == cols - 1) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
      else ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, w);
    }
    for (const auto& row : cells) {
      ImGui::TableNextRow();
      for (int c = 0; c < cols; ++c) {
        ImGui::TableNextColumn();
        if (c < int(row.size())) {
          ImGui::PushTextWrapPos(0);
          Inline(row[size_t(c)]);
          ImGui::PopTextWrapPos();
        }
      }
    }
    ImGui::EndTable();
  }
}

void Markdown(const char* text) {
  std::istringstream in(text);
  std::string line, para;
  std::vector<std::string> table;
  bool code = false;
  int n = 0;
  auto para_flush = [&] {
    if (para.empty()) return;
    Inline(para);
    ImGui::Spacing();
    para.clear();
  };
  auto table_flush = [&] {
    if (table.empty()) return;
    ImGui::PushID(n++);
    Table(table);
    ImGui::PopID();
    ImGui::Spacing();
    table.clear();
  };
  const std::string find = Lower(g_find);
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("```", 0) == 0) {
      para_flush();
      code = !code;
      if (code) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.8f, 0.55f, 1));
      else ImGui::PopStyleColor(), ImGui::Spacing();
      continue;
    }
    if (code) { ImGui::TextUnformatted(line.c_str()); continue; }
    if (line.rfind("| ", 0) == 0 || line.rfind("|-", 0) == 0) { para_flush(); table.push_back(line); continue; }
    table_flush();
    if (line.empty()) { para_flush(); continue; }
    if (line[0] == '#') {
      para_flush();
      int level = 0;
      while (level < int(line.size()) && line[size_t(level)] == '#') ++level;
      std::string title = line.substr(size_t(level));
      title.erase(0, title.find_first_not_of(' '));
      if (level == 1) continue;  // (the page heading says it)
      ImGui::Spacing();
      ImGui::PushFont(nullptr, ImGui::GetFontSize() * (level == 2 ? 1.3f : 1.1f));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.45f, 0.55f, 1));
      ImGui::TextUnformatted(title.c_str());
      ImGui::PopStyleColor();
      ImGui::PopFont();
      if (level == 2) ImGui::Separator();
      continue;
    }
    if (line.rfind("- ", 0) == 0) {
      para_flush();
      ImGui::Bullet();
      ImGui::SameLine();
      ImGui::BeginGroup();
      std::string item = line.substr(2);
      // continuation lines (indented) belong to the bullet
      std::streampos pos = in.tellg();
      std::string next;
      while (std::getline(in, next)) {
        if (!next.empty() && next.back() == '\r') next.pop_back();
        if (next.rfind("  ", 0) != 0 || next.rfind("- ", 0) == 0) { in.seekg(pos); break; }
        item += " " + next.substr(next.find_first_not_of(' '));
        pos = in.tellg();
      }
      Inline(item);
      ImGui::EndGroup();
      continue;
    }
    para += (para.empty() ? "" : " ") + line;
  }
  para_flush();
  table_flush();
}

}  // namespace

void Draw() {
  Heading("Manual", "How the Mod Maker works, page by page, and what is inside a .svrmod.");
  if (ImGui::RadioButton("The Mod Maker", g_doc == 0)) g_doc = 0;
  ImGui::SameLine();
  if (ImGui::RadioButton("The .svrmod format", g_doc == 1)) g_doc = 1;
  ImGui::SameLine(0, 30 * g_scale);
  {  // the full guide (pictures, diagrams, tutorials): next to the exe, or docs/guide in a development build
    static std::wstring guide;
    static bool looked = false;
    if (!looked) {
      looked = true;
      wchar_t buf[MAX_PATH * 2];
      GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
      const fs::path exe = fs::path(buf).parent_path();
      std::error_code ec;
      for (const fs::path cand : {exe / L"Mod Maker Guide" / L"index.html", exe / L".." / L".." / L".." / L".." / L"docs" / L"guide" / L"index.html"})
        if (fs::exists(cand, ec)) { guide = fs::weakly_canonical(cand, ec).wstring(); break; }
    }
    if (!guide.empty()) {
      PushAccent();
      if (ImGui::Button("Open the full guide (pictures and tutorials)")) ShellExecuteW(nullptr, L"open", guide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      PopAccent();
      ImGui::SameLine();
    }
  }
  ImGui::TextDisabled("Also as files: docs/MOD_MAKER_MANUAL.md and docs/SVRMOD_FORMAT.md.");
  ImGui::BeginChild("doc", ImVec2(0, 0), true);
  ImGui::PushTextWrapPos(0);
  Markdown(g_doc == 0 ? kManual : kFormat);
  ImGui::PopTextWrapPos();
  ImGui::EndChild();
}

}  // namespace help_page
}  // namespace mm
