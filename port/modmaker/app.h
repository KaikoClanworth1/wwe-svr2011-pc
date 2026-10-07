// SvR2011 Mod Maker: what every page shares (app.cpp owns it).
//
// The app is one ImGui window: a title bar (project: new / open / save), a
// rail of pages on the left (Make: Arena, Backstage, Superstar, Moves, Crowd
// signs, Media; Look: Game assets, Animations, Icons & renders; Help), the
// page, and a status line with the log and the Problems list folding out.
//
// A page is a namespace with Draw(), Problems(), Reset() and the project
// hooks (StateText / WriteProject / ReadProject); app.cpp lists them in
// kPages. Pages never block: anything slow goes through RunInBackground and
// Progress; anything the user could get wrong is a Problem (an error keeps
// Save and Install off; a warning is shown with what to do).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <shobjidl.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "imgui.h"
#include "svrfmt/texture.h"
#include "svrfmt/zip_write.h"

namespace mm {

namespace fs = std::filesystem;
using svrfmt::Bytes;
using svrfmt::Image;
using svrfmt::ZipEntry;

// ---------------------------------------------------------------- window, GPU

extern ID3D11Device* g_dev;
extern ID3D11DeviceContext* g_ctx;
extern HWND g_wnd;
extern std::wstring g_game;  // the installed game folder ("" = none)
extern float g_scale;        // UI scale (1 at 96 dpi with the 17 px font)

// An RGBA picture as a shader resource (nullptr for an empty picture).
ID3D11ShaderResourceView* MakeTexture(const Image& img);
inline ImTextureID Tex(ID3D11ShaderResourceView* v) { return ImTextureID(reinterpret_cast<uintptr_t>(v)); }
inline void Release(ID3D11ShaderResourceView*& v) { if (v) v->Release(), v = nullptr; }

// A picture kept with its GPU copy (the copy is made on first use, on the UI thread).
struct Picture {
  Image image;
  ID3D11ShaderResourceView* tex = nullptr;
  ~Picture() { Release(tex); }
  Picture() = default;
  Picture(const Picture& o) : image(o.image) {}
  Picture& operator=(const Picture& o) { image = o.image; Release(tex); return *this; }
  Picture(Picture&& o) noexcept : image(std::move(o.image)), tex(o.tex) { o.tex = nullptr; }
  Picture& operator=(Picture&& o) noexcept { image = std::move(o.image); Release(tex); tex = o.tex; o.tex = nullptr; return *this; }
  void Set(Image img) { image = std::move(img); Release(tex); }
  void Clear() { image = Image(); Release(tex); }
  bool Empty() const { return image.rgba.empty(); }
  ImTextureID Id() { if (!tex && !Empty()) tex = MakeTexture(image); return Tex(tex); }
  // Draws it, fitted into w x h (keeps the shape).
  void Draw(float w, float h);
};

// ---------------------------------------------------------------- strings, files

std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);
std::string PathStr(const fs::path& p);  // UTF-8
std::string Lower(std::string s);
std::string Upper(std::string s);
// A folder / file name from a title: letters and digits, '_' between words.
std::string IdFrom(const std::string& name, const char* fallback);
std::string FileName(const std::wstring& path);  // the last part, UTF-8
std::string Human(size_t bytes);                // "1.2 MB"
// "made_with=Mod Maker <version>\n": every manifest the Mod Maker writes carries it
// (the launcher flags mods without it as not made here).
std::string MadeWith();
// key=value text (manifests, projects): the value of a key ("" if absent).
std::string Value(const std::string& text, const char* key);
std::vector<std::string> Values(const std::string& text, const char* key);  // every line with that key

// Dialogs (on the UI thread).
std::wstring PickFile(bool save, const wchar_t* title, const COMDLG_FILTERSPEC* spec, UINT nspec,
                      const wchar_t* def_ext = nullptr, const wchar_t* def_name = nullptr);
std::vector<std::wstring> PickFiles(const wchar_t* title, const COMDLG_FILTERSPEC* spec, UINT nspec);
std::wstring PickFolder(const wchar_t* title);
extern const COMDLG_FILTERSPEC kPictureFilter[1], kSoundFilter[1], kBinkFilter[1], kPacFilter[1], kModFilter[1],
    kProjectFilter[1], kOpenFilter[1];
// A picture file -> Image (false logged).
bool LoadPicture(const std::wstring& file, Image& out);

// ---------------------------------------------------------------- log, status, work

void Log(const std::string& s);
void WriteLogTo(const std::wstring& file);  // the whole log into a file (test aids)
// The status line's message (also logged).
void Status(const std::string& s);
// One background job at a time; Busy() while it runs. The job may call
// Progress() to say what it is doing (shown in the status line).
void RunInBackground(std::function<void()> fn);
bool Busy();
void Progress(const std::string& what);
// Runs fn on the UI thread before the next frame (from a background job).
void OnUiThread(std::function<void()> fn);

