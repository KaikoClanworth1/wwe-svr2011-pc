// Per-mesh materials of a character model pac, as the game keeps them (docs/SVRMOD_FORMAT.md, the materials
// survey): shine g_fSpecularLev / g_iSpecularPow, the colour multiplier g_f4MatDifCol, the normal map
// (texNormal, a texture of the attire's bundle), the render flags at mesh +0x88 (0x110 see-through cut-out,
// 0x2 culled - else both sides). Read for the panel's true values; edits written into the pac at save.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "svrfmt/bytes.h"

namespace mm {

struct MatEdit {
  bool has_spec = false;
  float spec = 0.5f;
  int normal = 0;             // 0 as it is, 1 the picture below, 2 none (a flat normal)
  std::wstring normal_file;
  bool has_tint = false;
  float tint[3] = {1, 1, 1};
  int cutout = -1, two_sided = -1;  // -1 as it is
  int spec_alpha = 0;         // FBX builds only (svr11_build "specular_alpha"): 0 as is, 1 texture, 2 flat, 3 brightness
  bool Empty() const {
    return !has_spec && normal == 0 && !has_tint && cutout < 0 && two_sided < 0 && spec_alpha == 0;
  }
};

// One mesh of an attire's model as it is in the pac.
struct MeshMat {
  uint32_t model_id = 0;  // PACH child id (0 the body, 10000 the hair, 20000 lashes ...)
  int mesh = 0;           // mesh index in that model
  std::string model, shader, color_tex, normal_tex;
  float spec = 0, spec_pow = 0, tint[3] = {1, 1, 1};
  uint32_t flags = 0;
  bool has_spec = false, has_tint = false, normal_slot = false;  // (the shader has these parameters)
};

// The meshes of attire `attire` (1-4; gear: its entrance-gear model) of a character pac.
bool ReadMaterials(const svrfmt::Bytes& pac, int attire, bool gear, std::vector<MeshMat>& out, std::string* error = nullptr);

// Edits keyed by (model child id, mesh) written into attire `attire` (gear: its gear model): the parameters,
// the flags, a normal map picture added to the attire's texture bundle. Unchanged when nothing applies.
bool PatchMaterials(svrfmt::Bytes& pac, int attire, bool gear, const std::map<std::pair<uint32_t, int>, MatEdit>& edits,
                    std::string* error = nullptr);

// A project's edits: key (view attire 1-4, gear, model id, mesh) -> edit; as text (one line each) and back.
using MatKey = std::tuple<int, bool, uint32_t, int>;
std::string MaterialsText(const std::map<MatKey, MatEdit>& m);
std::map<MatKey, MatEdit> MaterialsFromText(const std::string& t);

}  // namespace mm
