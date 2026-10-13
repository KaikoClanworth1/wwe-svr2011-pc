// The Superstar page's project and helpers, shared with its Simple mode
// (simple_star.cpp: the guided view of the same project).
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "app.h"

namespace mm {
namespace star_page {

// Fighting styles: a new superstar's moves, entrance motions and starting
// attributes come from one (the game needs a full move-set). Each is a
// template from the roster underneath.
struct Style {
  const char* name;
  const char* about;
  int template_id;  // a playable superstar (its move-set, entrance motions)
  bool female;
};
constexpr int kStyleCount = 10;
extern const Style kStyles[kStyleCount];

struct StarProject {
  int style = 0;
  int ratings[7] = {74, 74, 74, 74, 74, 74, 74};
  bool ratings_set = false;  // (the style's, until moved)
  char name[32] = "", short_name[32] = "", author[64] = "", version[16] = "1.0";
  std::wstring model, song, movie, voice;
  std::wstring attires[4];        // [1..3]: attires 2-4 from other model pacs (their first attire)
  char attire_names[4][32] = {};  // [1..3]: their names on CHANGE ATTIRE ("" = ATTIRE N)
  int attire_entrance[4] = {-1, -1, -1, -1};  // [1..3]: attire<N>_entrance= (-1: the superstar's entrance)
  Picture picture;                // its own select picture (512): else a silhouette
  Image picture_small;            // the 256 bust made with it (or own_bust)
  bool own_bust = false;          // picture_small is the user's own bust picture
  Image picture_full;             // own full-body picture (render_full.dds -> SSFB; none: render.dds)
  bool game_look = false;         // the picture's tone moved toward the game's renders
  Image picture_src;              // the picture as picked (this session: the game look can be undone)
  int call = -1;                  // -1: "The Superstar"
  float height = 1.0f;            // height=<scale> from the feet (0.80-1.25; 1: left out)
  float theme_gain = 0.0f;        // theme_gain=<dB> (-20..+6; 0: left out)
  std::vector<Picture> signs;     // its fans' signs (up to 4; 128 x 64)
  // the SvR 2010-style extras (the game: superstar_mods.cpp)
  int pose = -1;                  // pose=<motion id>: the select / VS panel's pose (-1: the style's)
  int entrance = -1;              // entrance= number (-1: the style's); a superstar's id, or 535 (Jeff Hardy's, unused)
  char announcer[32] = "";        // announcer=<NAME>: the ring announcer's own clips of that name
  char hometown_city[32] = "";    // hometown=<CITY>[,<STATE>]: the announcer's own hometown clips
  char hometown_state[32] = "";
  std::vector<int> abilities;     // abilities=a,b,... (empty: the style's)
  bool own_abilities = false;
  std::wstring moves;             // moves=moves.txt: lines 0xOFF=<move id> (a file picked as it is)
  std::map<int, int> moveset;     // the page's picks: slot (0..223, offset slot*2) -> move id, after the file's lines
  std::wstring pack;              // a move pack folder (pack.txt + motions/) copied in as moves/
  // a model from an FBX (import): the file and the hand edits to its automatic bone mapping
  std::wstring fbx;
  std::map<std::string, std::string> fbx_map;  // source bone -> 2011 bone ("" = folds into its parent)
  std::map<std::string, std::string> fbx_on, fbx_kind, fbx_pic;  // per mesh ("node / material"): "1"/"0", kind, picture file
  // Simple mode (simple_star.cpp): the guided view of this same project, and its step
  bool simple = false;
  int simple_step = 0;
};
extern StarProject g_star;
extern int g_view_attire;  // the attire the preview shows (1-4)

// Simple mode's steps; Problems tag each problem with the step that fixes it (Problem::step).
enum SimpleStep { kStepName, kStepLook, kStepAttires, kStepMusic, kStepEntrance, kStepMoves, kStepTaunts, kStepPictures,
                  kStepExtras, kStepFinish, kStepCount };

void Problems(std::vector<Problem>& p);
void Preview();  // the 3D view, the attire buttons, the select picture
bool EntrancePicker(const char* label, int& value, const char* none, float width = 320);
void ThemeGainRow();
void SaveMod();
void Install(bool start);
void SetPicture(const Image& src);
std::wstring StyleChPac();  // the fighting style's superstar's model (pac\ch\ch<id>.pac)
void SimpleDraw();          // simple_star.cpp
void FbxRows(bool simple);  // Model from an FBX: pick, report, bone matching, meshes, Make

}  // namespace star_page
}  // namespace mm
