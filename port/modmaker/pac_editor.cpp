// The pac editor (checklist D16; 2.1.2 "full EPAC support"): a copy of any pac -
// SvR 2007 to WWE 2K17, from this game, another disc or a file - edited in
// memory and saved somewhere else, in the file's own layout (svrfmt/archive.h).
// A tree of every level: EPAC / EPK8 (12-, 16- and 20-byte entries), WWE 2K17
// HSPC, and the lists inside entries (PACH, SHDC, nested EPAC / HSPC), BPE and
// ZLIB wrapping undone for viewing. Entries can be previewed (DDS, texture
// bundles, models, text), exported, replaced, removed, added and (EPAC)
// renamed. What can't be written is said where it applies: HSPC entries can't
// be added (their names are hashes nobody knows how to make), SHDC children
// only with an id whose hash was seen; xcompress files and ZLIB entries are
// written back stored (no LZX / deflate), the way WWE 2K14 stores some itself.
// The game's own pac folder is never written: Save as refuses it. A save with
// nothing changed gives the original bytes (svrmod archive-test, --test-pac-edit).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "app.h"
#include "svrfmt/archive.h"
#include "svrfmt/jboy.h"
#include "svrfmt/texture.h"
#include "svrfmt/wwe13.h"

namespace mm {
namespace pac_editor {

using namespace svrfmt;

namespace {

Archive g_arc;
bool g_open = false;
std::string g_label;       // what it is ("SvR 2008: pac/menu/MatchSD.pac")
std::wstring g_from;       // where it came from (for the default save name)
std::string g_note;        // from the caller
int g_changes = 0;
int g_pack = 0;            // 0 like the original, 1 always BPE, 2 never
std::vector<size_t> g_sel; // the selected node: indexes from the root (empty: the root)
bool g_sel_valid = false;
char g_find[48] = "";
char g_new_name[16] = "";
char g_add_group[8] = "";
char g_add_name[16] = "";
uint32_t g_add_id = 0;
bool g_open_sel = false;   // (a test's select=: the lists down to it shown open, once)
std::map<const ArcNode*, std::string> g_kind;  // ArchiveKindOf, cached (cleared on every change: nodes move)
// the preview of the selected node
const ArcNode* g_prev_for = nullptr;
size_t g_prev_size = 0;
Picture g_pic;
std::vector<Picture> g_thumbs;
std::vector<std::string> g_thumb_names;
std::string g_prev_text;
Bytes g_prev_data;

ArcNode* At(const std::vector<size_t>& path) {
  ArcNode* n = &g_arc.root;
  for (size_t i : path) {
    if (!n->parsed || i >= n->kids.size()) return nullptr;
    n = &n->kids[i];
  }
  return n;
}

std::string Label(const ArcNode& n) { return n.group.empty() ? n.name : n.group + "/" + n.name; }

std::string KindOf(const ArcNode& n) {
  auto it = g_kind.find(&n);
  if (it != g_kind.end()) return it->second;
  return g_kind[&n] = ArchiveKindOf(n);
}

void Changed() {
  ++g_changes;
  g_kind.clear();
  g_prev_for = nullptr;
}

// A warning line, wrapped.
void Note(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
  ImGui::TextWrapped("%s", text);
  ImGui::PopStyleColor();
}

const char* WrapName(ArcWrap w) { return w == ArcWrap::Bpe ? "BPE" : w == ArcWrap::Zlib ? "ZLIB" : ""; }

// New data for a leaf, packed per the "Pack replaced data" choice (ZLIB entries stay ZLIB).
void Replace(ArcNode& n, Bytes b) {
  if (IsBpe(b)) b = Unpack(b);  // (packed again below if it should be)
  if (n.wrap != ArcWrap::Zlib) n.wrap = g_pack == 1 ? ArcWrap::Bpe : g_pack == 2 ? ArcWrap::None : n.wrap;
  ArchiveReplace(n, b);
  Changed();
}

bool ExportBytes(const Bytes& b, const std::string& stem) {
  static const COMDLG_FILTERSPEC kAny[1] = {{L"All files", L"*.*"}};
  const std::wstring f = PickFile(true, L"Export", kAny, 1, L"bin", Wide(stem + ".bin").c_str());
  if (f.empty()) return false;
  const bool ok = WriteFile(Utf8(f), b);
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
  const Bytes out = ArchiveSave(g_arc);
  if (!WriteFile(Utf8(f), out)) {
    Status("Could not write " + Utf8(f));
    return;
  }
  Log("pac editor: saved " + Utf8(f) + " (" + Human(out.size()) + ", " + std::to_string(g_changes) + " changes)");
  Status("Saved " + Utf8(f) + (out == g_arc.original ? " (unchanged: the same bytes as the original)" : ""));
}

// ---- the tree

bool Matches(const ArcNode& n, const std::string& find) {
  if (find.empty() || Lower(Label(n)).find(find) != std::string::npos) return true;
  if (n.parsed)
    for (const auto& k : n.kids)
      if (Matches(k, find)) return true;
  return false;
}

void TreeRows(ArcNode& n, std::vector<size_t>& path, const std::string& find) {
  for (size_t i = 0; i < n.kids.size(); ++i) {
    ArcNode& k = n.kids[i];
    if (!Matches(k, find)) continue;
    path.push_back(i);
    ImGui::PushID(int(i));
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    const bool container = k.parsed && k.kind != ArcKind::Leaf;
    // (what an entry is needs it unpacked: only for rows on screen, then kept)
    const bool known = g_kind.count(&k) || container;
    const std::string kind = known || ImGui::IsRectVisible(ImVec2(1, ImGui::GetFrameHeight())) ? KindOf(k) : std::string();
    const bool maybe = container || kind == "PACH" || kind == "SHDC" || kind == "EPAC" || kind == "EPK8" || kind == "HSPC";
    ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (!maybe) fl |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (g_sel_valid && g_sel == path) fl |= ImGuiTreeNodeFlags_Selected;
    if (!find.empty() && maybe) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (g_open_sel && maybe && path.size() < g_sel.size() && std::equal(path.begin(), path.end(), g_sel.begin()))
      ImGui::SetNextItemOpen(true);
    const bool changed = k.dirty;
    if (changed) ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
    const bool open = ImGui::TreeNodeEx("n", fl, "%s%s", Label(k).c_str(), changed ? " *" : "");
    if (changed) ImGui::PopStyleColor();
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) g_sel = path, g_sel_valid = true;
    ImGui::TableNextColumn();
    ImGui::TextDisabled("%s%s%s", kind.c_str(), k.wrap != ArcWrap::None ? " " : "", WrapName(k.wrap));
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(Human(k.stored.size()).c_str());
    if (maybe && open) {
      if (!k.parsed && !ArchiveExpand(k)) ImGui::TextDisabled("(not a list it can read)");
      else TreeRows(k, path, find);
      ImGui::TreePop();
    }
    ImGui::PopID();
    path.pop_back();
  }
}

// ---- the selected node

void Hex(const Bytes& b) {
  std::string s;
  char line[100];
  for (size_t o = 0; o < std::min<size_t>(b.size(), 512); o += 16) {
    int p = std::snprintf(line, sizeof line, "%06zX ", o);
    for (size_t i = 0; i < 16; ++i) p += std::snprintf(line + p, sizeof line - size_t(p), o + i < b.size() ? " %02X" : "   ", o + i < b.size() ? b[o + i] : 0);
    p += std::snprintf(line + p, sizeof line - size_t(p), "  ");
    for (size_t i = 0; i < 16 && o + i < b.size(); ++i) line[p++] = b[o + i] >= 0x20 && b[o + i] < 0x7F ? char(b[o + i]) : '.';
    line[p] = 0;
    s += line;
    s += '\n';
  }
  ImGui::TextUnformatted(s.c_str());
}

void MakePreview(const ArcNode& n) {
  g_prev_for = &n;
  g_prev_size = n.stored.size();
  g_pic.Clear();
  g_thumbs.clear();
  g_thumb_names.clear();
  g_prev_text.clear();
  g_prev_data = ArchiveData(n);
  const Bytes& d = g_prev_data;
  const std::string kind = KindOf(n);
  if (kind == "DDS") {
    Image img;
    if (DdsDecode(d, img)) g_pic.Set(std::move(img));
  } else if (kind == "textures") {
    std::vector<BundleTexture> t;
    if (BundleRead(d, t))
      for (size_t i = 0; i < t.size() && i < 48; ++i) {
        Image img;
        g_thumbs.emplace_back();
        if (DdsDecode(t[i].data, img)) g_thumbs.back().Set(std::move(img));
        g_thumb_names.push_back(t[i].name);
      }
    g_prev_text = std::to_string(t.size()) + " textures" + (t.size() > 48 ? " (the first 48 shown)" : "") + ".";
  } else if (kind == "model") {
    Model m;
    std::string err;
    if (IsJboy13(d) ? Jboy13Read(d, m, &err) : JboyRead(d, m, &err)) {
      size_t verts = 0, tris = 0;
      for (const auto& me : m.meshes) {
        verts += me.verts.size();
        for (const auto& s : me.strips) tris += s.indices.size() > 2 ? s.indices.size() - 2 : 0;
      }
      char b[256];
      std::snprintf(b, sizeof b, "Model %s (%s layout): %zu meshes, %zu vertices, about %zu triangles, %zu bones, %zu textures.",
                    m.name.c_str(), IsJboy13(d) ? "WWE '13 - 2K17" : "SvR", m.meshes.size(), verts, tris, m.nodes.size(), m.textures.size());
      g_prev_text = b;
      for (size_t i = 0; i < m.textures.size() && i < 24; ++i) g_prev_text += (i ? ", " : "\nTextures: ") + m.textures[i];
    } else {
      g_prev_text = "A JBOY model that didn't read" + (err.empty() ? std::string(".") : ": " + err);
    }
  } else if (kind == "text") {
    g_prev_text.assign(d.begin(), d.begin() + ptrdiff_t(std::min<size_t>(d.size(), 20000)));
  }
}

void AddPanel(ArcNode& p) {
  ImGui::SeparatorText("Add an entry");
  if (p.kind == ArcKind::Hspc) {
    Note(("Read-only for adding: " + ArchiveCantAdd(p)).c_str());
    return;
  }
  if (p.kind == ArcKind::Epac) {
    const size_t max = ArchiveNameMax(p);
    ImGui::SetNextItemWidth(70 * g_scale);
    ImGui::InputTextWithHint("##grp", "group", g_add_group, 5);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * g_scale);
    ImGui::InputTextWithHint("##nm", "name", g_add_name, max + 1);
    ImGui::SameLine();
    if (ImGui::Button("Add from a file...")) {
      std::string why;
      Bytes b;
      if (PickData(b)) {
        if (IsBpe(b)) b = Unpack(b);
        if (ArchiveAdd(p, g_add_group, g_add_name, 0, b, &why)) Changed(), Status(std::string("Added ") + g_add_group + "/" + g_add_name + ".");
        else Status("Not added: " + why);
      }
    }
    ImGui::TextDisabled("Group: 4 characters (an existing group's type, or a new one). Name: up to %zu characters - the game finds entries by name.", max);
    return;
  }
  ImGui::SetNextItemWidth(130 * g_scale);
  ImGui::InputScalar("id (hex)##addid", ImGuiDataType_U32, &g_add_id, nullptr, nullptr, "%X", ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  const std::string cant = ArchiveCantAdd(p, uint32_t(g_add_id));
  ImGui::BeginDisabled(!cant.empty());
  if (ImGui::Button("Add from a file...")) {
    std::string why;
    Bytes b;
    if (PickData(b)) {
      if (IsBpe(b)) b = Unpack(b);
      if (ArchiveAdd(p, "", "", uint32_t(g_add_id), b, &why)) Changed(), Status("Added the child " + std::to_string(g_add_id) + ".");
      else Status("Not added: " + why);
    }
  }
  ImGui::EndDisabled();
  if (!cant.empty()) Note(cant.c_str());
  else ImGui::TextDisabled("Children are found by id; they are kept in id order.");
}

void SelectedPanel() {
  ArcNode* n = g_sel_valid ? At(g_sel) : nullptr;
  if (!n) {
    ImGui::TextDisabled("Pick an entry in the tree. Lists inside entries (PACH, SHDC, ...) open with their arrow.");
    if (g_arc.root.parsed) AddPanel(g_arc.root);
    return;
  }
  std::vector<size_t> up(g_sel.begin(), g_sel.end() - 1);
  ArcNode* parent = At(up);
  std::string path;
  {
    ArcNode* w = &g_arc.root;
    for (size_t i : g_sel) w = &w->kids[i], path += "/" + Label(*w);
  }
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.1f);
  ImGui::TextUnformatted(path.c_str());
  ImGui::PopFont();
  const std::string kind = KindOf(*n);
  std::string info = kind + ", " + Human(n->stored.size()) + " stored";
  if (n->wrap == ArcWrap::Bpe) info += " (BPE packed)";
  if (n->wrap == ArcWrap::Zlib) info += n->stored.size() >= 16 && Le32(&n->stored[8]) == Le32(&n->stored[12]) ? " (ZLIB, stored)" : " (ZLIB packed)";
  if (n->hash && parent && parent->kind == ArcKind::Hspc) info += ", name hash " + n->name.substr(1);
  if (n->hash && parent && parent->kind == ArcKind::Shdc) {
    char b[40];
    std::snprintf(b, sizeof b, ", id hash %016llx", static_cast<unsigned long long>(n->hash));
    info += b;
  }
  if (g_arc.root.epac.e20 && parent == &g_arc.root) info += ", tail " + std::to_string(n->tail);
  ImGui::TextDisabled("%s", info.c_str());
  if (ImGui::Button("Export...")) ExportBytes(ArchiveData(*n), Label(*n));
  ImGui::SameLine();
  Hint("Exports the data unpacked (BPE / ZLIB undone). \"Export as stored\" gives the bytes as they are in the pac.");
  ImGui::SameLine();
  if (ImGui::Button("Export as stored...")) ExportBytes(n->stored, Label(*n) + "_stored");
  ImGui::SameLine();
  if (ImGui::Button("Replace...")) {
    Bytes b;
    if (PickData(b)) Replace(*n, std::move(b)), Status("Replaced " + path + ".");
  }
  if (parent) {
    ImGui::SameLine();
    if (ImGui::Button("Remove")) {
      ArchiveRemove(*parent, g_sel.back());
      Changed();
      g_sel_valid = false;
      Status("Removed " + path + ".");
      return;
    }
    if (parent->kind == ArcKind::Epac) {
      ImGui::SameLine();
      if (ImGui::Button("Rename")) {
        std::snprintf(g_new_name, sizeof g_new_name, "%s", n->name.c_str());
        ImGui::OpenPopup("rename");
      }
      if (ImGui::BeginPopup("rename")) {
        const size_t max = ArchiveNameMax(*parent);
        ImGui::SetNextItemWidth(140 * g_scale);
        ImGui::InputText("##n", g_new_name, max + 1);
        ImGui::SameLine();
        if (ImGui::Button("OK")) {
          std::string why;
          if (ArchiveRename(*parent, g_sel.back(), g_new_name, &why)) Changed();
          else Status("Not renamed: " + why);
          ImGui::CloseCurrentPopup();
        }
        ImGui::TextDisabled("Up to %zu characters: the game finds entries by name.", max);
        ImGui::EndPopup();
      }
    }
  }
  if (n->parsed && n->kind != ArcKind::Leaf) {
    ImGui::Text("A list of %zu entries (%s). Open it in the tree.", n->kids.size(), kind.c_str());
    AddPanel(*n);
    return;
  }
  if (g_prev_for != n || g_prev_size != n->stored.size()) MakePreview(*n);
  ImGui::Separator();
  if (!g_pic.Empty()) {
    ImGui::Text("%d x %d", g_pic.image.w, g_pic.image.h);
    const float aw = ImGui::GetContentRegionAvail().x, s = std::min(1.0f, aw / std::max(1, g_pic.image.w));
    ImGui::Image(g_pic.Id(), ImVec2(g_pic.image.w * s, g_pic.image.h * s));
  } else if (!g_thumbs.empty()) {
    ImGui::TextUnformatted(g_prev_text.c_str());
    const float tw = 96 * g_scale;
    const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (tw + 10 * g_scale)));
    for (size_t i = 0; i < g_thumbs.size(); ++i) {
      if (i % cols) ImGui::SameLine();
      ImGui::BeginGroup();
      if (!g_thumbs[i].Empty()) ImGui::Image(g_thumbs[i].Id(), ImVec2(tw, tw * g_thumbs[i].image.h / std::max(1, g_thumbs[i].image.w)));
      else ImGui::Button("?", ImVec2(tw, tw / 2));
      const ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::PushClipRect(p, ImVec2(p.x + tw, p.y + ImGui::GetTextLineHeight()), true);
      ImGui::TextUnformatted(g_thumb_names[i].c_str());
      ImGui::PopClipRect();
      ImGui::EndGroup();
    }
  } else if (!g_prev_text.empty()) {
    ImGui::BeginChild("txt", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(g_prev_text.c_str());
    if (kind == "model") {
      ImGui::Spacing();
      ImGui::TextDisabled("SvR 2011 character models open on the Animations page; other games' superstars port on the Port page.");
    }
    ImGui::EndChild();
  } else {
    Hex(g_prev_data);
  }
}

