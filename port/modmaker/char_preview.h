// The 3D character view (the Superstar page's preview and the Animations
// page): a character model pac (ch.pac) skinned and playing a motion from
// the game's YMBs banks - its idle stance by default - and, for two-person
// motions, a second character (the dummy) on the victim's track. Formats:
// docs/SUPERSTAR_MODS.md (character model), docs/MOVE_PACKS.md (banks).
#pragma once

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace char_preview {

void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx);
void Shutdown();
// The model to show and the game folder (its pac/m.pac has the idle). Loads
// in the background; the same file again does nothing.
void SetModel(const std::wstring& ch_pac, const std::wstring& game);
// The second character, for the victim's track ("" = none).
void SetDummy(const std::wstring& ch_pac);
// A motion from a YMBs bank: track 0 plays on the model, track 1 on the dummy.
// An empty bank (or ClearMotion) goes back to the idle. False (with why) when
// the motion isn't in the bank or its layout isn't understood.
bool SetMotion(std::shared_ptr<const std::vector<uint8_t>> bank, int id, int x, std::string* err = nullptr);
void ClearMotion();
// What plays: the id / x set (-1: the idle), its length in keys (30 a second).
struct Playback {
  bool playing = true;
  float speed = 1;   // 0.1 .. 2
  float key = 0;     // the current key (fraction allowed)
  int keys = 0;      // the motion's length
  int id = -1, x = 0;
  bool loop = true;
};
Playback& Play();
// The preview, w x h: left drag turns, right drag tilts, wheel zooms, middle drag pans.
void Draw(float w, float h);
void ResetCamera();
// "Loading...", what is shown ("11089 vertices, 102 bones, idle") or why not.
std::string Status();

}  // namespace char_preview
