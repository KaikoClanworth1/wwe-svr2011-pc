// The Arena Editor (Mod Maker, arenas branch): the arena in 3D with its
// objects listed by zone; move / turn / scale / duplicate / hide objects,
// add objects (OBJ / FBX files, boxes), retexture, the Ring Kit, lighting
// presets and the size budget. Edits go straight into the project's Arena;
// the Ring Kit and lighting are applied when the mod is built (ApplyBuild).
#pragma once

#include <d3d11.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "svrfmt/arena.h"
#include "svrfmt/ring_kit.h"

namespace editor {

struct Lighting {
  int preset = 0;                 // index in Presets() (0 = the arena as made)
  float color[3] = {1, 1, 1};     // multiplied into every material colour
  float strength = 1.0f;
  bool crowd = true;              // the people in the seats
  bool Default() const;
};

struct Hooks {
  std::function<void(const std::string&)> log;
  std::function<void()> test_in_game;  // install the mod and start the game
  std::function<std::string(const char* title)> pick_picture;  // a picture file dialog ("" = none)
  // the prop library: arenas to take models from (name, pac path)
  std::vector<std::pair<std::string, std::string>> library;
};

void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx, const Hooks& hooks);
// The arena being edited (nullptr: none). Rebuilds the object list when it changes.
void SetArena(svrfmt::Arena* arena, const std::string& title);
// Only the models with ids lo..hi are shown and edited (a backstage area in
// bg78, which holds seven); -1, -1: all. Before SetArena.
// spot: the game's box for the area (sub_8224EF28: centre x y z, half x z);
// the test edit's box goes beside its centre, where a match camera sees it.
void SetArea(const std::vector<std::pair<int, int>>& ids, const float* spot = nullptr);
// The editor's whole UI, filling the current ImGui window region.
void Draw();
// A second, read-only scene (the Installed mods page): an arena shown without touching the one being edited.
// ViewArena takes the arena (kept alive by the caller until ClearView) and its manifest (ring / lighting keys);
// DrawView draws it in w x h with an orbit camera only.
void ViewArena(svrfmt::Arena* arena, const std::string& title, const std::string& manifest);
// An arena's ring kit as the game puts it out (its own rope heights): each part's model (index into arena.models)
// with its world matrix (row-major, row vectors, game space: y down) - for views that draw a ring without the
// editor's scene (char_preview's basic ring).
struct RingPart {
  int model = -1;
  float m[16] = {};
};
std::vector<RingPart> RingPlacements(const svrfmt::Arena& a);
void DrawView(float w, float h);
void ClearView();
// The view's camera given by the caller (the Full preview: an entrance's camera track): view and projection,
// game coordinates, row vectors (HLSL row_major + mul(v, M)) - DrawView uses it and the mouse doesn't move it,
// until ClearViewCamera. The projection's depth range should be 1 .. 5000 (the editor's).
void SetViewCamera(const float view[16], const float proj[16]);
void ClearViewCamera();
// An arena texture drawn with another picture (the Full preview: the titantron movie on the screen's tf_titan);
// nullptr puts the arena's own back. The caller keeps the view alive while it's set.
void SetTextureOverride(const std::string& name, ID3D11ShaderResourceView* srv);
// Drawn after the view's scene into the same target and depth (w x h), with the camera used: the Full preview's
// wrestler. Empty: none.
using ViewExtraFn = std::function<void(ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w, int h,
                                       const float view[16], const float proj[16])>;
void SetViewExtra(ViewExtraFn fn);
// The view's labels / mouse hint shown (the Full preview turns them off).
void SetViewHint(bool on);
bool Busy();
bool HasArena();   // an arena is set
int EditCount();   // grows with every edit (the project's dirty check)
// The size budget (measured in the background after edits): the arena's file
// and unpacked sizes against the shipped one's, and the biggest model you
// changed or added (unpacked), for the problems list. False until measured.
struct BudgetInfo {
  size_t file = 0, file_max = 0, unpacked = 0, unpacked_max = 0;
  size_t biggest = 0;          // the biggest changed / added model, unpacked
  uint32_t biggest_id = 0;
};
bool Budget(BudgetInfo& out);
void Shutdown();
// test aid: a camera preset (0 hard camera, 1 top, 2 from the stage; -1 none)
// and an object to select (by name) once an arena is set
void TestStart(int view, const std::string& select);
// test aid: moves the selection 2 m, adds a 2 m box at ringside, red top rope, warmer light
void TestEdit();
// test aid: every entrance-zone model of library arena `lib` added in its place
void TestLibrary(int lib);

svrfmt::RingSpec& Ring();
Lighting& Light();
// The Ring Kit and lighting, onto a copy of the edited arena that is about
// to be saved (then FitFile).
void ApplyBuild(svrfmt::Arena& a);
// manifest lines for the ring settings (ring.*) and the lighting (light.*)
std::string ManifestLines();
void FromManifest(const std::string& text);

}  // namespace editor