// What can't be written, said plainly (read-only parts, stored compression).
void Notes() {
  bool hspc = g_arc.root.kind == ArcKind::Hspc, zlib = false, shdc = false;
  std::vector<const ArcNode*> st{&g_arc.root};
  while (!st.empty()) {
    const ArcNode* n = st.back();
    st.pop_back();
    hspc |= n->kind == ArcKind::Hspc && n->parsed;
    shdc |= n->kind == ArcKind::Shdc && n->parsed;
    zlib |= n->wrap == ArcWrap::Zlib;
    if (n->parsed)
      for (const auto& k : n->kids) st.push_back(&k);
  }
  if (g_arc.xcompress)
    Note("xcompress: saved xcompressed again with stored chunks (no LZX packing), like WWE 2K14's own files - the games "
                              "and the converters read them, but a changed file gets bigger.");
  if (zlib)
    Note("ZLIB entries you change are saved stored (not deflated), a form the games use themselves; unchanged ones keep their bytes.");
  if (hspc)
    Note("HSPC (WWE 2K17): entries can be replaced and removed but not added - read-only for adding (names are hashes nobody can make).");
  if (shdc)
    Note("SHDC: children can be added only with an id whose hash is known (seen in an SHDC opened so far).");
  if (!g_note.empty()) ImGui::TextDisabled("%s", g_note.c_str());
}

}  // namespace

