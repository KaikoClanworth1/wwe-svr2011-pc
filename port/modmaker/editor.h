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
bool Busy();
bool HasArena();   // an arena is set
int EditCount();   // grows with every edit (the project's dirty check)
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
