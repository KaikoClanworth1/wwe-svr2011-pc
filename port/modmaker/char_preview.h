// The Create a new superstar page's 3D preview: a character model pac
// (ch.pac) skinned and playing the game's own idle stance (m.pac MVMT/STAT,
// a YMBs motion bank), so a modder sees that an imported model works before
// building the mod. Formats: docs/SUPERSTAR_MODS.md (character model, idle).
#pragma once

#include <d3d11.h>

#include <string>

namespace char_preview {

void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx);
void Shutdown();
// The model to show and the game folder (its pac/m.pac has the idle). Loads
// in the background; the same file again does nothing.
void SetModel(const std::wstring& ch_pac, const std::wstring& game);
// The preview, w x h: the model playing its idle; drag it to turn it.
void Draw(float w, float h);
// "Loading...", what is shown ("11089 vertices, 102 bones, idle") or why not.
std::string Status();

}  // namespace char_preview
