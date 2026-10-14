// The material panel (Superstar page, Simple's Look / Attires, Attires page; the entrance gear when the 3D view
// shows it): per mesh of the model in the 3D view its shine, normal map, colour, see-through cut-out and both
// sides - the game's own per-mesh values (model_materials.h), previewed live, written into the pac at save.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <vector>

#include "app.h"
#include "char_preview.h"
#include "imgui.h"
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
}  // namespace

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
  for (const MeshMat& m : meshes) {
    if (Lower(m.model).rfind("blood", 0) == 0 || (m.model_id >= 0x7530 && m.model_id <= 0x7532)) continue;  // (blood overlays)
    const MatKey key{view_attire, gear, m.model_id, m.mesh};
    MatEdit e = edits.count(key) ? edits[key] : MatEdit();
    char label[160];
    std::snprintf(label, sizeof label, "%s, part %d%s###m%u_%d", PartName(m).c_str(), m.mesh + 1, e.Empty() ? "" : "  - changed",
                  unsigned(m.model_id), m.mesh);
    const bool node = ImGui::TreeNode(label);
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
