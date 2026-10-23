// The material panel (Superstar page, Simple's Look / Attires, Attires page; the entrance gear when the 3D view
// shows it): per mesh of the model in the 3D view its shine, normal map, colour, see-through cut-out and both
// sides - the game's own per-mesh values (model_materials.h), previewed live, written into the pac at save.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include "app.h"
#include "char_preview.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "model_materials.h"

namespace fs = std::filesystem;

namespace mm {

namespace {
struct Cache {
  std::wstring pac;
  int attire = 0;
  bool gear = false;
  fs::file_time_type when;
  std::vector<MeshMat> meshes;
};
Cache g_cache;
std::pair<uint32_t, int> g_atlas_for{~0u, -1};  // the mesh whose picture sheet is open
bool g_atlas_normal = false;
std::set<MatKey> g_sel;   // the parts picked to change together (their boxes; Ctrl / Shift click)
int g_sel_anchor = -1;    // (Shift: the range from the last part clicked)
std::string g_test_select;  // --mat-select: parts picked and changed together as if by hand (test aid)

// A checkbox that also shows "some on, some off" (state 2): clicking it sets them all on.
bool MixedCheckbox(const char* label, int state, bool* out) {
  bool b = state == 1;
  ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, state == 2);
  const bool hit = ImGui::Checkbox(label, &b);
  ImGui::PopItemFlag();
  if (hit) *out = state == 2 ? true : b;
  return hit;
}

const std::vector<MeshMat>& MeshesOf(const std::wstring& pac, int attire, bool gear) {
  std::error_code ec;
  const auto when = fs::last_write_time(pac, ec);
  if (g_cache.pac != pac || g_cache.attire != attire || g_cache.gear != gear || g_cache.when != when) {
    g_cache = Cache{pac, attire, gear, when, {}};
    svrfmt::Bytes b;
    if (svrfmt::ReadFile(Utf8(pac), b)) ReadMaterials(b, attire, gear, g_cache.meshes);
  }
  return g_cache.meshes;
}

// A part's name for people: by the model's place in the attire (the stock models' own names are terse or odd -
// many bodies are called "EYE"), else its name.
std::string PartName(const MeshMat& m) {
  const std::string n = Lower(m.model);
  if (m.model_id == 0) return "Body";
  if (m.model_id == 10000 || n.find("hair") != std::string::npos) return "Hair";
  if (n.find("lash") != std::string::npos || n.find("matsuge") != std::string::npos) return "Eyelashes";
  if (n.find("hige") != std::string::npos) return "Beard";
  if (m.model_id == 20000) return "Face extras";
  return m.model.empty() ? "Part" : m.model;
}

// A small picture of the part: its area of the picture sheet (its UVs' box). False when the 3D view has none.
bool PartThumb(uint32_t id, int mesh, float side) {
  void* tex = nullptr;
  std::vector<std::array<float, 2>> uvs;
  if (!char_preview::MeshAtlas(id, mesh, false, &tex, &uvs) || !tex || uvs.empty()) return false;
  float lo[2] = {1, 1}, hi[2] = {0, 0};
  for (const auto& uv : uvs)
    for (int k = 0; k < 2; ++k) {
      const float f = uv[size_t(k)] - std::floor(uv[size_t(k)]);  // (tiling: wrapped into the sheet)
      lo[k] = std::min(lo[k], f), hi[k] = std::max(hi[k], f);
    }
  if (hi[0] <= lo[0] || hi[1] <= lo[1]) return false;
  ImGui::Image(ImTextureID(tex), ImVec2(side, side), ImVec2(lo[0], lo[1]), ImVec2(hi[0], hi[1]));
  return true;
}

// The mesh's picture sheet with its own area outlined (its triangles in UV space).
void AtlasView(uint32_t id, int mesh, bool normal) {
  void* tex = nullptr;
  std::vector<std::array<float, 2>> uvs;
  if (!char_preview::MeshAtlas(id, mesh, normal, &tex, &uvs) || !tex) {
    ImGui::TextDisabled(normal ? "No normal map." : "The picture isn't loaded in the 3D view.");
    return;
  }
  const float side = std::min(ImGui::GetContentRegionAvail().x, 320 * g_scale);
  const ImVec2 o = ImGui::GetCursorScreenPos();
  ImGui::Image(ImTextureID(tex), ImVec2(side, side));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(o, ImVec2(o.x + side, o.y + side), true);
  auto at = [&](const std::array<float, 2>& uv) {
    const float u = uv[0] - std::floor(uv[0]), v = uv[1] - std::floor(uv[1]);  // (tiling: wrapped into the sheet)
    return ImVec2(o.x + u * side, o.y + v * side);
  };
  for (size_t t = 0; t + 2 < uvs.size(); t += 3) {
    const ImVec2 a = at(uvs[t]), b = at(uvs[t + 1]), c = at(uvs[t + 2]);
    dl->AddTriangleFilled(a, b, c, IM_COL32(255, 80, 110, 50));
    dl->AddTriangle(a, b, c, IM_COL32(255, 80, 110, 160));
  }
  dl->PopClipRect();
}
// The parts the panel lists: blood overlays left out.
std::vector<const MeshMat*> Shown(const std::vector<MeshMat>& meshes) {
  std::vector<const MeshMat*> v;
  for (const MeshMat& m : meshes)
    if (!(Lower(m.model).rfind("blood", 0) == 0 || (m.model_id >= 0x7530 && m.model_id <= 0x7532))) v.push_back(&m);
  return v;
}

// One change made to every picked part (g_sel) of those shown: each its own edit, emptied ones dropped.
using PartChange = std::function<void(MatEdit&, const MeshMat&)>;
void ChangePicked(const std::vector<const MeshMat*>& shown, int view_attire, bool gear, std::map<MatKey, MatEdit>& edits, const PartChange& f) {
  for (const MeshMat* m : shown) {
    const MatKey key{view_attire, gear, m->model_id, m->mesh};
    if (!g_sel.count(key)) continue;
    MatEdit e = edits.count(key) ? edits[key] : MatEdit();
    f(e, *m);
    if (e.Empty()) edits.erase(key);
    else edits[key] = e;
  }
}

// Test aid (--mat-select): "all" or "<model id>:<mesh>/..." picked, then ":spec=..,tint=r/g/b,cut=,both=,flip=" made
// to all of them the way the panel's "all picked" row does. True when it ran.
bool RunTestSelect(const std::vector<const MeshMat*>& shown, int view_attire, bool gear, std::map<MatKey, MatEdit>& edits) {
  if (g_test_select.empty() || shown.empty()) return false;
  const std::string t = g_test_select;
  g_test_select.clear();
  const size_t at = t.find('=') == std::string::npos ? t.size() : t.rfind(':', t.find('='));
  const std::string which = t.substr(0, at), what = at < t.size() ? t.substr(at + 1) : "";
  g_sel.clear();
  for (const MeshMat* m : shown) {
    const std::string id = std::to_string(m->model_id) + ":" + std::to_string(m->mesh);
    if (which == "all" || ("/" + which + "/").find("/" + id + "/") != std::string::npos) g_sel.insert(MatKey{view_attire, gear, m->model_id, m->mesh});
  }
  Log("test: " + std::to_string(g_sel.size()) + " parts picked (" + which + ")");
  auto apply = [&](const PartChange& f) { ChangePicked(shown, view_attire, gear, edits, f); };
  for (size_t p = 0; p < what.size();) {
    size_t q = what.find(',', p);
    if (q == std::string::npos) q = what.size();
    const std::string kv = what.substr(p, q - p);
    p = q + 1;
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = kv.substr(0, eq);
    const float v = float(std::atof(kv.c_str() + eq + 1));
    float c[3] = {1, 1, 1};
    if (k == "spec") apply([v](MatEdit& e, const MeshMat& m) { if (m.has_spec) e.has_spec = true, e.spec = v; });
    if (k == "tint" && std::sscanf(kv.c_str() + eq + 1, "%f/%f/%f", &c[0], &c[1], &c[2]) == 3)
      apply([c](MatEdit& e, const MeshMat& m) { if (m.has_tint) e.has_tint = true, std::copy(c, c + 3, e.tint); });
    if (k == "cut") apply([v](MatEdit& e, const MeshMat&) { e.cutout = int(v); });
    if (k == "both") apply([v](MatEdit& e, const MeshMat&) { e.two_sided = int(v); });
    if (k == "flip") apply([v](MatEdit& e, const MeshMat&) { e.flip = v != 0 ? 1 : -1; });
    Log("test: picked parts " + kv);
  }
  return true;
}
}  // namespace

