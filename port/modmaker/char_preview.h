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
// The model's attire shown (1-4: its EMD entry ......<n-1>2; one it doesn't have: the first). Reloads the model.
void SetAttire(int attire);
// Test aid (Animations phase B): the game's pose dump (src/pose_dump.cpp lines) for motion id / x against this
// view's pose of the same tracks - person 0's on the model, the other on the dummy; "" until both are loaded.
std::string ComparePoseDump(const std::wstring& dump, std::shared_ptr<const std::vector<uint8_t>> bank, int id, int x);
int Attire();
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
// The blood overlays (the model's blood_* parts, as matches show them) on / off.
void SetBlood(bool on);
bool Blood();
bool HasBlood();  // the model has blood overlays that can be shown (their textures found)
void SetBloodStage(int stage);  // 0 light, 1 medium, 2 heavy (the game's blood textures by stage)
int BloodStage();
// The hit volumes (port/docs/HIT_VOLUMES.md): target spheres green, attack spheres red while a motion's
// op 87 .. 94 (strikes) or op 40 .. 41 (weapons) window has them on - drawn over the view.
void SetHits(bool on);
bool Hits();
// The model's size from its feet (a superstar mod's height=, 0.80-1.25; 1 = as
// made), shown live; the dummy keeps its own. ModelHeight: the model as posed
// now, unscaled, in game units (1 = 10 cm; 0 until shown).
void SetHeight(float scale);
float ModelHeight();
// "Loading...", what is shown ("11089 vertices, 102 bones, idle") or why not.
std::string Status();

}  // namespace char_preview