// ---------------------------------------------------------------- problems

struct Problem {
  bool error = true;   // false: a warning (shown, does not block)
  std::string text;    // what is wrong
  std::string fix;     // what to do (optional)
};
inline Problem Error(std::string text, std::string fix = "") { return {true, std::move(text), std::move(fix)}; }
inline Problem Warning(std::string text, std::string fix = "") { return {false, std::move(text), std::move(fix)}; }
int ErrorCount(const std::vector<Problem>& p);
// A Save / Install button row: disabled (with the errors as a tooltip) while
// the page has errors or a job runs. Returns which was pressed (0 none,
// 1 save as mod, 2 install, 3 install and start the game), after confirming
// warnings once.
int ModButtons(const std::vector<Problem>& problems, bool with_test, float width = 0);
// Draws the problems inline (a page's top).
void DrawProblems(const std::vector<Problem>& problems);

// ---------------------------------------------------------------- game data

struct ArenaInfo {
  const char* banner;  // texture name in MatchHD.pac (and manifest base=)
  int number;          // pac/bg/bgNN.pac
  const char* name;
  Picture tile;        // its select banner
};
extern ArenaInfo g_arenas[20];  // the arena select screen's order
std::string ArenaPath(int tile);  // UTF-8 path of the tile's pac
int ArenaTileOf(int number);      // tile index of bgNN (-1)

struct StarInfo {
  int id = 0;
  std::string name;
};
// The playable superstars (CHAR/DAT names, SSFA renders), sorted by name.
// Loaded on first use (quick).
const std::vector<StarInfo>& Stars();
const StarInfo* StarById(int id);
// A superstar's 7 ratings (record +0..+6), 74 if unknown.
int StarRating(int id, int k);
// The base's select render (DLC_HD SSFA), decoded on first use (cached).
Picture* StarRender(int id);
// A pac's table without reading the whole file: EPAC (12-byte entries,
// 4-char names) and EPK8 (16-byte entries, 8-char names).
struct PacEntryInfo {
  std::string group, name;
  uint64_t offset = 0;
  uint32_t size = 0;
};
struct PacIndex {
  bool epk8 = false;
  std::vector<PacEntryInfo> entries;
};
bool ReadPacIndex(const fs::path& file, PacIndex& out);
// One entry's data (by group + name, or the first entry with that name when
// group is nullptr); false if absent.
bool ReadPacEntry(const fs::path& file, const char* name, Bytes& out, const char* group = nullptr);
bool ReadPacEntry(const fs::path& file, const PacEntryInfo& e, Bytes& out);

// The game's move names (misc.pac MOVS/WAZE: 160-byte records, the name at
// +0x10, the id at +0x90), loaded on first use; "" if unknown.
const std::string& MoveName(int id);
const std::map<int, std::string>& MoveNames();
bool GameRunning();
void StartGame(const std::wstring& extra_env = L"");

// ---------------------------------------------------------------- project

// What a page saves into a project (project.txt lines and files) and reads
// back. Files inside the project zip are named by the page ("star/ch.pac").
struct ProjectOut {
  std::string text;                 // key=value lines
  std::vector<ZipEntry> files;
  // slow work (an arena's compression) done in the background before the zip is written
  std::vector<std::function<void(std::vector<ZipEntry>&)>> deferred;
  void Key(const std::string& k, const std::string& v) { if (!v.empty()) text += k + "=" + v + "\n"; }
  void Key(const std::string& k, int v) { text += k + "=" + std::to_string(v) + "\n"; }
  // Copies the file into the project (false logged); returns its name there.
  std::string File(const std::string& key, const std::wstring& path, const std::string& as);
  void Png(const std::string& key, const Image& img, const std::string& as);
};
struct ProjectIn {
  std::string text;
  std::vector<ZipEntry> files;
  fs::path dir;  // the project file's folder (for relative paths)
  std::string Get(const char* key) const { return Value(text, key); }
  int GetInt(const char* key, int def) const { const auto v = Get(key); return v.empty() ? def : std::atoi(v.c_str()); }
  const ZipEntry* Find(const std::string& name) const;
  // A file of the project written next to it in the project's cache folder
  // (so pages can keep using paths); "" if absent.
  std::wstring Extract(const std::string& name) const;
  bool Png(const char* key, Image& out) const;
};

// The project's type is its page. Pages the rail shows.
enum class PageId { kArena, kEditor, kBackstage, kStar, kMoves, kSigns, kMedia, kOther, kCaw, kAssets, kAnims, kIcons, kHelp, kCount };
extern PageId g_page;
void GoTo(PageId p);
const char* PageName(PageId p);
// The mod-type pages' project hooks.
struct PageHooks {
  const char* type;  // manifest type= ("" = not a mod page)
  std::function<void()> draw;
  std::function<void(std::vector<Problem>&)> problems;
  std::function<void()> reset;
  std::function<std::string()> state;             // text that changes with every edit (dirty check)
  std::function<void(ProjectOut&)> write;
  std::function<bool(const ProjectIn&)> read;
  std::function<bool(const ProjectIn&)> read_mod;  // a .svrmod of this type back into the page
};
// Marks the project changed (pages whose state isn't all in state()).
void Touch();

