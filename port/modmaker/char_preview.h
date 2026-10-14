// The 3D character view (the Superstar page's preview and the Animations
// page): a character model pac (ch.pac) skinned and playing a motion from
// the game's YMBs banks - its idle stance by default - and, for two-person
// motions, a second character (the dummy) on the victim's track. Formats:
// docs/SUPERSTAR_MODS.md (character model), docs/MOVE_PACKS.md (banks).
#pragma once
#include <array>
#include <map>
#include "model_materials.h"

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
// Entrance gear (EMD kind 4, the outfit worn for the entrance): shown instead of the attire's model when on and the
// attire has one (HasGear: the shown attire has it).
void SetGear(bool on);
bool Gear();
bool HasGear();
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
// The view as it is now, size x size RGBA: the model alone on a clear background (alpha 0), no floor - a picture.
bool Capture(int size, std::vector<uint8_t>& rgba);
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
float Height();
// The model scaled about its root's x / z instead of the origin (the Full preview: a walk far from the ring).
void SetScaleAtRoot(bool on);
float ModelHeight();
// "Loading...", what is shown ("11089 vertices, 102 bones, idle") or why not.
std::string Status();

// The Full preview (full_preview.cpp): an entrance's entrant track (body y0, with its finger tracks y2 / y3 when the
// bank has them) decoded apart from the page's motion, and the model drawn with it into another view's target
// (rtv / dsv, w x h) with that view's camera - game coordinates, row vectors, as editor.cpp; no floor or framing.
struct Clip;
std::shared_ptr<Clip> DecodeClip(const std::vector<uint8_t>& bank, int id, int x, std::string* err = nullptr);
int ClipKeys(const Clip& clip);
bool ClipHasFingers(const Clip& clip);
bool ModelReady();  // the model is loaded and on the GPU (uploads a model loaded in the background)
// Per key of the clip, on the model: the root's x / z and the lowest foot's y (game coordinates, y down).
struct TraceKey {
  float x = 0, z = 0, foot = -1e30f;
};
std::vector<TraceKey> ClipTrace(const Clip& clip);
// offset: added to the posed model (game coordinates; nullptr = none) - the Full preview's ground.
void RenderInto(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w, int h, const float view[16],
                const float proj[16], const Clip* clip, float key, const float offset[3] = nullptr);

// Props (the Full preview): an entrance's evtobj.pac SOBJ models, drawn with the entrant. AddProp parses one (any
// thread; uploaded when first drawn) -> its index, -1 on error; ClearProps frees them all.
int AddProp(const std::vector<uint8_t>& sobj, std::string* err = nullptr);
// The basic ring (off by default): the practice arena's ring kit (`arena_pac`: the game's pac\\bg\\bg29.pac,
// UTF-8) round the model, its canvas where the model stands; read the first time it's turned on.
void SetRing(bool on, const std::string& arena_pac);
// Hair and body jiggle as the game moves them (the attire's PACH children 60 / 61 / 50; hair_sim.h), on by default.
void SetPhysics(bool on);
bool Physics();
// The material panel (model_materials.h): the shown model's edits previewed live, keyed by (model child id,
// mesh index) - the model in the 3D view, its attire / gear as shown. Empty: as in the pac.
void SetMaterialPreview(const std::map<std::pair<uint32_t, int>, mm::MatEdit>& edits);
void SetHighlight(uint32_t model_id, int mesh);  // a mesh lit up in the 3D view (the panel's hovered one; -1 none)
// A shown mesh's colour (or normal) texture as an ImGui texture id and its triangles' UVs (3 per triangle).
bool MeshAtlas(uint32_t model_id, int mesh, bool normal, void** texture, std::vector<std::array<float, 2>>* uvs);
bool Ring();
// The steel steps the game puts out at the ring (pac\\bg\\bgEtc.pac STG/WPON: pack 32 the model, placed by the
// ringside set 45010 key 1000 - f32 position x3, f32 rotation x3 in degrees, s16 motion, u8 place, u8 model): the
// model (an SOBJ for AddProp) and each place, row-major rotation + translation in game space. `bg_etc` UTF-8.
struct StepPlace {
  float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, t[3] = {0, 0, 0};
};
bool RingsideSteps(const std::string& bg_etc, std::vector<uint8_t>& model, std::vector<StepPlace>& places);
void ClearProps();
// The entrant's bone as posed by the last RenderInto (game coordinates: rotation row-major, translation).
bool EntrantBone(const char* name, float r[9], float t[3]);
// Every bone of the entrant as posed by the last RenderInto (test aid: the Full preview's dump): name, world position.
struct BonePos {
  std::string name;
  float t[3];
  float r[9] = {};  // (its world rotation, row-major, unscaled)
};
std::vector<BonePos> EntrantBones();
// A prop's own bones turned by its track (bone k: Euler angles, R = Rz Ry Rx; off = its rest).
struct PropBone {
  bool on = false;
  float e[3] = {0, 0, 0};
};
using PropBones = std::vector<PropBone>;
// Draws prop `prop` with its bone 0 placed at place_r / place_t (game coordinates), then moved by offset.
void RenderProp(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w, int h, const float view[16],
                const float proj[16], int prop, const float place_r[9], const float place_t[3], const PropBones& bones,
                const float offset[3] = nullptr);

}  // namespace char_preview