bool IsOpen() { return g_open; }

bool Open(Bytes pac, const std::string& label, const std::wstring& from, const std::string& note) {
  std::string err;
  Archive a;
  if (!ArchiveOpen(pac, a, &err)) {
    Status("Not a pac this editor reads (" + err + ").");
    return false;
  }
  g_arc = std::move(a);
  g_open = true;
  g_label = label;
  g_from = from;
  g_note = note;
  g_changes = 0;
  g_sel.clear();
  g_sel_valid = false;
  g_kind.clear();
  g_prev_for = nullptr;
  Log("pac editor: " + label + " (" + g_arc.format + ", " + Human(pac.size()) + ")");
  return true;
}

void Close() {
  g_open = false;
  g_arc = Archive();
  g_kind.clear();
  g_prev_for = nullptr;
  g_pic.Clear();
  g_thumbs.clear();
}

void Draw() {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.15f);
  ImGui::Text("Editing a copy: %s", g_label.c_str());
  ImGui::PopFont();
  ImGui::TextDisabled("%s, %zu entries, %s", g_arc.format.c_str(), g_arc.root.kids.size(), Human(g_arc.original.size()).c_str());
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
       "smaller. ZLIB entries stay ZLIB.");
  ImGui::TextColored(g_changes ? kWarn : ImVec4(0.6f, 0.6f, 0.65f, 1),
                     "%d change%s. Nothing is written until you save, and never into the game's pac folder. Saved in the pac's own layout.",
                     g_changes, g_changes == 1 ? "" : "s");
  Notes();
  ImGui::SetNextItemWidth(260 * g_scale);
  ImGui::InputTextWithHint("##find", "find an entry (in opened lists)", g_find, sizeof g_find);
  const std::string find = Lower(g_find);
  const float left = ImGui::GetContentRegionAvail().x * 0.48f;
  ImGui::BeginChild("tree", ImVec2(left, 0), true);
  if (ImGui::BeginTable("entries", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("entry", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed, 100 * g_scale);
    ImGui::TableSetupColumn("size", ImGuiTableColumnFlags_WidthFixed, 75 * g_scale);
    ImGui::TableHeadersRow();
    std::vector<size_t> path;
    TreeRows(g_arc.root, path, find);
    ImGui::EndTable();
    g_open_sel = false;
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginChild("sel", ImVec2(0, 0), true);
  SelectedPanel();
  ImGui::EndChild();
}

// --test-pac-edit <in>,<out>[,<op>...]: open, apply the ops, save (no dialogs; the game's pac folder refused).
// ops: replace=<path>=<file> | remove=<path> | add=<list path>=<name or group/name or id>=<file> |
// rename=<path>=<name> | select=<path> (shown in the editor, for --shot). A path is labels from the top
// joined by '>' ("MENU/SSFD>0x0>0x64"; "" = the top list). The old form <in>,<out>,<group>/<entry>,<file> replaces.
void TestEdit(const std::wstring& spec) {
  std::vector<std::string> p;
  const std::string s = Utf8(spec);
  for (size_t at = 0; at <= s.size();) {
    size_t c = s.find(',', at);
    if (c == std::string::npos) c = s.size();
    p.push_back(s.substr(at, c - at));
    at = c + 1;
  }
  Bytes d;
  if (p.size() < 2 || !ReadFile(p[0], d) || !Open(d, p[0], Wide(p[0]), "")) {
    Log("pac edit test: could not open");
    return;
  }
  if (p.size() == 4 && p[2].find('=') == std::string::npos) p = {p[0], p[1], "replace=" + p[2] + "=" + p[3]};
  auto split = [](const std::string& v, char sep) {
    std::vector<std::string> r;
    for (size_t at = 0; at <= v.size();) {
      size_t c = v.find(sep, at);
      if (c == std::string::npos) c = v.size();
      r.push_back(v.substr(at, c - at));
      at = c + 1;
    }
    return r;
  };
  // a path -> indexes (lists opened on the way)
  auto find = [&](const std::string& path, std::vector<size_t>& out) {
    out.clear();
    ArcNode* n = &g_arc.root;
    if (path.empty()) return true;
    for (const auto& label : split(path, '>')) {
      if (!n->parsed && !ArchiveExpand(*n)) return false;
      size_t i = 0;
      while (i < n->kids.size() && Label(n->kids[i]) != label) ++i;
      if (i == n->kids.size()) return false;
      out.push_back(i);
      n = &n->kids[i];
    }
    return true;
  };
  for (size_t k = 2; k < p.size(); ++k) {
    const auto op = split(p[k], '=');
    std::vector<size_t> at;
    std::string result;
    if (op.size() < 2 || !find(op[1], at)) result = "NOT FOUND";
    else if (op[0] == "replace" && op.size() == 3) {
      Bytes b;
      if (!ReadFile(op[2], b)) result = "no file";
      else Replace(*At(at), std::move(b)), result = "done";
    } else if (op[0] == "remove" && !at.empty()) {
      std::vector<size_t> up(at.begin(), at.end() - 1);
      ArchiveRemove(*At(up), at.back()), Changed(), result = "done";
    } else if (op[0] == "rename" && op.size() == 3 && !at.empty()) {
      std::vector<size_t> up(at.begin(), at.end() - 1);
      std::string why;
      if (ArchiveRename(*At(up), at.back(), op[2], &why)) Changed(), result = "done";
      else result = "refused: " + why;
    } else if (op[0] == "add" && op.size() == 4) {
      ArcNode* list = At(at);
      Bytes b;
      std::string why, group, name = op[2];
      if (list && !list->parsed) ArchiveExpand(*list);
      const size_t slash = name.find('/');
      if (slash != std::string::npos) group = name.substr(0, slash), name = name.substr(slash + 1);
      if (!list || !ReadFile(op[3], b)) result = "no list / file";
      else if (ArchiveAdd(*list, group, name, uint32_t(std::strtoul(name.c_str(), nullptr, 0)), b, &why)) Changed(), result = "done";
      else result = "refused: " + why;
    } else if (op[0] == "select") {
      g_sel = at, g_sel_valid = !at.empty(), g_open_sel = true, result = "done";
    } else {
      result = "bad op";
    }
    Log("pac edit test: " + p[k] + ": " + result);
  }
  if (InGamePac(Wide(p[1]))) {
    Log("pac edit test: refused (the game's pac folder)");
    return;
  }
  const Bytes out = ArchiveSave(g_arc);
  Log("pac edit test: " + std::string(WriteFile(p[1], out) ? "saved " : "could not save ") + p[1] + " (" + g_arc.format + ", " +
      std::to_string(out.size()) + " bytes" + (out == d ? ", identical)" : ", changed)"));
}

}  // namespace pac_editor
}  // namespace mm