// The current project: file and type page.
extern std::wstring g_project_file;  // "" = unsaved
extern PageId g_project_page;
bool ProjectDirty();
void ProjectNew(PageId type);
bool ProjectSave(bool ask_name);
bool ProjectOpen(const std::wstring& file);  // .svrproj or .svrmod
void ProjectReset();

// ---------------------------------------------------------------- settings

struct Settings {
  std::wstring game;
  std::vector<std::wstring> recent;  // projects, newest first
  int win_x = -1, win_y = -1, win_w = 0, win_h = 0;
  bool maximized = false;
  bool log_open = false;
  int font = 17;
};
extern Settings g_settings;
void SaveSettings();
void AddRecent(const std::wstring& file);

// ---------------------------------------------------------------- widgets

// A labelled file row: button opens the picker, shows the file name, 'x'
// clears. Returns true when changed.
bool FileRow(const char* label, std::wstring& file, const char* none, const COMDLG_FILTERSPEC* spec, UINT nspec,
             const char* tip = nullptr, float width = 220);
// A labelled text field (label on the left).
bool TextField(const char* label, char* buf, size_t size, const char* hint = nullptr, float width = 320);
void Hint(const char* text);  // (?) with a tooltip
void Heading(const char* title, const char* about);
void PushAccent();  // the red main-action button colours
void PopAccent();
const ImVec4 kAccent{0.78f, 0.06f, 0.18f, 1};
const ImVec4 kGood{0.12f, 0.42f, 0.25f, 1};
const ImVec4 kWarn{1, 0.75f, 0.3f, 1};
const ImVec4 kBad{1, 0.35f, 0.35f, 1};

// ---------------------------------------------------------------- pages

namespace arena_page { extern PageHooks hooks; void Tick(); void OpenEditor(int tile); void OpenModFile(const std::wstring& f);
                       void TestInGame(); void SetTestSave(const std::wstring& f, int lib); void StartNew(int tile);
                       void StartBackstage(int area);
                       // a converted arena.pac as the arena project (host tile, name), into the 3D editor
                       void SetArenaFromPac(const Bytes& pac, int host, const std::string& name);
                       // a converted bg78 as a backstage project: area, name, and its own-area gimmick pac (or "")
                       void SetBackstageFromPac(const Bytes& pac, int area, const std::string& name,
                                                const std::wstring& gimmick_pac, const std::string& gimmick); }
namespace backstage_page { extern PageHooks hooks; }
namespace star_page { extern PageHooks hooks; void TestStart(int id, const std::wstring& model, const std::string& name,
                      const std::wstring& picture, const std::wstring& save);
                      void SetModel(const std::wstring& ch_pac);  // (a converted model)
                      void TestFiles(const std::wstring& song, const std::wstring& movie, const std::wstring& voice, int call); }
namespace signs_page { extern PageHooks hooks; void TestStart(const std::vector<std::wstring>& files, const std::wstring& save);
                       Image SignPicture(const Image& src); }
namespace media_page { extern PageHooks hooks; void TestStart(int arena, const std::wstring& video,
                       const std::vector<std::wstring>& pictures, const std::wstring& save); }
namespace moves_page { extern PageHooks hooks; void TestOpen(const std::wstring& folder); void TestSave(const std::wstring& file); }
namespace caw_page { void Draw(); void TestStart(int index, int attire, const std::wstring& picture, bool save); }
namespace other_page { void Draw(); void TestConvertW13(const std::wstring& file, int host); }
namespace assets_page { void Draw(); void TestOpen(const std::string& path); }
namespace anims_page { void Draw(); void TestStart(int star_id, int bank, int motion, int dummy_id); }
namespace icons_page { void Draw(); void TestTab(int tab); }
namespace help_page { void Draw(); }

// The superstar picture -> the select renders (512 render, 256 bust, 64 icon).
void MakeRenders(const Image& src, Image& render, Image& bust, Image& icon);

// A video or picture -> a 320 x 320 Bink entrance movie (the launcher's movie
// maker), in the background; done(out or "" on failure) on the UI thread.
// fit: 0 whole picture with bars, 1 fill, 2 stretch; seconds: 0 = all.
void MakeBink(const std::wstring& video, const std::wstring& out, int fit, int seconds, std::function<void(std::wstring)> done);
// A "From a video..." button that makes the Bink into <game>\Mods\.convert and sets `movie`.
bool BinkFromVideoButton(const char* label, std::wstring& movie, const std::string& stem);

}  // namespace mm
