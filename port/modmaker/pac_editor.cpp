// The pac editor (checklist D16): a copy of any EPAC / EPK8 pac - from this
// game, another disc or a file - edited in memory and saved somewhere else.
// Entries can be exported, replaced, renamed, removed and added; an entry
// that is a PACH (a list of children by id) opens, and its children can be
// exported, replaced, removed and added the same way. Replaced data is packed
// like the original (BPE when the original was), always, or never. The game's
// own pac folder is never written: Save as refuses it. A save with nothing
// changed is byte-identical to the original (svrfmt/pac.h; --test-repack).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#include "app.h"
#include "svrfmt/pac.h"

namespace mm {
namespace pac_editor {

using namespace svrfmt;

namespace {

Epac g_pac;
bool g_open = false;
std::string g_label;       // what it is ("SvR 2008: pac/menu/MatchSD.pac")
std::wstring g_from;       // where it came from (for the default save name)
std::string g_note;        // e.g. "unwrapped from xcompress"
size_t g_original_size = 0;
int g_changes = 0;
int g_pack = 0;            // 0 like the original, 1 always BPE, 2 never
std::set<std::pair<int, int>> g_changed;  // (group, entry) edited
int g_pach_g = -1, g_pach_e = -1;          // the PACH entry opened below
std::vector<PachEntry> g_kids;             // its children (as stored)
bool g_pach_bpe = false;                   // the entry was a BPE-wrapped PACH
char g_find[48] = "";
char g_new_name[16] = "";

Bytes Pack(const Bytes& data, bool original_bpe) {
  if (IsBpe(data)) return data;  // (already packed)
  if (g_pack == 2 || (g_pack == 0 && !original_bpe)) return data;
  return BpeEncode(data);
}

void Changed(int g, int e) {
  g_changed.insert({g, e});
  ++g_changes;
}

// The open PACH's children back into its entry.
void StorePach() {
  if (g_pach_g < 0) return;
  Bytes p = PachWrite(g_kids);
  g_pac.groups[size_t(g_pach_g)].entries[size_t(g_pach_e)].data = g_pach_bpe ? BpeEncode(p) : p;
  Changed(g_pach_g, g_pach_e);
}

void ClosePach() { g_pach_g = g_pach_e = -1, g_kids.clear(); }

std::string Kind(const Bytes& stored) {
  const Bytes d = IsBpe(stored) ? Unpack(stored) : stored;
  if (d.size() >= 4) {
    if (!std::memcmp(d.data(), "PACH", 4)) return "PACH";
    if (!std::memcmp(d.data(), "DDS ", 4)) return "DDS";
    if (!std::memcmp(d.data(), "JBOY", 4)) return "model";
    if (!std::memcmp(d.data(), "YMBs", 4) || !std::memcmp(d.data(), "YMKs", 4)) return "motions";
  }
  return "data";
}

bool ExportBytes(const Bytes& b, const std::string& stem) {
  static const COMDLG_FILTERSPEC kAny[1] = {{L"All files", L"*.*"}};
  const std::wstring f = PickFile(true, L"Export (unpacked)", kAny, 1, L"bin", Wide(stem + ".bin").c_str());
  if (f.empty()) return false;
  const bool ok = WriteFile(Utf8(f), IsBpe(b) ? Unpack(b) : b);
  Status(ok ? "Exported " + Utf8(f) : "Could not write " + Utf8(f));
  return ok;
}

bool PickData(Bytes& out) {
  static const COMDLG_FILTERSPEC kAny[1] = {{L"All files", L"*.*"}};
  const std::wstring f = PickFile(false, L"The new data", kAny, 1);
  if (f.empty()) return false;
  if (!ReadFile(Utf8(f), out)) {
    Status("Could not read " + Utf8(f));
    return false;
  }
  return true;
}

// Inside the game's own pac folder (never written).
bool InGamePac(const std::wstring& f) {
  if (g_game.empty()) return false;
  std::error_code ec;
  const std::wstring a = Wide(Lower(Utf8(fs::weakly_canonical(fs::path(g_game) / L"pac", ec).wstring())));
  const std::wstring b = Wide(Lower(Utf8(fs::weakly_canonical(fs::path(f), ec).wstring())));
  return b.rfind(a, 0) == 0;
}

void SaveAs() {
  const std::wstring stem = g_from.empty() ? L"copy" : fs::path(g_from).stem().wstring();
  const std::wstring f = PickFile(true, L"Save the pac copy (not in the game's pac folder)", kPacFilter, 1, L"pac", (stem + L".pac").c_str());
  if (f.empty()) return;
  if (InGamePac(f)) {
    Status("The game's own pac folder is never written: save the copy somewhere else (a mod's folder, a work folder).");
    return;
  }
  StorePach();
  const Bytes out = EpacWrite(g_pac);
  if (!WriteFile(Utf8(f), out)) {
    Status("Could not write " + Utf8(f));
    return;
  }
  Log("pac editor: saved " + Utf8(f) + " (" + Human(out.size()) + ", " + std::to_string(g_changes) + " changes)");
  Status("Saved " + Utf8(f) + (g_changes ? "" : " (unchanged: the same bytes as the original)"));
}

void PachRows() {
  if (g_pach_g < 0) return;
  const auto& entry = g_pac.groups[size_t(g_pach_g)].entries[size_t(g_pach_e)];
  ImGui::SeparatorText((g_pac.groups[size_t(g_pach_g)].type + "/" + entry.name + ": its children").c_str());
  if (ImGui::BeginTable("kids", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                        ImVec2(0, std::min(260 * g_scale, (2 + float(g_kids.size())) * ImGui::GetFrameHeightWithSpacing())))) {
    ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, 110 * g_scale);
    ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
    ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 90 * g_scale);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int remove = -1;
    for (size_t k = 0; k < g_kids.size(); ++k) {
      ImGui::PushID(int(k));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%u (0x%X)", g_kids[k].id, g_kids[k].id);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(Kind(g_kids[k].data).c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(Human(g_kids[k].data.size()).c_str());
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Export")) ExportBytes(g_kids[k].data, entry.name + "_" + std::to_string(g_kids[k].id));
      ImGui::SameLine();
      if (ImGui::SmallButton("Replace...")) {
        Bytes b;
        if (PickData(b)) g_kids[k].data = Pack(b, IsBpe(g_kids[k].data)), StorePach();
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Remove")) remove = int(k);
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (remove >= 0) g_kids.erase(g_kids.begin() + remove), StorePach();
  }
  static int new_id = 0;
  ImGui::SetNextItemWidth(110 * g_scale);
  ImGui::InputInt("##newid", &new_id);
  ImGui::SameLine();
  if (ImGui::Button("Add a child with this id...")) {
    if (std::any_of(g_kids.begin(), g_kids.end(), [](const PachEntry& c) { return c.id == uint32_t(new_id); }))
      Status("That id is taken: replace that child instead.");
    else {
      Bytes b;
      if (PickData(b)) {
        PachEntry c;
        c.id = uint32_t(new_id);
        c.data = Pack(b, !g_kids.empty() && IsBpe(g_kids.front().data));
        auto at = std::lower_bound(g_kids.begin(), g_kids.end(), c.id, [](const PachEntry& x, uint32_t id) { return x.id < id; });
        g_kids.insert(at, std::move(c));
        StorePach();
      }
    }
  }
}

}  // namespace

bool IsOpen() { return g_open; }

bool Open(Bytes pac, const std::string& label, const std::wstring& from, const std::string& note) {
  Epac e;
  if (!EpacRead(pac, e)) {
    Status("Not an EPAC / EPK8 pac: it can't be edited here.");
    return false;
  }
  g_pac = std::move(e);
  g_open = true;
  g_label = label;
  g_from = from;
  g_note = note;
  g_original_size = pac.size();
  g_changes = 0;
  g_changed.clear();
  ClosePach();
  Log("pac editor: " + label + " (" + Human(pac.size()) + ")");
  return true;
}

void Close() {
  g_open = false;
  g_pac = Epac();
  ClosePach();
}

void Draw() {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.15f);
  ImGui::Text("Editing a copy: %s", g_label.c_str());
  ImGui::PopFont();
  size_t entries = 0;
  for (const auto& g : g_pac.groups) entries += g.entries.size();
  ImGui::TextDisabled("%s, %zu group%s, %zu entr%s, %s%s%s", g_pac.epk8 ? "EPK8" : "EPAC", g_pac.groups.size(),
                      g_pac.groups.size() == 1 ? "" : "s", entries, entries == 1 ? "y" : "ies", Human(g_original_size).c_str(),
                      g_note.empty() ? "" : ", ", g_note.c_str());
  PushAccent();
  if (ImGui::Button("Save the copy as...")) SaveAs();
  PopAccent();
  ImGui::SameLine();
  if (ImGui::Button("Close")) {
    Close();
    return;
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(220 * g_scale);
  ImGui::Combo("Pack replaced data", &g_pack, "like the original (BPE if it was)\0always BPE (smaller)\0never (raw)\0");
  Hint("How data you put in is stored. The game reads both packed (BPE) and raw entries; packing makes the pac "
       "smaller. Data that is already packed is kept as it is.");
  ImGui::TextColored(g_changes ? kWarn : ImVec4(0.6f, 0.6f, 0.65f, 1), "%d change%s. Nothing is written until you save, and never into the game's pac folder.",
                     g_changes, g_changes == 1 ? "" : "s");
  ImGui::SetNextItemWidth(260 * g_scale);
  ImGui::InputTextWithHint("##find", "find an entry", g_find, sizeof g_find);
  const std::string find = Lower(g_find);
  const float rows_h = g_pach_g >= 0 ? ImGui::GetContentRegionAvail().y * 0.55f : 0;
  if (ImGui::BeginTable("entries", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY,
                        ImVec2(0, rows_h))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("group", ImGuiTableColumnFlags_WidthFixed, 60 * g_scale);
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, 110 * g_scale);
    ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed, 70 * g_scale);
    ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 90 * g_scale);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    std::pair<int, int> remove{-1, -1};
    int add_to = -1;
    for (size_t gi = 0; gi < g_pac.groups.size(); ++gi) {
      auto& g = g_pac.groups[gi];
      ImGui::PushID(int(gi));
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextColored(kWarn, "%s", g.type.c_str());
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%zu entries", g.entries.size());
      ImGui::TableNextColumn();
      ImGui::TableNextColumn();
      ImGui::TableNextColumn();
      if (ImGui::SmallButton("Add an entry...")) add_to = int(gi);
      for (size_t ei = 0; ei < g.entries.size(); ++ei) {
        auto& en = g.entries[ei];
        if (!find.empty() && Lower(en.name).find(find) == std::string::npos) continue;
        ImGui::PushID(int(ei));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        const bool changed = g_changed.count({int(gi), int(ei)});
        if (changed) ImGui::TextColored(kWarn, "%s *", en.name.c_str());
        else ImGui::TextUnformatted(en.name.c_str());
        ImGui::TableNextColumn();
        const std::string kind = Kind(en.data);
        ImGui::TextUnformatted(kind.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(Human(en.data.size()).c_str());
        ImGui::TableNextColumn();
        if (kind == "PACH") {
          const bool shown = g_pach_g == int(gi) && g_pach_e == int(ei);
          if (ImGui::SmallButton(shown ? "Hide children" : "Children")) {
            ClosePach();
            if (!shown) {
              g_pach_g = int(gi), g_pach_e = int(ei);
              g_pach_bpe = IsBpe(en.data);
              PachRead(g_pach_bpe ? Unpack(en.data) : en.data, g_kids);
            }
          }
          ImGui::SameLine();
        }
        if (ImGui::SmallButton("Export")) ExportBytes(en.data, g.type + "_" + en.name);
        ImGui::SameLine();
        if (ImGui::SmallButton("Replace...")) {
          Bytes b;
          if (PickData(b)) {
            if (g_pach_g == int(gi) && g_pach_e == int(ei)) ClosePach();
            en.data = Pack(b, IsBpe(en.data));
            Changed(int(gi), int(ei));
          }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Rename")) {
          std::snprintf(g_new_name, sizeof g_new_name, "%s", en.name.c_str());
          ImGui::OpenPopup("rename");
        }
        if (ImGui::BeginPopup("rename")) {
          const size_t max = g_pac.wide ? 8 : 4;
          ImGui::SetNextItemWidth(140 * g_scale);
          ImGui::InputText("##n", g_new_name, max + 1);
          ImGui::SameLine();
          if (ImGui::Button("OK")) {
            en.name = g_new_name;
            Changed(int(gi), int(ei));
            ImGui::CloseCurrentPopup();
          }
          ImGui::TextDisabled("Up to %zu characters: the game finds entries by name.", max);
          ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) remove = {int(gi), int(ei)};
        ImGui::PopID();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
    if (remove.first >= 0) {
      ClosePach();
      auto& v = g_pac.groups[size_t(remove.first)].entries;
      v.erase(v.begin() + remove.second);
      ++g_changes;
      std::set<std::pair<int, int>> moved;  // (marks after the removed row shift up)
      for (auto c : g_changed)
        if (c.first != remove.first || c.second < remove.second) moved.insert(c);
        else if (c.second > remove.second) moved.insert({c.first, c.second - 1});
      g_changed = std::move(moved);
    }
    if (add_to >= 0) {
      Bytes b;
      if (PickData(b)) {
        auto& v = g_pac.groups[size_t(add_to)].entries;
        EpacEntry en;
        const size_t max = g_pac.wide ? 8 : 4;
        en.name = "NEW" + std::to_string(v.size());
        if (en.name.size() > max) en.name.resize(max);
        en.data = Pack(b, !v.empty() && IsBpe(v.front().data));
        v.push_back(std::move(en));
        Changed(add_to, int(v.size() - 1));
        Status("Added as " + v.back().name + ": rename it to the name the game looks for.");
      }
    }
  }
  PachRows();
}

// --test-pac-edit <in>,<out>[,<group>/<entry>,<file>]: open, optionally replace one
// entry's data with a file (packed like the original), save; no dialogs.
void TestEdit(const std::wstring& spec) {
  std::vector<std::wstring> p;
  for (size_t at = 0; at <= spec.size();) {
    size_t c = spec.find(L',', at);
    if (c == std::wstring::npos) c = spec.size();
    p.push_back(spec.substr(at, c - at));
    at = c + 1;
  }
  Bytes d;
  if (p.size() < 2 || !ReadFile(Utf8(p[0]), d) || !Open(d, Utf8(p[0]), p[0], "")) {
    Log("pac edit test: could not open");
    return;
  }
  if (p.size() >= 4) {
    const std::string where = Utf8(p[2]);
    const size_t slash = where.find('/');
    Bytes b;
    bool done = false;
    if (slash != std::string::npos && ReadFile(Utf8(p[3]), b))
      for (size_t gi = 0; gi < g_pac.groups.size() && !done; ++gi)
        for (size_t ei = 0; ei < g_pac.groups[gi].entries.size() && !done; ++ei)
          if (g_pac.groups[gi].type == where.substr(0, slash) && g_pac.groups[gi].entries[ei].name == where.substr(slash + 1)) {
            g_pac.groups[gi].entries[ei].data = Pack(b, IsBpe(g_pac.groups[gi].entries[ei].data));
            Changed(int(gi), int(ei));
            done = true;
          }
    Log(std::string("pac edit test: replace ") + (done ? "done" : "NOT FOUND"));
  }
  if (InGamePac(p[1])) {
    Log("pac edit test: refused (the game's pac folder)");
    return;
  }
  const Bytes out = EpacWrite(g_pac);
  Log("pac edit test: " + std::string(WriteFile(Utf8(p[1]), out) ? "saved " : "could not save ") + Utf8(p[1]) +
      (out == d ? " (identical)" : " (changed)"));
}

}  // namespace pac_editor
}  // namespace mm