void MaterialTestSelect(const std::string& spec) { g_test_select = spec; }
bool MaterialTestApply(const std::wstring& pac, int attire_in_pac, bool gear, int view_attire, std::map<MatKey, MatEdit>& edits) {
  if (g_test_select.empty() || pac.empty()) return false;
  return RunTestSelect(Shown(MeshesOf(pac, attire_in_pac, gear)), view_attire, gear, edits);
}

bool MaterialPanel(const std::wstring& pac, int attire_in_pac, bool gear, int view_attire, std::map<MatKey, MatEdit>& edits, bool simple) {
  bool changed = false;
  std::map<std::pair<uint32_t, int>, MatEdit> preview;
  for (const auto& [k, e] : edits)
    if (std::get<0>(k) == view_attire && std::get<1>(k) == gear) preview[{std::get<2>(k), std::get<3>(k)}] = e;
  if (pac.empty()) {
    char_preview::SetMaterialPreview({});
    ImGui::TextDisabled("Pick a model first.");
    return false;
  }
  const std::vector<MeshMat>& meshes = MeshesOf(pac, attire_in_pac, gear);
  if (meshes.empty()) {
    char_preview::SetMaterialPreview({});
    ImGui::TextDisabled("This model's materials couldn't be read.");
    return false;
  }
  ImGui::TextDisabled(simple ? "How each part of the model looks: shine, bumps, colour. The 3D view shows it as you go."
                             : "Per mesh, as the game keeps it (the model in the 3D view%s). The 3D view shows each change.",
                      gear ? ": the entrance gear" : "");
  std::pair<uint32_t, int> hover{~0u, -1};
  // the parts shown (blood overlays left out), and the picked ones among them
  std::vector<const MeshMat*> shown;
  shown = Shown(meshes);
  auto key_of = [&](const MeshMat& m) { return MatKey{view_attire, gear, m.model_id, m.mesh}; };
  {
    std::set<MatKey> keep;  // (another attire or the gear in view: its picks go)
    for (const MeshMat* m : shown)
      if (g_sel.count(key_of(*m))) keep.insert(key_of(*m));
    g_sel.swap(keep);
  }
  // every picked part changed the same way (its edit kept apart from the others' values)
  auto apply = [&](const PartChange& f) {
    ChangePicked(shown, view_attire, gear, edits, f);
    for (const MeshMat* m : shown)
      if (g_sel.count(key_of(*m))) preview[{m->model_id, m->mesh}] = edits.count(key_of(*m)) ? edits[key_of(*m)] : MatEdit();
    changed = true;
  };
  if (RunTestSelect(shown, view_attire, gear, edits)) {
    changed = true;
    for (const MeshMat* m : shown) preview[{m->model_id, m->mesh}] = edits.count(key_of(*m)) ? edits[key_of(*m)] : MatEdit();
  }
  if (shown.size() > 1) {  // picking several parts: one change for all of them
    if (ImGui::SmallButton("Select all")) {
      for (const MeshMat* m : shown) g_sel.insert(key_of(*m));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_sel.empty());
    if (ImGui::SmallButton("Select none")) g_sel.clear(), g_sel_anchor = -1;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(g_sel.size() > 1 ? "%d parts picked: the changes below go to all of them" : "Tick parts (Shift: a run of them) to change several at once",
                        int(g_sel.size()));
  }
  if (g_sel.size() > 1) {
    ImGui::PushID("allpicked");
    ImGui::Indent();
    std::vector<const MeshMat*> picked;
    for (const MeshMat* m : shown)
      if (g_sel.count(key_of(*m))) picked.push_back(m);
    auto edit_of = [&](const MeshMat& m) { const MatKey k = key_of(m); return edits.count(k) ? edits[k] : MatEdit(); };
    // shine: the same on all of them, else "mixed" until it is moved
    std::vector<float> specs;
    for (const MeshMat* m : picked)
      if (m->has_spec) { const MatEdit e = edit_of(*m); specs.push_back(e.has_spec ? e.spec : m->spec); }
    if (!specs.empty()) {
      const bool same = std::all_of(specs.begin(), specs.end(), [&](float v) { return std::fabs(v - specs[0]) < 1e-4f; });
      float v = specs[0];
      ImGui::SetNextItemWidth(220 * g_scale);
      if (ImGui::SliderFloat(simple ? "Shine (all picked)" : "Shine (specular level, all picked)", &v, 0, 4, same ? "%.2f" : "mixed",
                             ImGuiSliderFlags_Logarithmic))
        apply([v](MatEdit& e, const MeshMat& m) { if (m.has_spec) e.has_spec = true, e.spec = v; });
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Sets the shine of every picked part (the ones that have one). \"mixed\": they differ now.");
    }
    // colour
    std::vector<std::array<float, 3>> tints;
    for (const MeshMat* m : picked)
      if (m->has_tint) {
        const MatEdit e = edit_of(*m);
        tints.push_back(e.has_tint ? std::array<float, 3>{e.tint[0], e.tint[1], e.tint[2]} : std::array<float, 3>{m->tint[0], m->tint[1], m->tint[2]});
      }
    if (!tints.empty()) {
      const bool same = std::all_of(tints.begin(), tints.end(), [&](const std::array<float, 3>& t) {
        return std::fabs(t[0] - tints[0][0]) < 1e-4f && std::fabs(t[1] - tints[0][1]) < 1e-4f && std::fabs(t[2] - tints[0][2]) < 1e-4f;
      });
      float c[3] = {tints[0][0], tints[0][1], tints[0][2]};
      ImGui::SetNextItemWidth(220 * g_scale);
      if (ImGui::ColorEdit3(simple ? "Colour (all picked)" : "Colour (multiplier, all picked)", c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
        apply([c](MatEdit& e, const MeshMat& m) { if (m.has_tint) e.has_tint = true, std::copy(c, c + 3, e.tint); });
      if (!same) ImGui::SameLine(), ImGui::TextDisabled("mixed");
    }
    // bumps
    if (std::any_of(picked.begin(), picked.end(), [](const MeshMat* m) { return m->normal_slot; })) {
      ImGui::TextUnformatted(simple ? "Bumps (all picked):" : "Normal map (all picked):");
      ImGui::SameLine();
      if (ImGui::SmallButton("Pick...")) {
        static const COMDLG_FILTERSPEC kPics[1] = {{L"Pictures", L"*.png;*.dds;*.tga;*.jpg;*.bmp"}};
        const std::wstring f = PickFile(false, L"A normal map picture for every picked part (PNG, DDS ...)", kPics, 1);
        if (!f.empty()) apply([f](MatEdit& e, const MeshMat& m) { if (m.normal_slot) e.normal = 1, e.normal_file = f; });
      }
      ImGui::SameLine();
      if (ImGui::SmallButton(simple ? "Smooth" : "Flat")) apply([](MatEdit& e, const MeshMat& m) { if (m.normal_slot) e.normal = 2, e.normal_file.clear(); });
      ImGui::SameLine();
      if (ImGui::SmallButton("As it was")) apply([](MatEdit& e, const MeshMat&) { e.normal = 0, e.normal_file.clear(); });
    }
    // see-through, both sides, flip, flat look: on / off / "some of them" (mixed)
    auto state = [&](const std::function<bool(const MatEdit&, const MeshMat&)>& is, bool only_spec) {
      int on = 0, n = 0;
      for (const MeshMat* m : picked)
        if (!only_spec || m->has_spec) ++n, on += is(edit_of(*m), *m) ? 1 : 0;
      return on == 0 ? 0 : on == n ? 1 : 2;
    };
    bool b = false;
    if (MixedCheckbox(simple ? "See-through where clear" : "See-through cut-out",
                      state([](const MatEdit& e, const MeshMat& m) { return e.cutout >= 0 ? e.cutout != 0 : (m.flags & 0x100) != 0; }, false), &b))
      apply([b](MatEdit& e, const MeshMat&) { e.cutout = b; });
    ImGui::SameLine();
    if (MixedCheckbox(simple ? "Both sides" : "Double-sided",
                      state([](const MatEdit& e, const MeshMat& m) { return e.two_sided >= 0 ? e.two_sided != 0 : (m.flags & 0x2) == 0; }, false), &b))
      apply([b](MatEdit& e, const MeshMat&) { e.two_sided = b; });
    ImGui::SameLine();
    if (MixedCheckbox("Flip faces", state([](const MatEdit& e, const MeshMat&) { return e.flip == 1; }, false), &b))
      apply([b](MatEdit& e, const MeshMat&) { e.flip = b ? 1 : -1; });
    if (!specs.empty()) {
      ImGui::SameLine();
      if (MixedCheckbox("Flat look", state([](const MatEdit& e, const MeshMat& m) { return e.has_spec && e.spec <= 0.0f && (e.normal == 2 || !m.normal_slot); }, true), &b))
        apply([b](MatEdit& e, const MeshMat& m) {
          if (!m.has_spec) return;
          if (b) e.has_spec = true, e.spec = 0, e.normal = m.normal_slot ? 2 : e.normal, e.normal_file.clear();
          else e.has_spec = false, e.normal = 0;
        });
    }
    if (std::any_of(picked.begin(), picked.end(), [&](const MeshMat* m) { return !edit_of(*m).Empty(); })) {
      if (ImGui::SmallButton("Undo all (picked parts)")) apply([](MatEdit& e, const MeshMat&) { e = MatEdit(); });
    }
    ImGui::Unindent();
    ImGui::PopID();
    ImGui::Separator();
  }
  for (int row = 0; row < int(shown.size()); ++row) {
    const MeshMat& m = *shown[size_t(row)];
    const MatKey key{view_attire, gear, m.model_id, m.mesh};
    MatEdit e = edits.count(key) ? edits[key] : MatEdit();
    char label[160];
    std::snprintf(label, sizeof label, "%s, part %d%s###m%u_%d", PartName(m).c_str(), m.mesh + 1, e.Empty() ? "" : "  - changed",
                  unsigned(m.model_id), m.mesh);
    // its box: picked to change with the others (Shift: every part from the last one clicked)
    auto pick = [&](bool on) {
      const ImGuiIO& io = ImGui::GetIO();
      if (io.KeyShift && g_sel_anchor >= 0 && g_sel_anchor < int(shown.size())) {
        for (int r = std::min(row, g_sel_anchor); r <= std::max(row, g_sel_anchor); ++r) g_sel.insert(key_of(*shown[size_t(r)]));
      } else if (on) {
        g_sel.insert(key);
      } else {
        g_sel.erase(key);
      }
      g_sel_anchor = row;
    };
    if (shown.size() > 1) {
      bool on = g_sel.count(key) > 0;
      ImGui::PushID(row);
      if (ImGui::Checkbox("##pick", &on)) pick(on);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick it to change it together with the other picked parts (Shift: a run of parts).");
      ImGui::PopID();
      ImGui::SameLine();
    }
    if (PartThumb(m.model_id, m.mesh, ImGui::GetFrameHeight())) ImGui::SameLine();
    const bool was_open = ImGui::TreeNodeGetOpen(ImGui::GetID(label));
    bool node = ImGui::TreeNode(label);
    if (shown.size() > 1 && ImGui::IsItemClicked() && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift)) {
      // Ctrl / Shift click on the name: picks it (as its box), leaving it folded as it was
      ImGui::TreeNodeSetOpen(ImGui::GetItemID(), was_open);
      pick(!g_sel.count(key) || ImGui::GetIO().KeyShift);
      if (node && !was_open) ImGui::TreePop(), node = false;
    }
    if (ImGui::IsItemHovered() || node) hover = {m.model_id, m.mesh};  // (lit up in the 3D view)
    if (!node) continue;
    ImGui::PushID(label);
    bool ch = false;
    // (the game draws a culled skin part with its skin shader: the shine follows the normal map's alpha there)
    const bool skin = m.skin_shine && (e.two_sided >= 0 ? e.two_sided == 0 : (m.flags & 0x2) != 0) && e.cutout != 1;
    // shine
    if (m.has_spec) {
      float v = e.has_spec ? e.spec : m.spec;
      ImGui::SetNextItemWidth(220 * g_scale);
      if (ImGui::SliderFloat(simple ? "Shine" : "Shine (specular level)", &v, 0, 4, "%.2f", ImGuiSliderFlags_Logarithmic))
        e.has_spec = true, e.spec = v, ch = true;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip(skin ? "How glossy it is (the game's g_fSpecularLev: skin is about 0.5). Skin: the bumps (normal map) picture's alpha\n"
                                 "makes some places shinier than others; the colour picture's alpha is where sweat shines in a match."
                               : "How glossy it is (the game's g_fSpecularLev: skin is about 0.5). The colour picture's alpha makes some places\n"
                                 "shinier than others.");
    }
    // colour
    if (m.has_tint) {
      float c[3] = {e.has_tint ? e.tint[0] : m.tint[0], e.has_tint ? e.tint[1] : m.tint[1], e.has_tint ? e.tint[2] : m.tint[2]};
      ImGui::SetNextItemWidth(220 * g_scale);
      if (ImGui::ColorEdit3(simple ? "Colour" : "Colour (multiplier)", c, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR))
        e.has_tint = true, std::copy(c, c + 3, e.tint), ch = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Multiplied into its picture (white: as it is) - the game's g_f4MatDifCol.");
    }
    // normal map
    if (m.normal_slot) {
      ImGui::TextUnformatted(simple ? "Bumps:" : "Normal map:");
      ImGui::SameLine();
      ImGui::TextDisabled("%s", e.normal == 1 ? Utf8(fs::path(e.normal_file).filename().wstring()).c_str()
                                : e.normal == 2 ? "none (flat)" : m.normal_tex.empty() ? "none" : m.normal_tex.c_str());
      ImGui::SameLine();
      if (ImGui::SmallButton("Pick...")) {
        static const COMDLG_FILTERSPEC kPics[1] = {{L"Pictures", L"*.png;*.dds;*.tga;*.jpg;*.bmp"}};
        const std::wstring f = PickFile(false, L"A normal map picture (PNG, DDS ...)", kPics, 1);
        if (!f.empty()) e.normal = 1, e.normal_file = f, ch = true;
      }
      if (skin && ImGui::IsItemHovered())
        ImGui::SetTooltip("Skin: the picture's alpha is where it shines. A picture without an alpha keeps the old map's.");
      ImGui::SameLine();
      if (ImGui::SmallButton(simple ? "Smooth" : "Flat")) e.normal = 2, e.normal_file.clear(), ch = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(skin ? "No bumps: a flat normal map (keeping the old map's alpha: where it shines)." : "No bumps: a flat normal map.");
      if (e.normal) {
        ImGui::SameLine();
        if (ImGui::SmallButton("As it was")) e.normal = 0, e.normal_file.clear(), ch = true;
      }
    }
    // see-through and both sides
    bool cut = e.cutout >= 0 ? e.cutout != 0 : (m.flags & 0x100) != 0;
    if (ImGui::Checkbox(simple ? "See-through where the picture is clear" : "See-through cut-out (alpha test)", &cut)) e.cutout = cut, ch = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(skin ? "Where its picture's alpha is low it isn't drawn (lace, mesh, torn cloth) - the game's flag 0x110, as stock hair.\n"
                               "Skin part: the game's skin shader can't cut, so this also turns its sweat off (g_fSweatSPA 0): the shine then\n"
                               "follows the colour picture's alpha. Cut-off point: alpha about 0.2-0.3."
                             : "Where its picture's alpha is low it isn't drawn (lace, mesh, torn cloth) - the game's flag 0x110, as stock hair.\n"
                               "Cut-off point: alpha about 0.2-0.3.");
    ImGui::SameLine();
    bool both = e.two_sided >= 0 ? e.two_sided != 0 : (m.flags & 0x2) == 0;
    if (ImGui::Checkbox(simple ? "Both sides" : "Double-sided", &both)) e.two_sided = both, ch = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(m.skin_shine ? "Drawn from behind too (capes, open jackets, single-layer cloth); off: only its front - the game's flag 0x2.\n"
                                       "Skin parts drawn from both sides get the game's plain shader: no sweat, the shine from the colour picture's alpha."
                                     : "Drawn from behind too (capes, open jackets, single-layer cloth); off: only its front - the game's flag 0x2.");
    ImGui::SameLine();
    bool flip = e.flip == 1;
    if (ImGui::Checkbox("Flip faces", &flip)) e.flip = flip ? 1 : -1, ch = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Turns its faces the other way round (winding and normals): for a part that shows inside out in the game -\n"
                        "dark where it should be lit, its inside showing. The 3D view draws it as the game does.");
    // the flat look (no shine, a flat normal map): the closest to unlit the game's superstar shaders allow
    if (m.has_spec) {
      bool flat = e.has_spec && e.spec <= 0.0f && (e.normal == 2 || !m.normal_slot);
      if (ImGui::Checkbox("Flat look", &flat)) {
        if (flat) e.has_spec = true, e.spec = 0, e.normal = m.normal_slot ? 2 : e.normal, e.normal_file.clear();
        else e.has_spec = false, e.normal = 0;
        ch = true;
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Flat, cartoon-like shading: no shine (specular 0, even) and no bumps (a flat normal map) - the closest the\n"
                          "game's superstar shaders get to unlit: they have no self-lit setting, so the arena's lights still shade it.");
    }
    // the picture sheet
    const bool open = g_atlas_for == std::make_pair(m.model_id, m.mesh);
    if (ImGui::SmallButton(open ? "Hide the picture sheet" : (simple ? "Show where it is on the picture" : "Picture sheet (atlas)"))) g_atlas_for = open ? std::make_pair(~0u, -1) : std::make_pair(m.model_id, m.mesh);
    if (!e.Empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Undo all")) e = MatEdit(), ch = true;
    }
    if (open) {
      if (m.normal_slot) {
        ImGui::RadioButton("Colour##sheet", &reinterpret_cast<int&>(g_atlas_normal), 0);
        ImGui::SameLine();
        ImGui::RadioButton(simple ? "Bumps##sheet" : "Normal##sheet", &reinterpret_cast<int&>(g_atlas_normal), 1);
      }
      AtlasView(m.model_id, m.mesh, g_atlas_normal && m.normal_slot);
    }
    if (ch) {
      changed = true;
      if (e.Empty()) edits.erase(key);
      else edits[key] = e;
      preview[{m.model_id, m.mesh}] = e;
    }
    ImGui::PopID();
    ImGui::TreePop();
  }
  char_preview::SetMaterialPreview(preview);
  char_preview::SetHighlight(hover.first, hover.second);
  return changed;
}

}  // namespace mm
