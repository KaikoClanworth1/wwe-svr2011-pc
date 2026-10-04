// SvR2011 Mod Maker (arenas branch): makes mods for the PC port.
// Win32 + Direct3D 11 + Dear ImGui. Opened from the launcher's Mods tab
// (--game "<game folder>"), or on its own (asks for the game folder).
//
// Arenas page: pick one of the game's 20 arenas, then
//   Export to Blender   arena.fbx + textures/*.png + arena.json (svrfmt/arena)
//   Import from Blender the edited FBX back onto that arena (svrfmt/arena_import)
//   Banner / name       the arena select banner (any picture -> 256 x 128 DXT5)
//   Save as mod         a .svrmod (zip: manifest.txt, arena.pac, banner.dds)
//   Install into game   the same folder straight into <game>/Mods/Arenas/<id>
//   Arena Editor        the arena in 3D: objects, Ring Kit, lighting (editor.cpp)
//   Open a mod          a saved .svrmod back into the editor
// Superstars page: a new playable character from one of the game's superstars
//   (its model or another ch.pac, a name, a theme song and an entrance movie),
//   saved as a .svrmod or installed into <game>/Mods/Superstars/<id>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "char_preview.h"
#include "editor.h"
#include "svrfmt/arena.h"
#include "svrfmt/arena_build.h"
#include "svrfmt/arena_import.h"
#include "svrfmt/png.h"
#include "svrfmt/ring_kit.h"
#include "svrfmt/zip_write.h"
extern "C" {
#include "../launcher/bink_decode.h"
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;
using namespace svrfmt;

namespace {

// ---------------------------------------------------------------- D3D11

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_wnd = nullptr;

void CreateTarget() {
  ID3D11Texture2D* back = nullptr;
  g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
  g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
  back->Release();
}

bool CreateDevice(HWND w) {
  DXGI_SWAP_CHAIN_DESC sd = {};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = w;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx)) &&
      FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swap, &g_dev, &got, &g_ctx)))
    return false;
  CreateTarget();
  return true;
}

ID3D11ShaderResourceView* MakeTexture(const Image& img) {
  if (img.rgba.empty()) return nullptr;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = UINT(img.w);
  td.Height = UINT(img.h);
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA sd = {img.rgba.data(), UINT(img.w * 4), 0};
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &tex))) {
    g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
  }
  return srv;
}

ImTextureID Tex(ID3D11ShaderResourceView* v) { return ImTextureID(reinterpret_cast<uintptr_t>(v)); }

// ---------------------------------------------------------------- data

struct ArenaInfo {
  const char* banner;  // texture name in MatchHD.pac
  int number;          // pac/bg/bgNN.pac
  const char* name;
  ID3D11ShaderResourceView* tex = nullptr;
};
// The arena select screen's order.
ArenaInfo g_arenas[20] = {
    {"arena_SD", 0, "SmackDown"},          {"arena_RAW", 1, "RAW"},
    {"arena_WM26", 11, "WrestleMania XXVI"}, {"arena_slam", 4, "SummerSlam"},
    {"arena_SS", 17, "Superstars"},     {"arena_HinC", 6, "Hell in a Cell"},
    {"arena_Judg", 13, "Judgment Day"},     {"arena_BASH", 15, "The Bash"},
    {"arena_NoC", 14, "Night of Champions"}, {"arena_BP", 5, "Breaking Point"},
    {"arena_RR", 9, "Royal Rumble"},        {"arena_BR", 18, "Bragging Rights"},
    {"arena_Series", 7, "Survivor Series"}, {"arena_TLC", 8, "TLC"},
    {"arena_ttt", 3, "Tribute to Troops"}, {"arena_EC", 10, "Elimination Chamber"},
    {"arena_back", 12, "Backlash"},         {"arena_EXT", 16, "Extreme Rules"},
    {"arena_ECW", 2, "ECW"},                {"arena_drui", 19, "Undertaker"},
};

std::wstring g_game;  // the installed game folder
std::wstring g_star_model;  // test aid: --star-model <ch.pac>
std::string g_star_name;    // test aid: --star-name <name>
int g_sel = 0;

// The mod being made
struct Project {
  int arena = -1;          // index in g_arenas the mod starts from
  std::unique_ptr<Arena> edited;  // after an import
  std::string fbx;
  Image banner;
  ID3D11ShaderResourceView* banner_tex = nullptr;
  char name[96] = "";
  char author[64] = "";
  char version[16] = "1.0";
  // the VS screen: replaced textures of the slot's VS theme (name -> picture)
  std::map<std::string, Image> vs;
};
Project g_proj;
int g_page = 0;  // 0 arenas, 1 editor, 2 VS screen, 3 superstars, 4 crowd signs, 5-7 media (videos, renders, audio)
// test aid (--test-edit-save <file>): once the editor has its arena, editor::TestEdit,
// name "Test Edit", save the mod there and quit
std::wstring g_test_save;
int g_test_lib = -1;  // --test-lib <tile>: its entrance models into the arena first
std::atomic<int> g_test_state{0};
// an arena loaded in the background, handed to the UI thread (the editor holds g_proj.edited)
std::mutex g_pending_mutex;
std::unique_ptr<Arena> g_pending;
std::string g_pending_fbx;
bool g_pending_to_editor = false;
bool g_pending_no_crowd = false;  // (a new empty arena)

std::mutex g_log_mutex;
std::vector<std::string> g_log;
std::atomic<bool> g_busy{false};
std::thread g_worker;

void Log(const std::string& s) {
  std::lock_guard lock(g_log_mutex);
  g_log.push_back(s);
}

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

// Backstage brawl areas: all seven are rooms of one file, bg78.pac (models
// by id range); a backstage brawl's match type decides the room (scratchpad
// re_backstage). A backstage mod is bg78 with one room rebuilt: the game
// plays it in that room's matches.
struct Area {
  const char* name;
  std::vector<std::pair<int, int>> ids;  // its models' ids (1000+: its objects: cars, crates...)
  // the game's box for the area (sub_8224EF28, logged with SVR2011_TEST_BOX_LOG):
  // centre x y z, half x z. Not where the fighters are kept (they roam
  // beyond it); the test edit's box goes beside its centre.
  float spot[5];
};
const Area kAreas[7] = {{"Parking lot", {{0, 19}, {1040, 1069}}, {125, 0, -410, 38, 50}},
                        {"GM's office", {{20, 39}, {1000, 1024}}, {-338, -8, 217, 30, 27}},
                        {"Locker room A", {{40, 59}, {1090, 1109}}, {290, -8, -190, 34, 34}},
                        {"Locker room B", {{60, 79}, {1070, 1089}}, {326, -8, 241, 34, 34}},
                        {"Large locker room", {{80, 99}, {1025, 1039}}, {460, -8, -76, 25, 25}},
                        {"Interview area", {{160, 179}, {140, 159}, {1110, 1139}}, {0, -8, 141, 30, 30}},
                        {"Catering area", {{140, 159}, {1130, 1139}}, {20, -8, -190, 45, 30}}};
int g_backstage = -1;  // the area being made (else an arena)

std::string ArenaPath(int i) {
  char b[32];
  std::snprintf(b, sizeof b, "bg%02d.pac", g_arenas[i].number);
  return Utf8((fs::path(g_game) / L"pac" / L"bg" / b).wstring());
}

bool LoadBanners() {
  Bytes d;
  Epac e;
  if (!ReadFile(Utf8((fs::path(g_game) / L"pac" / L"menu" / L"MatchHD.pac").wstring()), d) || !EpacRead(d, e))
    return false;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name != "MASI") continue;
      std::vector<PachEntry> ents;
      if (!PachRead(en.data, ents)) continue;
      for (const auto& pe : ents) {
        std::vector<BundleTexture> texs;
        if (!BundleRead(Unpack(pe.data), texs)) continue;
        for (const auto& t : texs)
          for (auto& a : g_arenas)
            if (t.name == a.banner && !a.tex) {
              Image img;
              if (DdsDecode(t.data, img)) a.tex = MakeTexture(img);
            }
      }
    }
  return true;
}

// ---------------------------------------------------------------- dialogs

std::wstring PickFile(bool save, const wchar_t* title, const COMDLG_FILTERSPEC* spec, UINT nspec,
                      const wchar_t* def_ext = nullptr, const wchar_t* def_name = nullptr) {
  IFileDialog* dlg = nullptr;
  std::wstring out;
  if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&dlg))))
    return out;
  dlg->SetTitle(title);
  if (nspec) dlg->SetFileTypes(nspec, spec);
  if (def_ext) dlg->SetDefaultExtension(def_ext);
  if (def_name) dlg->SetFileName(def_name);
  IShellItem* item = nullptr;
  PWSTR path = nullptr;
  if (SUCCEEDED(dlg->Show(g_wnd)) && SUCCEEDED(dlg->GetResult(&item)) &&
      SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
    out = path;
    CoTaskMemFree(path);
  }
  if (item) item->Release();
  dlg->Release();
  return out;
}

std::wstring PickFolder(const wchar_t* title) {
  IFileOpenDialog* dlg = nullptr;
  std::wstring out;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opt = 0;
  dlg->GetOptions(&opt);
  dlg->SetOptions(opt | FOS_PICKFOLDERS);
  dlg->SetTitle(title);
  IShellItem* item = nullptr;
  PWSTR path = nullptr;
  if (SUCCEEDED(dlg->Show(g_wnd)) && SUCCEEDED(dlg->GetResult(&item)) &&
      SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
    out = path;
    CoTaskMemFree(path);
  }
  if (item) item->Release();
  dlg->Release();
  return out;
}

// ---------------------------------------------------------------- actions

void RunInBackground(std::function<void()> fn) {
  if (g_busy) return;
  if (g_worker.joinable()) g_worker.join();
  g_busy = true;
  g_worker = std::thread([fn] {
    fn();
    g_busy = false;
  });
}

void StartProject(int i) {
  if (g_backstage >= 0) {  // (leaving a backstage area)
    g_backstage = -1;
    editor::SetArea({});
    g_proj.arena = -1;
  }
  if (g_proj.arena == i) return;
  g_proj.arena = i;
  g_proj.edited.reset();
  g_proj.fbx.clear();
  std::snprintf(g_proj.name, sizeof g_proj.name, "%s (custom)", g_arenas[i].name);
}

void ExportArena(int i) {
  const std::wstring dir = PickFolder(L"Export to Blender: choose a folder for the arena files");
  if (dir.empty()) return;
  const std::string out = Utf8((fs::path(dir) / (std::wstring(g_arenas[i].banner + 6, g_arenas[i].banner + std::strlen(g_arenas[i].banner)) + L" export")).wstring());
  const std::string pac = ArenaPath(i), title = g_arenas[i].name;
  RunInBackground([pac, out, title] {
    Log("Exporting " + title + " ...");
    Arena a;
    std::string err;
    if (!a.Load(pac, &err)) { Log("  " + err); return; }
    ExportReport rep;
    if (!svrfmt::ExportArena(a, out, title, rep)) {
      for (const auto& w : rep.warnings) Log("  " + w);
      Log("  export failed");
      return;
    }
    char b[256];
    std::snprintf(b, sizeof b, "  %d objects, %d textures -> %s", rep.models, rep.textures, out.c_str());
    Log(b);
    Log("  Open arena.fbx in Blender (File > Import > FBX). 1 Blender unit = 1 metre.");
    ShellExecuteA(nullptr, "open", out.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  });
}

void ImportArena(int i) {
  const COMDLG_FILTERSPEC spec[] = {{L"FBX (*.fbx)", L"*.fbx"}};
  const std::wstring fbx = PickFile(false, L"Import from Blender: the edited arena FBX", spec, 1);
  if (fbx.empty()) return;
  StartProject(i);
  const std::string pac = ArenaPath(i), fbx8 = Utf8(fbx);
  RunInBackground([pac, fbx8] {
    Log("Importing " + fbx8 + " ...");
    auto a = std::make_unique<Arena>();
    std::string err;
    if (!a->Load(pac, &err)) { Log("  " + err); return; }
    ImportOptions opt;
    ImportReport rep;
    const bool ok = ImportFbx(*a, fbx8, opt, rep);
    for (const auto& w : rep.warnings) Log("  " + w);
    for (const auto& e : rep.errors) Log("  error: " + e);
    if (!ok) { Log("  import failed"); return; }
    char b[256];
    std::snprintf(b, sizeof b, "  %d models changed, %d objects added, textures %d replaced %d added",
                  rep.models_changed, rep.objects_added, rep.textures_replaced, rep.textures_added);
    Log(b);
    std::string serr;
    a->Save(&serr);  // (checks it would load)
    if (!serr.empty()) { Log("  error: " + serr); return; }
    std::lock_guard lock(g_pending_mutex);
    g_pending = std::move(a);
    g_pending_fbx = fbx8;
    Log("  ready: edit it in the Arena Editor, save it as a mod, or install it into the game.");
  });
}

// The project's arena into the editor (loaded from the game folder the first time).
void OpenBackstage(int area) {
  if (g_backstage == area && g_proj.edited) {
    g_page = 1;
    return;
  }
  g_backstage = area;
  g_proj.arena = -1;
  g_proj.edited.reset();
  g_proj.fbx.clear();
  std::snprintf(g_proj.name, sizeof g_proj.name, "%s (custom)", kAreas[area].name);
  editor::SetArea(kAreas[area].ids, kAreas[area].spot);
  const std::string pac = Utf8((fs::path(g_game) / L"pac" / L"bg" / L"bg78.pac").wstring());
  RunInBackground([pac] {
    auto a = std::make_unique<Arena>();
    std::string err;
    if (!a->Load(pac, &err)) { Log("  " + err); return; }
    std::lock_guard lock(g_pending_mutex);
    g_pending = std::move(a);
    g_pending_fbx.clear();
    g_pending_to_editor = true;
  });
}

void OpenEditor(int i) {
  if (g_backstage >= 0) {  // (from a backstage area back to an arena)
    g_backstage = -1;
    editor::SetArea({});
    g_proj.arena = -1;
  }
  if (g_proj.arena == i && g_proj.edited) {
    g_page = 1;
    return;
  }
  StartProject(i);
  const std::string pac = ArenaPath(i);
  RunInBackground([pac] {
    auto a = std::make_unique<Arena>();
    std::string err;
    if (!a->Load(pac, &err)) { Log("  " + err); return; }
    std::lock_guard lock(g_pending_mutex);
    g_pending = std::move(a);
    g_pending_fbx.clear();
    g_pending_to_editor = true;
  });
}

// A new arena from nothing: the slot's file with everything but the ring,
// the floor and the ringside parts emptied. The slot (the arena it loads in
// place of) only decides the room it has and the VS screen style.
void NewArena(int slot) {
  StartProject(slot);
  std::snprintf(g_proj.name, sizeof g_proj.name, "New Arena");
  const std::string pac = ArenaPath(slot);
  RunInBackground([pac] {
    auto a = std::make_unique<Arena>();
    std::string err;
    if (!a->Load(pac, &err)) { Log("  " + err); return; }
    EmptyOptions opt;
    EmptyReport rep;
    MakeEmpty(*a, opt, rep);
    char b[200];
    std::snprintf(b, sizeof b, "New empty arena: %d objects and %d textures cleared (%.1f MB of room). Add objects, "
                  "props from other arenas (Library) and your own files.", rep.models_emptied, rep.textures_shrunk,
                  rep.bytes_freed / 1048576.0);
    Log(b);
    std::lock_guard lock(g_pending_mutex);
    g_pending = std::move(a);
    g_pending_fbx.clear();
    g_pending_to_editor = true;
    g_pending_no_crowd = true;  // (no seats left for them)
  });
}

// A saved mod back into the editor: its arena, name, banner and settings.
void OpenModFile(const std::wstring& f) {
  Bytes zip;
  std::vector<ZipEntry> files;
  if (!ReadFile(Utf8(f), zip) || !ZipRead(zip, files)) { Log("That is not a mod the Mod Maker made."); return; }
  const ZipEntry *manifest = nullptr, *pac = nullptr, *banner = nullptr;
  g_proj.vs.clear();
  for (const auto& e : files) {
    if (e.name == "manifest.txt") manifest = &e;
    if (e.name == "arena.pac") pac = &e;
    if (e.name == "banner.dds") banner = &e;
    if (e.name.rfind("vs/", 0) == 0 && e.name.size() > 7) {
      Image img;
      if (DdsDecode(e.data, img)) g_proj.vs[e.name.substr(3, e.name.size() - 7)] = img;
    }
  }
  if (!manifest || !pac) { Log("That mod has no arena in it."); return; }
  const std::string text(manifest->data.begin(), manifest->data.end());
  auto value = [&](const char* key) {
    const std::string k = std::string("\n") + key + "=";
    const std::string t = "\n" + text;
    const size_t at = t.find(k);
    if (at == std::string::npos) return std::string();
    const size_t end = t.find_first_of("\r\n", at + k.size());
    return t.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
  };
  int host = -1;
  for (int i = 0; i < 20; ++i)
    if (value("base") == g_arenas[i].banner) host = i;
  if (host < 0) { Log("The mod does not say which arena it is built on (base=)."); return; }
  auto a = std::make_unique<Arena>();
  std::string err;
  if (!a->LoadData(pac->data, &err)) { Log("  " + err); return; }
  Arena shipped;  // the budget is the host arena's shipped size
  if (shipped.Load(ArenaPath(host))) {
    a->original_file = shipped.original_file;
    a->original_unpacked = shipped.original_unpacked;
    // The Ring Kit's output (per-rope models 900-911, tinted / hidden ring
    // parts) back to the shipped ring: the kit applies again from the
    // manifest's ring.* when the mod is saved.
    auto ring_part = [](uint32_t id) { return id == 952 || (id >= 956 && id <= 967); };
    for (auto& m : a->models)
      if (ring_part(m.id))
        for (const auto& s : shipped.models)
          if (s.id == m.id) m.model = s.model, m.changed = true;
    for (auto& e : a->entries)  // the crowd (crowd=0 empties it at save)
      if (e.id == 0x4E20)
        for (const auto& se : shipped.entries)
          if (se.id == e.id) e.data = se.data;
    auto per_rope = [](uint32_t id) { return id >= 900 && id <= 911; };
    a->models.erase(std::remove_if(a->models.begin(), a->models.end(), [&](const ArenaModel& m) { return per_rope(m.id); }),
                    a->models.end());
    a->entries.erase(std::remove_if(a->entries.begin(), a->entries.end(), [&](const PachEntry& e) { return per_rope(e.id); }),
                     a->entries.end());
  }
  g_proj.arena = host;
  g_sel = host;
  std::snprintf(g_proj.name, sizeof g_proj.name, "%s", value("name").c_str());
  std::snprintf(g_proj.author, sizeof g_proj.author, "%s", value("author").c_str());
  std::snprintf(g_proj.version, sizeof g_proj.version, "%s", value("version").c_str());
  if (banner) {
    Image img;
    if (DdsDecode(banner->data, img)) {
      g_proj.banner = img;
      if (g_proj.banner_tex) g_proj.banner_tex->Release();
      g_proj.banner_tex = MakeTexture(img);
    }
  }
  editor::FromManifest(text);
  {  // the lighting is in the material colours: out again (it applies at save)
    const editor::Lighting& l = editor::Light();
    if (!l.Default()) {
      float inv[3];
      for (int k = 0; k < 3; ++k) inv[k] = l.color[k] * l.strength > 1e-4f ? 1.0f / (l.color[k] * l.strength) : 1.0f;
      for (auto& am : a->models) {
        if (am.id == 952 || (am.id >= 956 && am.id <= 967)) continue;  // (shipped ring parts, put back above)
        for (auto& s : am.model.meshes)
          for (auto& p : s.params)
            if (p.type == 0x0d && p.value.size() >= 16 && (p.name == "g_f4MatAmbCol" || p.name == "g_f4MatDifCol"))
              for (int k = 0; k < 3; ++k) PutBeF(&p.value[4 * k], BeF(&p.value[4 * k]) * inv[k]);
        am.changed = true;
      }
    }
  }
  g_proj.edited = std::move(a);
  g_proj.fbx.clear();
  editor::SetArena(g_proj.edited.get(), g_proj.name);
  g_page = 1;
  Log("Opened " + Utf8(f) + " (built on " + g_arenas[host].name + ").");
}

void OpenMod() {
  const COMDLG_FILTERSPEC spec[] = {{L"SvR2011 mod (*.svrmod)", L"*.svrmod"}};
  const std::wstring f = PickFile(false, L"Open a mod to keep working on it", spec, 1);
  if (!f.empty()) OpenModFile(f);
}

void PickBanner() {
  const COMDLG_FILTERSPEC spec[] = {{L"Pictures (*.png, *.jpg, *.tga, *.bmp)", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp"}};
  const std::wstring f = PickFile(false, L"Banner picture (shown at 256 x 128)", spec, 1);
  if (f.empty()) return;
  Image img;
  if (!LoadImageFile(Utf8(f), img)) { Log("That picture could not be read."); return; }
  g_proj.banner = Resize(img, 256, 128);
  if (g_proj.banner_tex) g_proj.banner_tex->Release();
  g_proj.banner_tex = MakeTexture(g_proj.banner);
  Log("Banner set from " + Utf8(f));
}

std::string ModId() {
  std::string id;
  for (char c : std::string(g_proj.name)) {
    if (std::isalnum(static_cast<unsigned char>(c))) id.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    else if (!id.empty() && id.back() != '_') id.push_back('_');
  }
  while (!id.empty() && id.back() == '_') id.pop_back();
  return id.empty() ? "arena" : id;
}

// A mod build: copied and set up on the UI thread (the editor keeps
// editing g_proj.edited), compressed in the background.
struct BuildJob {
  bool backstage = false;  // (a backstage area: no banner, no VS screen)
  std::vector<std::string> keep;  // textures FitFile leaves as they are
  std::shared_ptr<Arena> arena;
  std::string manifest, id;
  Image banner;
  std::map<std::string, Image> vs;
};

bool PrepareBuild(BuildJob& job) {
  if (g_backstage >= 0) {  // a backstage area: bg78 with the room rebuilt
    if (!g_proj.edited) { Log("Open the area in the Arena Editor first."); return false; }
    job.arena = std::make_shared<Arena>(*g_proj.edited);
    editor::ApplyBuild(*job.arena);
    job.id = ModId();
    job.backstage = true;
    // only the room's own textures may be made smaller to fit
    for (const auto& am : job.arena->models) {
      bool mine = am.added;
      for (const auto& [lo, hi] : kAreas[g_backstage].ids) mine |= am.id >= uint32_t(lo) && am.id <= uint32_t(hi);
      if (!mine) job.keep.insert(job.keep.end(), am.model.textures.begin(), am.model.textures.end());
    }
    job.manifest = "type=backstage\nid=" + job.id + "\nname=" + g_proj.name + "\nauthor=" + g_proj.author +
                   "\nversion=" + g_proj.version + "\narea=" + std::to_string(g_backstage) + "\n";
    return true;
  }
  if (g_proj.arena < 0) { Log("Pick an arena first (Open in Arena Editor or Import from Blender)."); return false; }
  job.arena = std::make_shared<Arena>();
  if (g_proj.edited) {
    *job.arena = *g_proj.edited;
  } else if (!job.arena->Load(ArenaPath(g_proj.arena))) {
    Log("The arena file could not be read.");
    return false;
  }
  editor::ApplyBuild(*job.arena);
  job.banner = g_proj.banner;
  job.vs = g_proj.vs;
  if (job.banner.rgba.empty()) {
    Log("No banner picture set: using a dark banner (Banner picture... sets one).");
    job.banner.w = 256;
    job.banner.h = 128;
    job.banner.rgba.assign(256 * 128 * 4, 40);
    for (size_t i = 3; i < job.banner.rgba.size(); i += 4) job.banner.rgba[i] = 255;
  }
  job.id = ModId();
  job.manifest = "type=arena\nid=" + job.id + "\nname=" + g_proj.name + "\nauthor=" + g_proj.author +
                 "\nversion=" + g_proj.version + "\nbase=" + g_arenas[g_proj.arena].banner + "\n" +
                 editor::ManifestLines();
  return true;
}

// manifest.txt, arena.pac, banner.dds (false on error, logged)
bool FinishBuild(BuildJob& job, std::vector<ZipEntry>& files) {
  const auto halved = job.arena->FitFile(job.keep);
  if (!halved.empty())
    Log("  " + std::to_string(halved.size()) + " textures halved to fit the game's room for this arena.");
  std::string err;
  Bytes pac = job.arena->Save(&err);
  if (!err.empty()) { Log("error: " + err); return false; }
  files.push_back({"manifest.txt", Bytes(job.manifest.begin(), job.manifest.end())});
  files.push_back({"arena.pac", std::move(pac)});
  if (job.backstage) return true;
  files.push_back({"banner.dds", DdsEncode(job.banner, DxtFormat::kDxt5, false)});
  for (const auto& [name, img] : job.vs)  // (same size as the original: VsPage resizes)
    files.push_back({"vs/" + name + ".dds", DdsEncode(img, DxtFormat::kDxt5, false)});
  return true;
}

void SaveMod() {
  BuildJob job;
  if (!PrepareBuild(job)) return;
  const COMDLG_FILTERSPEC spec[] = {{L"SvR2011 mod (*.svrmod)", L"*.svrmod"}};
  const std::wstring name(job.id.begin(), job.id.end());
  const std::wstring f = PickFile(true, L"Save the mod", spec, 1, L"svrmod", (name + L".svrmod").c_str());
  if (f.empty()) return;
  const std::string out = Utf8(f);
  RunInBackground([job, out]() mutable {
    Log("Building the mod ...");
    std::vector<ZipEntry> files;
    if (!FinishBuild(job, files)) return;
    if (WriteFile(out, ZipWrite(files))) Log("Saved " + out + " (add it in the launcher's Mods tab with +).");
    else Log("The mod could not be written.");
  });
}

bool InstallFiles(const std::string& id, const std::vector<ZipEntry>& files) {
  const fs::path dir = fs::path(g_game) / L"Mods" / (g_backstage >= 0 ? L"Backstage" : L"Arenas") / fs::u8path(id);
  std::error_code ec;
  fs::create_directories(dir, ec);
  for (const auto& f : files)
    if (!WriteFile(Utf8((dir / fs::u8path(f.name)).wstring()), f.data)) {
      Log("Could not write into " + Utf8(dir.wstring()) + " (is the game running?)");
      return false;
    }
  if (g_backstage >= 0)
    Log(std::string("Installed into the game: backstage brawls in the ") + kAreas[g_backstage].name + " (" +
        Utf8(dir.wstring()) + ").");
  else
    Log("Installed into the game: arena select, page 2 onwards (" + Utf8(dir.wstring()) + ").");
  return true;
}

void StartGame() {
  const std::wstring exe = (fs::path(g_game) / L"svr2011.exe").wstring();
  STARTUPINFOW si = {sizeof si};
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = L"\"" + exe + L"\"";
  if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, g_game.c_str(), &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Log("The game is starting: SELECT ARENA, then right past the last arena for page 2.");
  } else {
    Log("Could not start " + Utf8(exe) + ".");
  }
}

// Builds and installs into <game>/Mods/Arenas/<id>; then, with `start`, starts the game.
void InstallMod(bool start = false) {
  if (start && FindWindowW(nullptr, L"WWE SmackDown vs. Raw 2011")) {
    Log("The game is running: close it first (mods load when it starts).");
    return;
  }
  BuildJob job;
  if (!PrepareBuild(job)) return;
  RunInBackground([job, start]() mutable {
    Log("Building the mod ...");
    std::vector<ZipEntry> files;
    if (!FinishBuild(job, files) || !InstallFiles(job.id, files)) return;
    if (start) StartGame();
  });
}

void TestInGame() { InstallMod(true); }

// ---------------------------------------------------------------- VS screen

// The VS screen (the match screen behind the two Superstars) is a theme per
// arena: menu/MatchHD.pac group M<nn>I (nn = the arena's bg number), DXT5
// textures. A custom arena shows its slot's theme; the mod can replace any
// of its textures (vs/<name>.dds, same size), swapped in by the game while
// that arena is chosen.
struct VsTexture {
  std::string name;
  int w = 0, h = 0;
  Image original;
  ID3D11ShaderResourceView* tex = nullptr;    // the original
  ID3D11ShaderResourceView* mine = nullptr;   // the replacement (if any)
};
int g_vs_slot = -1;
std::vector<VsTexture> g_vs;

void LoadVsTheme(int slot) {
  if (slot == g_vs_slot) return;
  for (auto& t : g_vs) {
    if (t.tex) t.tex->Release();
    if (t.mine) t.mine->Release();
  }
  g_vs.clear();
  g_vs_slot = slot;
  if (slot < 0) return;
  Bytes d;
  Epac e;
  if (!ReadFile(Utf8((fs::path(g_game) / L"pac" / L"menu" / L"MatchHD.pac").wstring()), d) || !EpacRead(d, e)) return;
  char group[8];
  std::snprintf(group, sizeof group, "M%02dI", g_arenas[slot].number);
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name != group) continue;
      std::vector<PachEntry> ents;
      if (!PachRead(en.data, ents)) continue;
      for (const auto& pe : ents) {
        std::vector<BundleTexture> texs;
        if (!BundleRead(Unpack(pe.data), texs)) continue;
        for (const auto& t : texs) {
          VsTexture v;
          v.name = t.name;
          if (!DdsDecode(t.data, v.original) || v.original.w < 64 || v.original.h < 32) continue;  // (tiny: dots, glows)
          v.w = v.original.w, v.h = v.original.h;
          v.tex = MakeTexture(v.original);
          g_vs.push_back(std::move(v));
        }
      }
    }
}

void VsPage() {
  const int slot = g_proj.arena >= 0 ? g_proj.arena : g_sel;
  LoadVsTheme(slot);
  ImGui::Text("VS screen: the %s theme", g_arenas[slot].name);
  ImGui::TextWrapped("The screen behind the two Superstars before the match. Your arena uses the theme of the arena it "
                     "plays in place of; replace any of its pictures. Pictures are fitted to the same size.");
  if (g_vs.empty()) {
    ImGui::TextDisabled("No VS screen theme found for this arena.");
    return;
  }
  const float scale = ImGui::GetFontSize() / 17.0f;
  const float col = 300 * scale;
  const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (col + 10)));
  int i = 0;
  for (auto& t : g_vs) {
    if (i++ % cols) ImGui::SameLine();
    ImGui::PushID(t.name.c_str());
    ImGui::BeginGroup();
    const auto mine = g_proj.vs.find(t.name);
    if (mine != g_proj.vs.end() && !t.mine) t.mine = MakeTexture(mine->second);
    if (mine == g_proj.vs.end() && t.mine) t.mine->Release(), t.mine = nullptr;
    const float w = col, h = std::min(col * t.h / t.w, 220 * scale);
    ImGui::Image(Tex(t.mine ? t.mine : t.tex), ImVec2(h * t.w / t.h, h));
    ImGui::Text("%s  %dx%d%s", t.name.c_str(), t.w, t.h, t.mine ? "  (yours)" : "");
    if (ImGui::Button("Replace...")) {
      const COMDLG_FILTERSPEC spec[] = {{L"Pictures (*.png, *.jpg, *.tga, *.bmp)", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp"}};
      const std::wstring f = PickFile(false, L"A picture for this part of the VS screen", spec, 1);
      Image img;
      if (!f.empty() && LoadImageFile(Utf8(f), img)) {
        g_proj.vs[t.name] = Resize(img, t.w, t.h);
        if (t.mine) t.mine->Release(), t.mine = nullptr;
        Log("VS screen: " + t.name + " from " + Utf8(f));
      }
    }
    if (t.mine) {
      ImGui::SameLine();
      if (ImGui::Button("Original")) g_proj.vs.erase(t.name);
    }
    ImGui::Dummy(ImVec2(w, 4));
    ImGui::EndGroup();
    ImGui::PopID();
  }
}

// ---------------------------------------------------------------- UI

Image SignPicture(const Image& src);  // (crowd signs, below)
std::vector<std::wstring> PickFiles(const wchar_t* title, const COMDLG_FILTERSPEC* spec, UINT nspec);

// ---------------------------------------------------------------- Superstars

// A superstar mod (the game: src/superstar_mods.cpp, docs/SUPERSTAR_MODS.md):
// a new playable character under the character select's M tile, made from
// one of the game's superstars (its stats, moves, entrance motions and select
// render) with its own model pac, name, theme song and entrance movie.
// The .svrmod: manifest.txt (type=superstar, id, name, short, base, author,
// version, song=, movie=), ch.pac, theme.<ext>, movie.bik.
struct StarBase {
  int id = 0;
  std::string name;
};
std::vector<StarBase> g_star_bases;  // the playable superstars (select renders in DLC_HD.pac)
bool g_star_loaded = false;
struct StarProject {
  int style = 0;     // kStyles
  int ratings[7] = {74, 74, 74, 74, 74, 74, 74};
  bool ratings_set = false;  // (the style's, until moved)
  int base = -1;  // index in g_star_bases (old mods: the superstar it started from)
  char name[32] = "", short_name[32] = "", author[64] = "", version[16] = "1.0";
  std::wstring model, song, movie;  // files ("" model: the base's own)
  std::wstring voice;               // a recording of the name, for the ring announcer
  std::wstring attires[4];          // [1..3]: attires 2-4 from other model pacs (their first attire)
  char attire_names[4][32] = {};    // [1..3]: their names on CHANGE ATTIRE ("" = ATTIRE N)
  ID3D11ShaderResourceView* render = nullptr;
  int render_for = -1;
  // its own select picture (else the base's): 512 x 512 and the 256 x 256 bust
  Image picture, picture_small;
  ID3D11ShaderResourceView* picture_tex = nullptr;
  int call = -1;  // name call: -1 the base's, else a Created Superstar nickname
  std::vector<Image> signs;  // its fans' signs (up to 4; 128 x 64)
  std::vector<ID3D11ShaderResourceView*> sign_tex;
};
// The Created Superstar nicknames the announcer and commentary can say
// (call=<index>; the game's CAS_* list, sub_8261EAB0's table at 0x82043F98).
const char* const kNickNames[84] = {
    "Alpha",
    "Andersen",
    "Angel",
    "Angus",
    "Archangel",
    "Azrael",
    "Bison",
    "Brittany",
    "Chen",
    "Dregs",
    "Dynamite",
    "El Jefe",
    "Franco",
    "Gonzalez",
    "Griffin",
    "Grime",
    "Hero",
    "Icon",
    "Jessica",
    "Jester",
    "Justice",
    "Kowalczyk",
    "Lassiter",
    "Lee",
    "Lilith",
    "Mantis",
    "Matsuda",
    "Matsumoto",
    "Maverick",
    "Mercer",
    "Amazing",
    "Black",
    "Macho",
    "Omega",
    "Quinn",
    "Rodriguez",
    "Santos",
    "Savior",
    "Silva",
    "Skinner",
    "Sokolov",
    "Sophia",
    "Sullivan",
    "Sunshine",
    "The Bad Guy",
    "The Barbarian",
    "The Bruiser",
    "Champ",
    "The Cowboy",
    "The Disaster",
    "The Dog",
    "The Dominator",
    "The Future",
    "The Gangster",
    "The Hardcore Icon",
    "(no commentary)",
    "The King",
    "The Maniac",
    "The Masked Man",
    "The Mastodon",
    "The Mechanic",
    "The Monster",
    "The Motor",
    "The Natural",
    "The Nightmare",
    "The Ninja",
    "The Olympian",
    "The Phenom",
    "The Prince",
    "The Princess",
    "The Professor",
    "The Rocker",
    "The Samoan",
    "The Samurai",
    "The Scorpion",
    "The Show",
    "The Soldier",
    "The Superstar",
    "The Tornado",
    "Thunder",
    "Vega",
    "Williams",
    "Yosef",
    "Youngblood"
};
StarProject g_star;

// Fighting styles: a new superstar's moves, entrance motions and starting
// attributes come from one (the game needs a full move-set). Each is a
// template from the roster underneath - the user never sees it.
struct Style {
  const char* name;
  const char* about;
  int template_id;  // a playable superstar (its move-set, entrance motions)
  bool female;
};
const Style kStyles[] = {
    {"Powerhouse", "Big, slow and strong: power slams and bear hugs.", 125, false},
    {"Brawler", "Strikes, clotheslines and a power finisher.", 160, false},
    {"All-rounder", "A bit of everything: the main-event style.", 139, false},
    {"Technical", "Holds, counters and a submission finisher.", 104, false},
    {"Submission specialist", "Wears opponents down with holds.", 267, false},
    {"High flyer", "Fast, light, off the top rope.", 123, false},
    {"Showman", "Flashy moves and taunts.", 218, false},
    {"Diva: powerhouse", "A strong Diva: slams and power moves.", 224, true},
    {"Diva: high flyer", "A quick, acrobatic Diva.", 164, true},
    {"Diva: technical", "A Diva who wrestles holds and counters.", 143, true},
};
// The record's 7 attributes in its order (record +0..+6).
const char* const kAttributes[7] = {"Grapple", "Submission", "Speed", "Strikes", "Hardcore", "Charisma", "Durability"};
// test aids: --star <id> picks the base, --star-song / --star-movie <file>,
// --test-star-save <file> saves the mod there and quits
int g_star_start = 0;
std::wstring g_star_test_save;
std::wstring g_star_picture;  // --star-picture <file>

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(uint8_t(c)));
  return s;
}

// Names from chEtc.pac CHAR/DAT (260-byte records after a 4-byte header, the
// full name at +34); the playable ones are those with a render in DLC_HD.pac.
void LoadStarBases() {
  g_star_loaded = true;
  Bytes d;
  Epac e;
  std::map<int, std::string> names;
  if (ReadFile(Utf8((fs::path(g_game) / L"pac" / L"ch" / L"chEtc.pac").wstring()), d) && EpacRead(d, e))
    for (const auto& g : e.groups)
      for (const auto& en : g.entries)
        if (g.type == "CHAR" && en.name.rfind("DAT", 0) == 0) {
          std::vector<PachEntry> pe;
          if (!PachRead(en.data, pe)) continue;
          for (const auto& x : pe) {
            const Bytes r = Unpack(x.data);
            if (r.size() >= 4 + 66) names[int(x.id)] = std::string(reinterpret_cast<const char*>(&r[4 + 34]), 0,
                                                                   strnlen(reinterpret_cast<const char*>(&r[4 + 34]), 32));
          }
        }
  Bytes h;
  if (!ReadFile(Utf8((fs::path(g_game) / L"pac" / L"DLC_HD.pac").wstring()), h) || !EpacRead(h, e)) return;
  for (const auto& g : e.groups)
    if (g.type == "SSFA")
      for (const auto& en : g.entries) {
        const int id = std::atoi(en.name.c_str());
        if (id < 100 || id > 321) continue;  // (attires 1000+, DLC ids)
        g_star_bases.push_back({id, names.count(id) ? names[id] : "Superstar " + std::to_string(id)});
      }
  std::sort(g_star_bases.begin(), g_star_bases.end(),
            [](const StarBase& a, const StarBase& b) { return a.name < b.name; });
}

// The base's select render (DLC_HD.pac SSFA/<id>) as a picture.
void LoadStarRender() {
  if (g_star.render) g_star.render->Release(), g_star.render = nullptr;
  g_star.render_for = g_star.base;
  if (g_star.base < 0) return;
  Bytes h;
  Epac e;
  if (!ReadFile(Utf8((fs::path(g_game) / L"pac" / L"DLC_HD.pac").wstring()), h) || !EpacRead(h, e)) return;
  char key[8];
  std::snprintf(key, sizeof key, "%04d", g_star_bases[g_star.base].id);
  for (const auto& g : e.groups)
    if (g.type == "SSFA")
      for (const auto& en : g.entries)
        if (en.name == key) {
          Image img;
          if (DdsDecode(Unpack(en.data), img)) g_star.render = MakeTexture(img);  // (BPE-packed DDS)
        }
}

// A picture -> the select screen's renders: fitted into 512 x 512 (top
// aligned, centred), and the bust (SSFC): that at 3/4 size, its top 256 rows
// around the figure's middle (the game's own are about that).
void SetStarPicture(const Image& src) {
  Image big;
  big.w = big.h = 512;
  big.rgba.assign(size_t(512) * 512 * 4, 0);
  const float k = std::min(512.0f / src.w, 512.0f / src.h);
  const int w = std::max(1, int(src.w * k)), h = std::max(1, int(src.h * k));
  const Image fit = Resize(src, w, h);
  const int x0 = (512 - w) / 2;
  for (int y = 0; y < h; ++y)
    std::memcpy(&big.rgba[(size_t(y) * 512 + x0) * 4], &fit.rgba[size_t(y) * w * 4], size_t(w) * 4);
  const Image three = Resize(big, 384, 384);
  double sx = 0, sa = 0;  // the figure's middle (alpha-weighted)
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 384; ++x) {
      const double a = three.rgba[(size_t(y) * 384 + x) * 4 + 3];
      sx += a * x, sa += a;
    }
  const int cx = sa > 0 ? int(sx / sa) : 192;
  const int left = std::clamp(cx - 128, 0, 384 - 256);
  Image bust;
  bust.w = bust.h = 256;
  bust.rgba.resize(size_t(256) * 256 * 4);
  for (int y = 0; y < 256; ++y)
    std::memcpy(&bust.rgba[size_t(y) * 256 * 4], &three.rgba[(size_t(y) * 384 + left) * 4], 256 * 4);
  g_star.picture = big;
  g_star.picture_small = bust;
  if (g_star.picture_tex) g_star.picture_tex->Release();
  g_star.picture_tex = MakeTexture(big);
}

void PickStarPicture() {
  const COMDLG_FILTERSPEC spec[] = {{L"Pictures (*.png, *.jpg, *.tga, *.bmp)", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp"}};
  const std::wstring f = PickFile(false, L"Select screen picture (a PNG with a transparent background is best)",
                                  spec, 1);
  if (f.empty()) return;
  Image img;
  if (!LoadImageFile(Utf8(f), img)) {
    Log("That picture could not be read.");
    return;
  }
  SetStarPicture(img);
  Log("Select picture set from " + Utf8(f));
}

// A style's starting attributes: its template's record +0..+6 (chEtc.pac CHAR/DAT).
int StyleRating(int id, int k) {
  static std::map<int, std::array<int, 7>> cache;
  if (cache.empty()) {
    Bytes d;
    Epac e;
    if (ReadFile(Utf8((fs::path(g_game) / L"pac" / L"ch" / L"chEtc.pac").wstring()), d) && EpacRead(d, e))
      for (const auto& g : e.groups)
        for (const auto& en : g.entries)
          if (g.type == "CHAR" && en.name.rfind("DAT", 0) == 0) {
            std::vector<PachEntry> pe;
            if (!PachRead(en.data, pe)) continue;
            for (const auto& x : pe) {
              const Bytes r = Unpack(x.data);
              if (r.size() < 4 + 7) continue;
              std::array<int, 7> v{};
              for (int j = 0; j < 7; ++j) v[j] = r[4 + j];
              cache[int(x.id)] = v;
            }
          }
  }
  const auto it = cache.find(id);
  return it == cache.end() ? 74 : std::clamp(it->second[k], 1, 99);
}

// The right side: the superstar's picture (the 3D model preview: TODO).
// The model in 3D playing the game's idle stance (char_preview), so a
// modder sees it works; the select-screen picture below it.
void StarPreview(float scale) {
  char_preview::SetModel(g_star.model, g_game);
  ImGui::BeginGroup();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const float w = std::max(220 * scale, avail.x);
  const float h = std::clamp(avail.y - (g_star.picture_tex ? 150 : 40) * scale, 300 * scale, w * 1.5f);
  char_preview::Draw(w, h);
  if (!char_preview::Status().empty()) ImGui::TextDisabled("%s", char_preview::Status().c_str());
  if (g_star.picture_tex) {
    ImGui::Image(Tex(g_star.picture_tex), ImVec2(110 * scale, 110 * scale));
    ImGui::SameLine();
    ImGui::TextDisabled("The select screen's picture.");
  }
  ImGui::EndGroup();
}

std::string StarId() {
  std::string id;
  for (char c : std::string(g_star.name))
    id += std::isalnum(uint8_t(c)) ? char(std::tolower(uint8_t(c))) : '_';
  return id.empty() ? "superstar" : id;
}

// The mod's files. Everything is read on the UI thread (small, or one model pac).
bool BuildStar(std::string& id, std::vector<ZipEntry>& files) {
  if (!g_star.name[0]) {
    Log("Give the superstar a name.");
    return false;
  }
  if (g_star.model.empty()) {
    Log("Pick the superstar's model (a character model pac, ch.pac).");
    return false;
  }
  const Style& st = kStyles[g_star.style];
  StarBase b{st.template_id, st.name};
  if (!g_star.ratings_set) {  // (the style's own, if the page never showed them)
    for (int k = 0; k < 7; ++k) g_star.ratings[k] = StyleRating(st.template_id, k);
    g_star.ratings_set = true;
  }
  id = StarId();
  Bytes ch;
  const std::wstring model = g_star.model;
  if (!ReadFile(Utf8(model), ch) || ch.size() < 0x4000 || std::memcmp(ch.data(), "EPK8", 4)) {
    Log("The model is not a character model pac (EPK8): " + Utf8(model));
    return false;
  }
  std::string man = "type=superstar\nid=" + id + "\nname=" + g_star.name + "\nshort=" +
                    (g_star.short_name[0] ? g_star.short_name : g_star.name) + "\nbase=" + std::to_string(b.id) +
                    "\nstyle=" + st.name + "\nauthor=" + g_star.author + "\nversion=" + g_star.version + "\n";
  man += "ratings=";
  for (int k = 0; k < 7; ++k) man += std::to_string(g_star.ratings[k]) + (k < 6 ? "," : "\n");
  if (g_star.call >= 0) man += "call=" + std::to_string(g_star.call) + "\n";
  files.push_back({"ch.pac", std::move(ch)});
  if (!g_star.song.empty()) {
    Bytes s;
    const std::string ext = Utf8(fs::path(g_star.song).extension().wstring());
    if (!ReadFile(Utf8(g_star.song), s)) {
      Log("Could not read the theme song.");
      return false;
    }
    files.push_back({"theme" + ext, std::move(s)});
    man += "song=theme" + ext + "\n";
  }
  for (int a = 1; a < 4; ++a) {
    if (g_star.attires[a].empty()) continue;
    Bytes pac;
    if (!ReadFile(Utf8(g_star.attires[a]), pac) || pac.size() < 0x4000 || std::memcmp(pac.data(), "EPK8", 4)) {
      Log("Attire " + std::to_string(a + 1) + " is not a character model pac (EPK8).");
      return false;
    }
    const std::string n = "attire" + std::to_string(a + 1) + ".pac";
    files.push_back({n, std::move(pac)});
    man += "attire" + std::to_string(a + 1) + "=" + n + "\n";
    if (g_star.attire_names[a][0])
      man += "attire" + std::to_string(a + 1) + "_name=" + g_star.attire_names[a] + "\n";
  }
  if (!g_star.voice.empty()) {
    Bytes v;
    const std::string ext = Utf8(fs::path(g_star.voice).extension().wstring());
    if (!ReadFile(Utf8(g_star.voice), v)) {
      Log("Could not read the name recording.");
      return false;
    }
    files.push_back({"voice" + ext, std::move(v)});
    man += "voice=voice" + ext + "\n";
  }
  if (!g_star.movie.empty()) {
    Bytes m;
    if (!ReadFile(Utf8(g_star.movie), m) || m.size() < 4 || std::memcmp(m.data(), "BIK", 3)) {
      Log("The entrance movie must be a Bink file (.bik): the launcher's Movies tab makes them.");
      return false;
    }
    files.push_back({"movie.bik", std::move(m)});
    man += "movie=movie.bik\n";
  }
  for (size_t k = 0; k < g_star.signs.size() && k < 4; ++k)
    files.push_back({"sign" + std::to_string(k + 1) + ".dds", DdsEncode(g_star.signs[k], DxtFormat::kDxt1, true)});
  if (g_star.picture.w) {
    files.push_back({"render.dds", DdsEncode(g_star.picture, DxtFormat::kDxt5, false)});
    files.push_back({"render_small.dds", DdsEncode(g_star.picture_small, DxtFormat::kDxt5, false)});
  }
  files.insert(files.begin(), ZipEntry{"manifest.txt", Bytes(man.begin(), man.end())});
  return true;
}

void SaveStar() {
  std::string id;
  std::vector<ZipEntry> files;
  if (!BuildStar(id, files)) return;
  const COMDLG_FILTERSPEC spec[] = {{L"SvR2011 mod (*.svrmod)", L"*.svrmod"}};
  const std::wstring f = PickFile(true, L"Save the superstar mod", spec, 1, L"svrmod",
                                  (std::wstring(id.begin(), id.end()) + L".svrmod").c_str());
  if (f.empty()) return;
  if (WriteFile(Utf8(f), ZipWrite(files))) Log("Saved " + Utf8(f) + " (add it in the launcher's Mods tab with +).");
  else Log("The mod could not be written.");
}

void InstallStar(bool start) {
  if (start && FindWindowW(nullptr, L"WWE SmackDown vs. Raw 2011")) {
    Log("The game is running: close it first (mods load when it starts).");
    return;
  }
  std::string id;
  std::vector<ZipEntry> files;
  if (!BuildStar(id, files)) return;
  const fs::path dir = fs::path(g_game) / L"Mods" / L"Superstars" / fs::u8path(id);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  for (const auto& f : files)
    if (!WriteFile(Utf8((dir / fs::u8path(f.name)).wstring()), f.data)) {
      Log("Could not write into " + Utf8(dir.wstring()) + " (is the game running?)");
      return;
    }
  Log("Installed into the game: character select, M tile (" + Utf8(dir.wstring()) + ").");
  if (start) {
    const std::wstring exe = (fs::path(g_game) / L"svr2011.exe").wstring();
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\"";
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, g_game.c_str(), &si, &pi)) {
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
      Log("The game is starting: PLAY, then the M tile on the character select.");
    }
  }
}

void StarPage() {
  if (!g_star_loaded) {
    LoadStarBases();
    if (g_star_start) {  // (test aid: that superstar's model, the style with it as template if any)
      for (int i = 0; i < int(std::size(kStyles)); ++i)
        if (kStyles[i].template_id == g_star_start) g_star.style = i;
      for (int i = 0; i < int(g_star_bases.size()); ++i)
        if (g_star_bases[i].id == g_star_start)
          std::snprintf(g_star.name, sizeof g_star.name, "Test %s", g_star_bases[i].name.c_str());
      g_star.model = (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(g_star_start) + L".pac")).wstring();
    }
    if (!g_star_model.empty()) g_star.model = g_star_model;  // (after --star)
    if (!g_star_name.empty()) std::snprintf(g_star.name, sizeof g_star.name, "%s", g_star_name.c_str());
    if (!g_star_picture.empty()) {
      Image img;
      if (LoadImageFile(Utf8(g_star_picture), img)) SetStarPicture(img);
    }
    if (!g_star_test_save.empty()) {
      std::string id;
      std::vector<ZipEntry> files;
      if (BuildStar(id, files) && WriteFile(Utf8(g_star_test_save), ZipWrite(files)))
        Log("test: saved " + Utf8(g_star_test_save));
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
  }
  const float scale = ImGui::GetFontSize() / 13.0f;
  ImGui::Text("Create a new superstar");
  ImGui::TextDisabled("A new playable character under the M tile of the character select (up to 50 mods).");
  ImGui::Separator();
  ImGui::BeginGroup();
  ImGui::PushItemWidth(320 * scale);
  ImGui::InputText("Name", g_star.name, sizeof g_star.name);
  ImGui::InputText("Short name (optional)", g_star.short_name, sizeof g_star.short_name);
  if (ImGui::BeginCombo("Fighting style", kStyles[g_star.style].name)) {
    for (int i = 0; i < int(std::size(kStyles)); ++i)
      if (ImGui::Selectable(kStyles[i].name, g_star.style == i)) {
        g_star.style = i;
        g_star.ratings_set = false;
      }
    ImGui::EndCombo();
  }
  ImGui::TextDisabled("%s", kStyles[g_star.style].about);
  if (!g_star.ratings_set) {  // (the style's own attributes, from its template's record)
    if (!g_star_loaded) LoadStarBases();
    for (int k = 0; k < 7; ++k) g_star.ratings[k] = StyleRating(kStyles[g_star.style].template_id, k);
    g_star.ratings_set = true;
  }
  ImGui::Text("Attributes");
  for (int k = 0; k < 7; ++k)
    if (ImGui::SliderInt(kAttributes[k], &g_star.ratings[k], 1, 99)) g_star.ratings_set = true;
  if (ImGui::BeginCombo("Name call (optional)", g_star.call < 0 ? "The Superstar" : kNickNames[g_star.call])) {
    if (ImGui::Selectable("The Superstar##d", g_star.call < 0)) g_star.call = -1;
    for (int i = 0; i < 84; ++i)
      if (ImGui::Selectable((std::string(kNickNames[i]) + "##n" + std::to_string(i)).c_str(), g_star.call == i))
        g_star.call = i;
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("What the ring announcer and the commentators call the superstar: one of the nicknames\n"
                      "a Created Superstar can have (or record the name below).");
  ImGui::InputText("Author (optional)", g_star.author, sizeof g_star.author);
  ImGui::InputText("Version (optional)", g_star.version, sizeof g_star.version);
  ImGui::PopItemWidth();
  ImGui::Separator();
  auto file_row = [&](const char* label, std::wstring& f, const char* none, const COMDLG_FILTERSPEC* spec,
                      UINT n) {
    ImGui::PushID(label);
    if (ImGui::Button(label, ImVec2(200 * scale, 0))) {
      const std::wstring p = PickFile(false, std::wstring(label, label + std::strlen(label)).c_str(), spec, n);
      if (!p.empty()) f = p;
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(f.empty() ? none : Utf8(fs::path(f).filename().wstring()).c_str());
    if (!f.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) f.clear();
    }
    ImGui::PopID();
  };
  const COMDLG_FILTERSPEC pac[] = {{L"Character model pac (*.pac)", L"*.pac"}};
  const COMDLG_FILTERSPEC song[] = {{L"Songs", L"*.mp3;*.m4a;*.aac;*.wav;*.flac;*.wma;*.ogg"}};
  const COMDLG_FILTERSPEC bik[] = {{L"Bink movie (*.bik)", L"*.bik"}};
  file_row("Model (ch.pac)...", g_star.model, "required: the superstar's model", pac, 1);
  file_row("Theme song (optional)...", g_star.song, "the style's", song, 1);
  file_row("Entrance movie (optional)...", g_star.movie, "the style's", bik, 1);
  for (int a = 1; a < 4; ++a) {
    const std::string label = "Attire " + std::to_string(a + 1) + " (optional)...";
    file_row(label.c_str(), g_star.attires[a], "none", pac, 1);
    if (!g_star.attires[a].empty()) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(160 * scale);
      ImGui::InputTextWithHint(("##an" + std::to_string(a)).c_str(), ("ATTIRE " + std::to_string(a + 1)).c_str(),
                               g_star.attire_names[a], sizeof g_star.attire_names[a]);
    }
  }
  ImGui::TextDisabled("Attire 1 is the model above; each extra attire is another pac's first attire.");
  {  // its fans' crowd signs
    const COMDLG_FILTERSPEC pics[] = {{L"Pictures (*.png, *.jpg, *.tga, *.bmp)", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp"}};
    ImGui::BeginDisabled(g_star.signs.size() >= 4);
    if (ImGui::Button("Crowd signs (optional)...", ImVec2(200 * scale, 0)))
      for (const auto& f : PickFiles(L"Signs for the superstar's fans (up to 4)", pics, 1)) {
        Image img;
        if (g_star.signs.size() < 4 && LoadImageFile(Utf8(f), img)) {
          g_star.signs.push_back(SignPicture(img));
          g_star.sign_tex.push_back(MakeTexture(g_star.signs.back()));
        }
      }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (g_star.signs.empty()) ImGui::TextUnformatted("none");
    for (size_t k = 0; k < g_star.sign_tex.size(); ++k) {
      ImGui::SameLine();
      ImGui::Image(Tex(g_star.sign_tex[k]), ImVec2(64 * scale, 32 * scale));
    }
    if (!g_star.signs.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton("x##signs")) {
        for (auto* t : g_star.sign_tex) t->Release();
        g_star.signs.clear(), g_star.sign_tex.clear();
      }
    }
  }
  file_row("Name recording (optional)...", g_star.voice, "none (the name call above)", song, 1);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("A short recording of the name, said the ring announcer's way: it plays when he\n"
                      "announces the superstar. The commentators keep the name call above.");
  ImGui::TextDisabled("Entrance movies are .bik files: make them in the launcher's Movies tab.");
  ImGui::PushID("pic");
  if (ImGui::Button("Select picture (optional)...", ImVec2(200 * scale, 0))) PickStarPicture();
  ImGui::SameLine();
  ImGui::TextUnformatted(g_star.picture.w ? "your picture" : "a silhouette");
  if (g_star.picture.w) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
      g_star.picture = g_star.picture_small = Image();
      if (g_star.picture_tex) g_star.picture_tex->Release(), g_star.picture_tex = nullptr;
    }
  }
  ImGui::PopID();
  ImGui::Separator();
  ImGui::BeginDisabled(g_busy);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.06f, 0.18f, 1));
  if (ImGui::Button("Save as mod (.svrmod)...", ImVec2(260 * scale, 0))) SaveStar();
  ImGui::PopStyleColor();
  if (ImGui::Button("Install into game", ImVec2(260 * scale, 0))) InstallStar(false);
  if (ImGui::Button("Test in game", ImVec2(260 * scale, 0))) InstallStar(true);
  ImGui::EndDisabled();
  ImGui::EndGroup();
  ImGui::SameLine();
  StarPreview(scale);
}

// ---------------------------------------------------------------- Crowd signs

// Crowd signs (the game: src/crowd_signs.cpp): pictures the crowd holds up -
// 128 x 64 DXT1 with mipmaps, as the game's own (audience.pac AUDE/BORD).
// A sign pack (.svrmod: manifest.txt type=signs, NN_<name>.dds) installs into
// <game>/Mods/Signs/<id>; its signs join the general signs every match draws
// from. A superstar mod can have 4 signs of its own (sign1..4.dds) that its
// fans hold up.
struct Sign {
  std::string name;
  Image picture;  // 128 x 64
  ID3D11ShaderResourceView* tex = nullptr;
};
struct SignPack {
  char name[64] = "", author[64] = "", version[16] = "1.0";
  std::vector<Sign> signs;
};
SignPack g_pack;
std::vector<std::wstring> g_sign_files;  // test aids: --sign <picture>..., --test-sign-save <file>
std::wstring g_sign_test_save;

// Any picture -> a sign: fitted onto a white 128 x 64 board (no stretching).
Image SignPicture(const Image& src) {
  Image out;
  out.w = 128, out.h = 64;
  out.rgba.assign(size_t(128) * 64 * 4, 255);
  const float k = std::min(128.0f / src.w, 64.0f / src.h);
  const int w = std::max(1, int(src.w * k)), h = std::max(1, int(src.h * k));
  const Image fit = Resize(src, w, h);
  const int x0 = (128 - w) / 2, y0 = (64 - h) / 2;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint8_t* q = &fit.rgba[(size_t(y) * w + x) * 4];
      uint8_t* o = &out.rgba[(size_t(y + y0) * 128 + x + x0) * 4];
      const int a = q[3];
      for (int c = 0; c < 3; ++c) o[c] = uint8_t((q[c] * a + 255 * (255 - a)) / 255);  // (over white)
      o[3] = 255;
    }
  return out;
}

Bytes SignDds(const Image& picture) { return DdsEncode(picture, DxtFormat::kDxt1, true); }

std::vector<std::wstring> PickFiles(const wchar_t* title, const COMDLG_FILTERSPEC* spec, UINT nspec) {
  std::vector<std::wstring> out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_ALLOWMULTISELECT);
  dlg->SetTitle(title);
  dlg->SetFileTypes(nspec, spec);
  IShellItemArray* items = nullptr;
  if (SUCCEEDED(dlg->Show(g_wnd)) && SUCCEEDED(dlg->GetResults(&items))) {
    DWORD n = 0;
    items->GetCount(&n);
    for (DWORD i = 0; i < n; ++i) {
      IShellItem* item = nullptr;
      PWSTR path = nullptr;
      if (SUCCEEDED(items->GetItemAt(i, &item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        out.push_back(path);
        CoTaskMemFree(path);
      }
      if (item) item->Release();
    }
    items->Release();
  }
  dlg->Release();
  return out;
}

void AddSigns(const std::vector<std::wstring>& files) {
  for (const auto& f : files) {
    Image img;
    if (!LoadImageFile(Utf8(f), img)) {
      Log("Could not read " + Utf8(f));
      continue;
    }
    Sign sg;
    sg.name = Utf8(fs::path(f).stem().wstring());
    sg.picture = SignPicture(img);
    sg.tex = MakeTexture(sg.picture);
    g_pack.signs.push_back(std::move(sg));
  }
}

std::string PackId() {
  std::string id;
  for (char c : std::string(g_pack.name))
    id += std::isalnum(uint8_t(c)) ? char(std::tolower(uint8_t(c))) : '_';
  return id.empty() ? "signs" : id;
}

bool BuildPack(std::vector<ZipEntry>& files) {
  if (g_pack.signs.empty()) {
    Log("Add some pictures first.");
    return false;
  }
  if (!g_pack.name[0]) std::snprintf(g_pack.name, sizeof g_pack.name, "My Signs");
  const std::string man = "type=signs\nid=" + PackId() + "\nname=" + g_pack.name + "\nauthor=" + g_pack.author +
                          "\nversion=" + g_pack.version + "\n";
  files.push_back({"manifest.txt", Bytes(man.begin(), man.end())});
  for (size_t i = 0; i < g_pack.signs.size(); ++i) {
    char n[16];
    std::snprintf(n, sizeof n, "%02zu_", i + 1);
    std::string stem;
    for (char c : g_pack.signs[i].name) stem += std::isalnum(uint8_t(c)) ? c : '_';
    files.push_back({n + stem.substr(0, 40) + ".dds", SignDds(g_pack.signs[i].picture)});
  }
  return true;
}

void SavePack() {
  std::vector<ZipEntry> files;
  if (!BuildPack(files)) return;
  const COMDLG_FILTERSPEC spec[] = {{L"SvR2011 mod (*.svrmod)", L"*.svrmod"}};
  const std::string id = PackId();
  const std::wstring f =
      PickFile(true, L"Save the sign pack", spec, 1, L"svrmod", (std::wstring(id.begin(), id.end()) + L".svrmod").c_str());
  if (f.empty()) return;
  if (WriteFile(Utf8(f), ZipWrite(files))) Log("Saved " + Utf8(f) + " (add it in the launcher's Mods tab with +).");
  else Log("The sign pack could not be written.");
}

void InstallPack() {
  std::vector<ZipEntry> files;
  if (!BuildPack(files)) return;
  const fs::path dir = fs::path(g_game) / L"Mods" / L"Signs" / fs::u8path(PackId());
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  for (const auto& f : files)
    if (!WriteFile(Utf8((dir / fs::u8path(f.name)).wstring()), f.data)) {
      Log("Could not write into " + Utf8(dir.wstring()) + " (is the game running?)");
      return;
    }
  Log("Installed: the crowd holds these signs in every match from the next game start (" + Utf8(dir.wstring()) + ").");
}

void SignsPage() {
  const float scale = ImGui::GetFontSize() / 13.0f;
  if (!g_sign_files.empty()) {
    AddSigns(g_sign_files);
    g_sign_files.clear();
    if (!g_sign_test_save.empty()) {
      std::vector<ZipEntry> files;
      if (BuildPack(files) && WriteFile(Utf8(g_sign_test_save), ZipWrite(files))) Log("test: saved " + Utf8(g_sign_test_save));
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
  }
  ImGui::Text("Crowd signs");
  ImGui::TextDisabled("New signs for the crowd to hold up in every match (with the game's own). Any picture: it is "
                      "fitted onto a white 128 x 64 board.");
  ImGui::Separator();
  ImGui::PushItemWidth(320 * scale);
  ImGui::InputText("Pack name", g_pack.name, sizeof g_pack.name);
  ImGui::InputText("Author (optional)", g_pack.author, sizeof g_pack.author);
  ImGui::InputText("Version (optional)", g_pack.version, sizeof g_pack.version);
  ImGui::PopItemWidth();
  const COMDLG_FILTERSPEC spec[] = {{L"Pictures (*.png, *.jpg, *.tga, *.bmp)", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp"}};
  if (ImGui::Button("Add pictures...", ImVec2(200 * scale, 0))) AddSigns(PickFiles(L"Sign pictures", spec, 1));
  ImGui::SameLine();
  ImGui::TextDisabled("%zu sign%s", g_pack.signs.size(), g_pack.signs.size() == 1 ? "" : "s");
  ImGui::Separator();
  const float tw = 128 * 1.5f * scale, th = 64 * 1.5f * scale;
  const int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (tw + 16 * scale)));
  ImGui::BeginChild("signs", ImVec2(0, -60 * scale), false);
  for (size_t i = 0; i < g_pack.signs.size(); ++i) {
    if (i % cols) ImGui::SameLine();
    ImGui::PushID(int(i));
    ImGui::BeginGroup();
    ImGui::Image(Tex(g_pack.signs[i].tex), ImVec2(tw, th));
    if (ImGui::SmallButton("remove")) {
      if (g_pack.signs[i].tex) g_pack.signs[i].tex->Release();
      g_pack.signs.erase(g_pack.signs.begin() + long(i));
      ImGui::EndGroup();
      ImGui::PopID();
      break;
    }
    ImGui::EndGroup();
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::BeginDisabled(g_busy);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.06f, 0.18f, 1));
  if (ImGui::Button("Save as mod (.svrmod)...", ImVec2(260 * scale, 0))) SavePack();
  ImGui::PopStyleColor();
  ImGui::SameLine();
  if (ImGui::Button("Install into game", ImVec2(260 * scale, 0))) InstallPack();
  ImGui::EndDisabled();
}

// ---------------------------------------------------------------- Media pack

// A media pack (the game: src/media_mods.cpp, docs/MEDIA_MODS.md) replaces
// the game's own media: superstars' entrance videos, renders and entrance
// themes, arena screens' pictures, the menu music and any game sound. The
// three pages (Titantron videos, Menus & renders, Audio) fill one pack.
struct MediaStar {
  std::wstring video, theme;   // files
  Image render, bust, icon;    // pictures (512, 256 bust, 64 icon)
  ID3D11ShaderResourceView* tex = nullptr;
};
struct MediaArena {
  std::vector<Image> frames;  // the screens' flip-book (10 frames)
  std::vector<ID3D11ShaderResourceView*> tex;
};
struct MediaPack {
  char name[64] = "", author[64] = "", version[16] = "1.0";
  std::map<int, MediaStar> stars;    // character id -> replacements
  std::map<int, MediaArena> arenas;  // arena number -> screens
  std::wstring menu_music;
  std::vector<std::pair<std::string, std::wstring>> sounds;  // event (no "Play_") -> file
};
MediaPack g_media;
int g_media_star = -1;   // index in g_star_bases
int g_media_arena = 0;   // index in g_arenas
char g_media_event[64] = "";
int g_media_test_arena = -1;  // test aids: --media-arena <bg number> --media-video <bik> --test-media-save <file>
std::wstring g_media_test_video, g_media_test_save;
std::vector<std::wstring> g_media_test_pictures;  // --media-picture <file> (repeatable): the screens from pictures

std::string MediaId() {
  std::string id;
  for (char c : std::string(g_media.name)) id += std::isalnum(uint8_t(c)) ? char(std::tolower(uint8_t(c))) : '_';
  return id.empty() ? "media" : id;
}

void MediaPackHeader(float scale) {
  ImGui::PushItemWidth(320 * scale);
  ImGui::InputText("Pack name", g_media.name, sizeof g_media.name);
  ImGui::InputText("Author (optional)", g_media.author, sizeof g_media.author);
  ImGui::PopItemWidth();
}

bool StarPicker(float scale) {
  if (!g_star_loaded) LoadStarBases();
  ImGui::SetNextItemWidth(320 * scale);
  const char* cur = g_media_star >= 0 ? g_star_bases[g_media_star].name.c_str() : "(pick a superstar)";
  if (ImGui::BeginCombo("Superstar", cur)) {
    for (int i = 0; i < int(g_star_bases.size()); ++i) {
      const bool has = g_media.stars.count(g_star_bases[i].id) > 0;
      if (ImGui::Selectable((g_star_bases[i].name + (has ? "  *" : "") + "##m" + std::to_string(i)).c_str(),
                            g_media_star == i))
        g_media_star = i;
    }
    ImGui::EndCombo();
  }
  return g_media_star >= 0;
}

// The arena's screen flip-book: its textures named <prefix>_animNN (00-09).
std::vector<std::string> ScreenFrames(Arena& a) {
  std::vector<std::string> names;
  for (const auto& b : a.bundles)
    for (const auto& t : b.textures) {
      const size_t at = t.name.rfind("_anim");
      if (at != std::string::npos && t.name.size() == at + 7 && std::isdigit(uint8_t(t.name[at + 5])) &&
          std::isdigit(uint8_t(t.name[at + 6])))
        names.push_back(t.name);
    }
  std::sort(names.begin(), names.end());
  return names;
}

void SetArenaFrames(int arena, std::vector<Image> frames) {
  MediaArena& m = g_media.arenas[arena];
  for (auto* t : m.tex) t->Release();
  m.tex.clear();
  m.frames = std::move(frames);
  for (const auto& f : m.frames) m.tex.push_back(MakeTexture(f));
}

// A .bik -> 10 frames, evenly spread.
std::vector<Image> FramesFromVideo(const std::wstring& file) {
  std::vector<Image> out;
  wchar_t err[256] = L"";
  BinkReader* r = bink_open(file.c_str(), err, 256);
  if (!r) {
    Log("Not a Bink video (.bik): " + Utf8(file) + " - the launcher's Movies tab makes them.");
    return out;
  }
  const int w = bink_width(r), h = bink_height(r), n = std::max(1, bink_frames(r));
  std::vector<uint8_t> bgra(size_t(w) * h * 4);
  int pos = 0;
  for (int k = 0; k < 10; ++k) {
    const int want = k * n / 10;
    while (pos <= want) bink_next(r), ++pos;
    bink_bgra(r, bgra.data());
    Image img;
    img.w = w, img.h = h;
    img.rgba.resize(bgra.size());
    for (size_t i = 0; i < bgra.size(); i += 4)
      img.rgba[i] = bgra[i + 2], img.rgba[i + 1] = bgra[i + 1], img.rgba[i + 2] = bgra[i], img.rgba[i + 3] = 255;
    out.push_back(std::move(img));
  }
  bink_close(r);
  return out;
}

// The pack's files.
bool BuildMedia(std::vector<ZipEntry>& files) {
  if (!g_media.name[0]) std::snprintf(g_media.name, sizeof g_media.name, "My Media");
  std::string man = "type=media\nid=" + MediaId() + "\nname=" + g_media.name + "\nauthor=" + g_media.author +
                    "\nversion=" + g_media.version + "\n";
  auto add_file = [&](const std::string& key, const std::wstring& src, const std::string& stem) -> bool {
    Bytes d;
    if (!ReadFile(Utf8(src), d)) {
      Log("Could not read " + Utf8(src));
      return false;
    }
    const std::string n = stem + Utf8(fs::path(src).extension().wstring());
    files.push_back({n, std::move(d)});
    man += key + "=" + n + "\n";
    return true;
  };
  for (const auto& [id, s] : g_media.stars) {
    const std::string i = std::to_string(id);
    if (!s.video.empty() && !add_file("video." + i, s.video, "video_" + i)) return false;
    if (!s.theme.empty() && !add_file("theme." + i, s.theme, "theme_" + i)) return false;
    if (s.render.w) {
      files.push_back({"render_" + i + ".dds", DdsEncode(s.render, DxtFormat::kDxt5, false)});
      files.push_back({"bust_" + i + ".dds", DdsEncode(s.bust, DxtFormat::kDxt5, false)});
      files.push_back({"icon_" + i + ".dds", DdsEncode(s.icon, DxtFormat::kDxt5, false)});
      man += "render." + i + "=render_" + i + ".dds\nbust." + i + "=bust_" + i + ".dds\nicon." + i + "=icon_" + i +
             ".dds\n";
    }
  }
  for (const auto& [num, ma] : g_media.arenas) {
    if (ma.frames.empty()) continue;
    char bg[16];
    std::snprintf(bg, sizeof bg, "bg%02d.pac", num);
    Arena a;
    std::string err;
    if (!a.Load(Utf8((fs::path(g_game) / L"pac" / L"bg" / fs::u8path(bg)).wstring()), &err)) {
      Log(std::string(bg) + ": " + err);
      return false;
    }
    const auto names = ScreenFrames(a);
    if (names.empty()) {
      Log(std::string(bg) + " has no screen flip-book (<name>_anim00..09) to replace.");
      continue;
    }
    std::vector<std::string> keep;
    for (size_t k = 0; k < names.size(); ++k) {
      BundleTexture* t = a.FindTexture(names[k]);
      DdsInfo info;
      if (!t || !DdsInfoOf(t->data, info)) continue;
      const Image& src = ma.frames[k * ma.frames.size() / names.size()];
      const DxtFormat f = info.format == DxtFormat::kArgb ? DxtFormat::kDxt5 : info.format;
      t->data = DdsEncode(Resize(src, info.w, info.h), f, info.mips > 1);
      keep.push_back(names[k]);
      for (auto& b : a.bundles)
        for (auto& bt : b.textures)
          if (&bt == t) b.changed = true;
    }
    a.FitFile(keep);
    const Bytes pac = a.Save(&err);
    if (!err.empty()) {
      Log(std::string(bg) + ": " + err);
      return false;
    }
    files.push_back({bg, pac});
    man += "arena." + std::to_string(num) + "=" + bg + "\n";
  }
  if (!g_media.menu_music.empty() && !add_file("menu_music", g_media.menu_music, "menu_music")) return false;
  for (size_t k = 0; k < g_media.sounds.size(); ++k)
    if (!add_file("sound." + g_media.sounds[k].first, g_media.sounds[k].second, "sound_" + std::to_string(k)))
      return false;
  files.insert(files.begin(), ZipEntry{"manifest.txt", Bytes(man.begin(), man.end())});
  return files.size() > 1;
}

void SaveMedia() {
  std::vector<ZipEntry> files;
  if (!BuildMedia(files)) {
    Log("Nothing in the pack yet.");
    return;
  }
  const COMDLG_FILTERSPEC spec[] = {{L"SvR2011 mod (*.svrmod)", L"*.svrmod"}};
  const std::string id = MediaId();
  const std::wstring f =
      PickFile(true, L"Save the media pack", spec, 1, L"svrmod", (std::wstring(id.begin(), id.end()) + L".svrmod").c_str());
  if (f.empty()) return;
  if (WriteFile(Utf8(f), ZipWrite(files))) Log("Saved " + Utf8(f) + " (add it in the launcher's Mods tab with +).");
  else Log("The media pack could not be written.");
}

void InstallMedia() {
  std::vector<ZipEntry> files;
  if (!BuildMedia(files)) {
    Log("Nothing in the pack yet.");
    return;
  }
  const fs::path dir = fs::path(g_game) / L"Mods" / L"Media" / fs::u8path(MediaId());
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  for (const auto& f : files)
    if (!WriteFile(Utf8((dir / fs::u8path(f.name)).wstring()), f.data)) {
      Log("Could not write into " + Utf8(dir.wstring()) + " (is the game running?)");
      return;
    }
  Log("Installed: it takes effect the next time the game starts (" + Utf8(dir.wstring()) + ").");
}

void MediaButtons(float scale) {
  ImGui::Separator();
  ImGui::TextDisabled("One pack holds everything on the Titantron videos, Menus & renders and Audio pages.\n"
                      "Every item is optional: fill in only what you want to replace.");
  ImGui::BeginDisabled(g_busy);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.06f, 0.18f, 1));
  if (ImGui::Button("Save as mod (.svrmod)...", ImVec2(260 * scale, 0))) SaveMedia();
  ImGui::PopStyleColor();
  ImGui::SameLine();
  if (ImGui::Button("Install into game", ImVec2(260 * scale, 0))) InstallMedia();
  ImGui::EndDisabled();
}

void FileField(const char* label, std::wstring& f, const COMDLG_FILTERSPEC* spec, float scale) {
  ImGui::PushID(label);
  if (ImGui::Button(label, ImVec2(220 * scale, 0))) {
    const std::wstring p = PickFile(false, std::wstring(label, label + std::strlen(label)).c_str(), spec, 1);
    if (!p.empty()) f = p;
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(f.empty() ? "the game's" : Utf8(fs::path(f).filename().wstring()).c_str());
  if (!f.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) f.clear();
  }
  ImGui::PopID();
}

void VideosPage() {
  const float scale = ImGui::GetFontSize() / 13.0f;
  if (g_media_test_arena >= 0 && (!g_media_test_video.empty() || !g_media_test_pictures.empty())) {
    std::vector<Image> frames;
    for (const auto& f : g_media_test_pictures) {
      Image img;
      if (LoadImageFile(Utf8(f), img)) frames.push_back(std::move(img));
    }
    SetArenaFrames(g_media_test_arena, frames.empty() ? FramesFromVideo(g_media_test_video) : std::move(frames));
    for (int i = 0; i < 20; ++i)
      if (g_arenas[i].number == g_media_test_arena) g_media_arena = i;
    g_media_test_arena = -1;
    if (!g_media_test_save.empty()) {
      std::vector<ZipEntry> files;
      if (BuildMedia(files) && WriteFile(Utf8(g_media_test_save), ZipWrite(files))) Log("test: saved " + Utf8(g_media_test_save));
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
  }
  ImGui::Text("Titantron videos");
  ImGui::TextDisabled("Superstars' entrance videos (320 x 320 Bink, the launcher's Movies tab makes them) and the "
                      "arena screens' pictures between entrances.");
  ImGui::Separator();
  MediaPackHeader(scale);
  ImGui::Separator();
  const COMDLG_FILTERSPEC bik[] = {{L"Bink movie (*.bik)", L"*.bik"}};
  if (StarPicker(scale)) {
    MediaStar& s = g_media.stars[g_star_bases[g_media_star].id];
    FileField("Entrance video...", s.video, bik, scale);
  }
  ImGui::Separator();
  ImGui::Text("Arena screens");
  ImGui::SetNextItemWidth(320 * scale);
  if (ImGui::BeginCombo("Arena", g_arenas[g_media_arena].name)) {
    for (int i = 0; i < 20; ++i)
      if (ImGui::Selectable(g_arenas[i].name, g_media_arena == i)) g_media_arena = i;
    ImGui::EndCombo();
  }
  const int num = g_arenas[g_media_arena].number;
  const COMDLG_FILTERSPEC pics[] = {{L"Pictures (*.png, *.jpg, *.tga, *.bmp)", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp"}};
  if (ImGui::Button("From a video (.bik)...", ImVec2(220 * scale, 0))) {
    const std::wstring f = PickFile(false, L"Screen video", bik, 1);
    if (!f.empty()) {
      auto frames = FramesFromVideo(f);
      if (!frames.empty()) SetArenaFrames(num, std::move(frames));
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("From pictures...", ImVec2(220 * scale, 0))) {
    std::vector<Image> frames;
    for (const auto& f : PickFiles(L"Screen pictures (played in turn)", pics, 1)) {
      Image img;
      if (LoadImageFile(Utf8(f), img)) frames.push_back(std::move(img));
    }
    if (!frames.empty()) SetArenaFrames(num, std::move(frames));
  }
  if (auto it = g_media.arenas.find(num); it != g_media.arenas.end() && !it->second.frames.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("x##arena")) SetArenaFrames(num, {});
    for (size_t k = 0; k < it->second.tex.size(); ++k) {
      if (k) ImGui::SameLine();
      ImGui::Image(Tex(it->second.tex[k]), ImVec2(96 * scale, 48 * scale));
    }
  } else {
    ImGui::TextDisabled("the arena's own screens");
  }
  MediaButtons(scale);
}

void RendersPage() {
  const float scale = ImGui::GetFontSize() / 13.0f;
  ImGui::Text("Menus & renders");
  ImGui::TextDisabled("A superstar's pictures in the menus: the render, the bust and the face icon (from one "
                      "picture; a PNG with a transparent background is best).");
  ImGui::Separator();
  MediaPackHeader(scale);
  ImGui::Separator();
  if (StarPicker(scale)) {
    MediaStar& s = g_media.stars[g_star_bases[g_media_star].id];
    const COMDLG_FILTERSPEC pics[] = {{L"Pictures (*.png, *.jpg, *.tga, *.bmp)", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp"}};
    if (ImGui::Button("Picture...", ImVec2(220 * scale, 0))) {
      const std::wstring f = PickFile(false, L"Superstar picture", pics, 1);
      Image img;
      if (!f.empty() && LoadImageFile(Utf8(f), img)) {
        SetStarPicture(img);  // (the 512 render and 256 bust, as for a superstar mod)
        s.render = g_star.picture, s.bust = g_star.picture_small;
        g_star.picture = g_star.picture_small = Image();
        if (g_star.picture_tex) g_star.picture_tex->Release(), g_star.picture_tex = nullptr;
        // the face icon: the bust's top middle, 64 x 64
        Image head;
        head.w = head.h = 128;
        head.rgba.resize(size_t(128) * 128 * 4);
        for (int y = 0; y < 128; ++y)
          std::memcpy(&head.rgba[size_t(y) * 128 * 4], &s.bust.rgba[(size_t(y) * 256 + 64) * 4], 128 * 4);
        s.icon = Resize(head, 64, 64);
        if (s.tex) s.tex->Release();
        s.tex = MakeTexture(s.render);
      }
    }
    if (s.render.w) {
      ImGui::SameLine();
      if (ImGui::SmallButton("x")) {
        s.render = s.bust = s.icon = Image();
        if (s.tex) s.tex->Release(), s.tex = nullptr;
      }
      ImGui::Image(Tex(s.tex), ImVec2(256 * scale, 256 * scale));
    } else {
      ImGui::SameLine();
      ImGui::TextDisabled("the game's");
    }
  }
  MediaButtons(scale);
}

void AudioPage() {
  const float scale = ImGui::GetFontSize() / 13.0f;
  ImGui::Text("Audio");
  ImGui::TextDisabled("Entrance themes, the menu music and any game sound (crowd chants, hits ...) replaced by your "
                      "own files (.mp3 .m4a .wav .flac .wma .ogg).");
  ImGui::Separator();
  MediaPackHeader(scale);
  ImGui::Separator();
  const COMDLG_FILTERSPEC song[] = {{L"Sounds", L"*.mp3;*.m4a;*.aac;*.wav;*.flac;*.wma;*.ogg"}};
  if (StarPicker(scale)) {
    MediaStar& s = g_media.stars[g_star_bases[g_media_star].id];
    FileField("Entrance theme...", s.theme, song, scale);
  }
  ImGui::Separator();
  FileField("Menu music...", g_media.menu_music, song, scale);
  ImGui::Separator();
  ImGui::Text("Game sounds");
  ImGui::TextDisabled("By the game's event name without \"Play_\" (e.g. SVR10_Chant_Sena_001 for the Cena chant, "
                      "elbow_mid_0231_0_0).");
  ImGui::SetNextItemWidth(320 * scale);
  ImGui::InputTextWithHint("##event", "event name", g_media_event, sizeof g_media_event);
  ImGui::SameLine();
  if (ImGui::Button("Sound file...", ImVec2(160 * scale, 0)) && g_media_event[0]) {
    const std::wstring f = PickFile(false, L"Sound file", song, 1);
    std::string ev = g_media_event;
    if (ev.rfind("Play_", 0) == 0) ev = ev.substr(5);
    if (!f.empty()) g_media.sounds.push_back({ev, f}), g_media_event[0] = 0;
  }
  for (size_t k = 0; k < g_media.sounds.size(); ++k) {
    ImGui::PushID(int(k));
    ImGui::Text("%s  ->  %s", g_media.sounds[k].first.c_str(),
                Utf8(fs::path(g_media.sounds[k].second).filename().wstring()).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) {
      g_media.sounds.erase(g_media.sounds.begin() + long(k));
      ImGui::PopID();
      break;
    }
    ImGui::PopID();
  }
  MediaButtons(scale);
}

void Style() {
  ImGuiStyle& s = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  s.WindowRounding = 0;
  s.FrameRounding = 4;
  s.FramePadding = ImVec2(10, 6);
  s.ItemSpacing = ImVec2(10, 8);
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.086f, 0.090f, 0.106f, 1);
  c[ImGuiCol_ChildBg] = ImVec4(0.110f, 0.114f, 0.133f, 1);
  c[ImGuiCol_Button] = ImVec4(0.165f, 0.176f, 0.208f, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.55f, 0.07f, 0.13f, 1);
  c[ImGuiCol_ButtonActive] = ImVec4(0.78f, 0.06f, 0.18f, 1);
  c[ImGuiCol_Header] = ImVec4(0.78f, 0.06f, 0.18f, 0.6f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.78f, 0.06f, 0.18f, 0.8f);
  c[ImGuiCol_FrameBg] = ImVec4(0.165f, 0.176f, 0.208f, 1);
}

void Draw() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::Begin("Mod Maker", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus);
  const float scale = ImGui::GetFontSize() / 13.0f;
  const float rail = (g_page == 1 ? 110 : 170) * scale, side = 300 * scale, logh = (g_page == 1 ? 70 : 100) * scale;
  // rail
  // an arena loaded in the background: into the project (and the editor)
  {
    std::lock_guard lock(g_pending_mutex);
    if (g_pending) {
      g_proj.edited = std::move(g_pending);
      g_proj.fbx = g_pending_fbx;
      editor::SetArena(g_proj.edited.get(), g_proj.name);
      if (g_pending_to_editor) g_page = 1;
      g_pending_to_editor = false;
      if (g_pending_no_crowd) editor::Light().crowd = false;
      g_pending_no_crowd = false;
      if (!g_test_save.empty() && g_test_state == 0) {
        g_test_state = 1;
        if (g_test_lib >= 0) editor::TestLibrary(g_test_lib);
        editor::TestEdit();
        // and the VS screen: the theme's biggest picture as a red / yellow checker
        if (g_backstage < 0) LoadVsTheme(g_proj.arena);
        const VsTexture* big = nullptr;
        for (const auto& t : g_vs)
          if (!big || t.w * t.h > big->w * big->h) big = &t;
        if (big) {
          Image img;
          img.w = big->w, img.h = big->h;
          img.rgba.resize(size_t(img.w) * img.h * 4);
          for (int y = 0; y < img.h; ++y)
            for (int x = 0; x < img.w; ++x) {
              uint8_t* q = &img.rgba[(size_t(y) * img.w + x) * 4];
              const bool on = ((x / 64) + (y / 64)) & 1;
              q[0] = on ? 230 : 250, q[1] = on ? 20 : 210, q[2] = on ? 30 : 0, q[3] = 255;
            }
          g_proj.vs[big->name] = img;
          Log("test: VS screen " + big->name + " replaced");
        }
        std::snprintf(g_proj.name, sizeof g_proj.name, "Test Edit");
        BuildJob job;
        if (PrepareBuild(job)) {
          const std::string out = Utf8(g_test_save);
          RunInBackground([job, out]() mutable {
            std::vector<ZipEntry> files;
            if (FinishBuild(job, files) && WriteFile(out, ZipWrite(files))) Log("test: saved " + out);
            else Log("test: build failed");
            g_test_state = 2;
          });
        }
      }
    }
  }
  if (g_test_state == 2 && !g_busy) {
    if (FILE* f = _wfopen((g_test_save + L".log").c_str(), L"wb")) {  // (the log, for the test scripts)
      std::lock_guard lock(g_log_mutex);
      for (const auto& line : g_log) std::fprintf(f, "%s\n", line.c_str());
      std::fclose(f);
    }
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    g_test_state = 3;
  }
  ImGui::BeginChild("rail", ImVec2(rail, -logh), true);
  for (int p = 0; p < 8; ++p) {
    const char* names[] = {"Arenas",      "Arena Editor",     "VS screen", "Superstars",
                           "Crowd signs", "Titantron videos", "Menus & renders", "Audio"};
    if (g_page == p) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.06f, 0.18f, 1));
    if (ImGui::Button(names[p], ImVec2(-1, 0))) {
      if (p == 1 && !g_proj.edited) OpenEditor(g_sel);
      else g_page = p;
    }
    if (g_page == p) ImGui::PopStyleColor();
  }
  ImGui::Separator();

  ImGui::EndChild();
  ImGui::SameLine();
  if (g_page == 1) {
    ImGui::BeginChild("editor", ImVec2(0, -logh), false);
    editor::Draw();
    ImGui::EndChild();
  } else if (g_page == 2) {
    ImGui::BeginChild("vs", ImVec2(0, -logh), true);
    VsPage();
    ImGui::EndChild();
  } else if (g_page == 3) {
    ImGui::BeginChild("stars", ImVec2(0, -logh), true);
    StarPage();
    ImGui::EndChild();
  } else if (g_page == 4) {
    ImGui::BeginChild("signs", ImVec2(0, -logh), true);
    SignsPage();
    ImGui::EndChild();
  } else if (g_page >= 5) {
    ImGui::BeginChild("media", ImVec2(0, -logh), true);
    if (g_page == 5) VideosPage();
    else if (g_page == 6) RendersPage();
    else AudioPage();
    ImGui::EndChild();
  } else {
  // grid
  ImGui::BeginChild("grid", ImVec2(-side, -logh), true);
  ImGui::Text("Pick an arena to start from");
  ImGui::TextDisabled("Read from the game folder, never changed. A mod adds a new arena on the arena select pages.");
  const float avail = ImGui::GetContentRegionAvail().x;
  const int cols = 5;
  const ImGuiStyle& st = ImGui::GetStyle();
  const float tw = (avail - st.ItemSpacing.x * (cols - 1)) / cols - 1;  // a tile, frame included
  const float iw = tw - st.FramePadding.x * 2, ih = iw * 0.5f;          // its picture (2:1)
  for (int i = 0; i < 20; ++i) {
    if (i % cols) ImGui::SameLine();
    ImGui::PushID(i);
    const bool sel = g_sel == i;
    if (sel) ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.9f, 0.1f, 0.2f, 1));
    ImGui::BeginGroup();
    if (g_arenas[i].tex) {
      if (ImGui::ImageButton("t", Tex(g_arenas[i].tex), ImVec2(iw, ih))) g_sel = i;
    } else if (ImGui::Button(g_arenas[i].name, ImVec2(tw, ih + st.FramePadding.y * 2))) {
      g_sel = i;
    }
    {  // the name, clipped to the tile
      const ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::PushClipRect(p, ImVec2(p.x + tw, p.y + ImGui::GetTextLineHeight()), true);
      ImGui::TextUnformatted(g_arenas[i].name);
      ImGui::PopClipRect();
    }
    ImGui::EndGroup();
    if (sel) {
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddRect(a, b, IM_COL32(230, 25, 50, 255), 4, 0, 3 * scale);
      ImGui::PopStyleColor();
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::SameLine();
  // selected arena
  ImGui::BeginChild("side", ImVec2(0, -logh), true);
  const ArenaInfo& a = g_arenas[g_sel];
  ImGui::Text("%s", a.name);
  ImGui::TextDisabled("pac/bg/bg%02d.pac", a.number);
  const float bw = ImGui::GetContentRegionAvail().x;
  if (a.tex) ImGui::Image(Tex(a.tex), ImVec2(bw * 0.6f, bw * 0.3f));
  ImGui::Separator();
  ImGui::BeginDisabled(g_busy);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.06f, 0.18f, 1));
  if (ImGui::Button("Open in Arena Editor", ImVec2(-1, 0))) OpenEditor(g_sel);
  ImGui::PopStyleColor();
  if (ImGui::Button("Export to Blender...", ImVec2(-1, 0))) ExportArena(g_sel);
  if (ImGui::Button("Import from Blender...", ImVec2(-1, 0))) ImportArena(g_sel);
  if (ImGui::Button("Open a mod (.svrmod)...", ImVec2(-1, 0))) OpenMod();
  if (ImGui::Button("Start an empty arena", ImVec2(-1, 0))) NewArena(g_sel);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Only the ring, the floor and the ringside parts: build the rest.\n"
                      "It plays in this arena's place (its room and VS screen style).");
  ImGui::Separator();
  ImGui::Text("Backstage areas");
  ImGui::TextDisabled("Rebuild a backstage brawl room.");
  static int area = 0;
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##area", kAreas[area].name)) {
    for (int k = 0; k < 7; ++k)
      if (ImGui::Selectable(kAreas[k].name, area == k)) area = k;
    ImGui::EndCombo();
  }
  if (ImGui::Button("Open area in Arena Editor", ImVec2(-1, 0))) OpenBackstage(area);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The game's backstage rooms are all in one file: the room you make is played in that\n"
                      "room's backstage brawls. Keep its floor and walls where they are (the game's\n"
                      "walls and cameras for the room stay). The cars, crates and other things the\n"
                      "superstars can use are placed by the game: they are not shown here and stay.\n"
                      "One backstage mod plays at a time.");
  ImGui::EndDisabled();
  ImGui::Separator();
  ImGui::Text("Your arena");
  if (g_proj.arena >= 0) {
    ImGui::TextDisabled("from %s%s", g_arenas[g_proj.arena].name, g_proj.fbx.empty() ? "" : ", edited in Blender");
  } else {
    ImGui::TextDisabled("import an FBX to start");
  }
  ImGui::InputText("Name", g_proj.name, sizeof g_proj.name);
  ImGui::InputText("Author (optional)", g_proj.author, sizeof g_proj.author);
  ImGui::InputText("Version (optional)", g_proj.version, sizeof g_proj.version);
  if (ImGui::Button("Banner picture (optional)...", ImVec2(-1, 0))) PickBanner();
  if (g_proj.banner_tex) ImGui::Image(Tex(g_proj.banner_tex), ImVec2(bw * 0.6f, bw * 0.3f));
  ImGui::Separator();
  ImGui::BeginDisabled(g_busy || g_proj.arena < 0);
  if (ImGui::Button("Save as mod (.svrmod)...", ImVec2(-1, 0))) SaveMod();
  if (ImGui::Button("Install into game", ImVec2(-1, 0))) InstallMod();
  ImGui::EndDisabled();
  if (g_busy) ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Working...");
  ImGui::EndChild();
  }  // arenas page
  // log
  ImGui::BeginChild("log", ImVec2(0, 0), true);
  {
    std::lock_guard lock(g_log_mutex);
    for (const auto& l : g_log) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();
  ImGui::End();
}

LRESULT CALLBACK WndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
  if (ImGui_ImplWin32_WndProcHandler(w, m, wp, lp)) return true;
  switch (m) {
    case WM_SIZE:
      if (g_dev && wp != SIZE_MINIMIZED) {
        if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
        g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
        CreateTarget();
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(w, m, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  // test aids: --editor <arena tile 0-19> opens the editor on it, --open <mod> opens a mod,
  // --editor-view <0-2> a camera preset, --select <name> an object
  int start_editor = -1, start_view = -1, start_new = -1, start_page = -1, start_backstage = -1;
  std::wstring start_mod, start_select;
  for (int i = 1; i + 1 < argc; ++i) {
    if (!wcscmp(argv[i], L"--game")) g_game = argv[i + 1];
    if (!wcscmp(argv[i], L"--editor")) start_editor = _wtoi(argv[i + 1]);
    if (!wcscmp(argv[i], L"--editor-view")) start_view = _wtoi(argv[i + 1]);
    if (!wcscmp(argv[i], L"--open")) start_mod = argv[i + 1];
    if (!wcscmp(argv[i], L"--select")) start_select = argv[i + 1];
    if (!wcscmp(argv[i], L"--test-edit-save")) g_test_save = argv[i + 1];
    if (!wcscmp(argv[i], L"--new-arena")) start_new = _wtoi(argv[i + 1]);
    if (!wcscmp(argv[i], L"--test-lib")) g_test_lib = _wtoi(argv[i + 1]);
    if (!wcscmp(argv[i], L"--page")) start_page = _wtoi(argv[i + 1]);
    if (!wcscmp(argv[i], L"--backstage")) start_backstage = _wtoi(argv[i + 1]);
    if (!wcscmp(argv[i], L"--star")) g_star_start = _wtoi(argv[i + 1]);
    if (!wcscmp(argv[i], L"--star-song")) g_star.song = argv[i + 1];
    if (!wcscmp(argv[i], L"--star-movie")) g_star.movie = argv[i + 1];
    if (!wcscmp(argv[i], L"--star-picture")) g_star_picture = argv[i + 1];
    if (!wcscmp(argv[i], L"--star-call")) g_star.call = _wtoi(argv[i + 1]);
    if (!wcscmp(argv[i], L"--star-voice")) g_star.voice = argv[i + 1];
    if (!wcscmp(argv[i], L"--star-model")) g_star_model = argv[i + 1];
    if (!wcscmp(argv[i], L"--star-name")) g_star_name = Utf8(argv[i + 1]);
    if (!wcscmp(argv[i], L"--sign")) g_sign_files.push_back(argv[i + 1]);
    if (!wcscmp(argv[i], L"--test-sign-save")) g_sign_test_save = argv[i + 1];
    if (!wcscmp(argv[i], L"--media-arena")) g_media_test_arena = _wtoi(argv[i + 1]);
    if (!wcscmp(argv[i], L"--media-video")) g_media_test_video = argv[i + 1];
    if (!wcscmp(argv[i], L"--media-picture")) g_media_test_pictures.push_back(argv[i + 1]);
    if (!wcscmp(argv[i], L"--test-media-save")) g_media_test_save = argv[i + 1];
    if (!wcscmp(argv[i], L"--test-star-save")) g_star_test_save = argv[i + 1];
  }
  WNDCLASSEXW wc = {sizeof wc, CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), nullptr,
                    nullptr, nullptr, L"SvR2011ModMaker", nullptr};
  RegisterClassExW(&wc);
  const UINT dpi = GetDpiForSystem();
  g_wnd = CreateWindowW(wc.lpszClassName, L"SvR2011 Mod Maker", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        MulDiv(1280, dpi, 96), MulDiv(800, dpi, 96), nullptr, nullptr, inst, nullptr);
  if (!CreateDevice(g_wnd)) {
    MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"SvR2011 Mod Maker", MB_ICONERROR);
    return 1;
  }
  ShowWindow(g_wnd, show);
  UpdateWindow(g_wnd);
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  {
    wchar_t fonts[MAX_PATH];
    GetWindowsDirectoryW(fonts, MAX_PATH);
    const std::string seg = Utf8(std::wstring(fonts) + L"\\Fonts\\segoeui.ttf");
    if (fs::exists(seg)) io.Fonts->AddFontFromFileTTF(seg.c_str(), 17.0f * dpi / 96.0f);
  }
  Style();
  ImGui::GetStyle().ScaleAllSizes(dpi / 96.0f);
  ImGui_ImplWin32_Init(g_wnd);
  ImGui_ImplDX11_Init(g_dev, g_ctx);
  if (g_game.empty() || !fs::exists(fs::path(g_game) / L"pac" / L"bg")) {
    Log("Choose the folder where the game is installed (it has the pac folder).");
    g_game = PickFolder(L"The installed game folder (with pac\\bg)");
  }
  if (!g_game.empty() && LoadBanners()) Log("Game folder: " + Utf8(g_game));
  else Log("No game folder: the arena files can't be read. Open the Mod Maker from the launcher's Mods tab.");
  editor::Hooks hooks;
  hooks.log = [](const std::string& s) { Log(s); };
  hooks.test_in_game = [] { TestInGame(); };
  if (!g_game.empty())
    for (int i = 0; i < 20; ++i) hooks.library.push_back({g_arenas[i].name, ArenaPath(i)});
  editor::Init(g_dev, g_ctx, hooks);
  char_preview::Init(g_dev, g_ctx);
  if (start_new >= 0 && start_new < 20) {
    g_sel = start_new;
    NewArena(start_new);
  }
  if (start_editor >= 0 && start_editor < 20) {
    g_sel = start_editor;
    OpenEditor(start_editor);
  }
  editor::TestStart(start_view, Utf8(start_select));  // (before the arena is set)
  if (start_page >= 0 && start_mod.empty()) g_page = start_page;
  if (start_backstage >= 0 && start_backstage < 7) OpenBackstage(start_backstage);
  if (!start_mod.empty()) {
    OpenModFile(start_mod);
    if (start_page >= 0) g_page = start_page;
    // test aid: --open <mod> --test-edit-save <file> saves it again unchanged and quits
    BuildJob job;
    if (!g_test_save.empty() && g_proj.edited && PrepareBuild(job)) {
      g_test_state = 1;
      const std::string out = Utf8(g_test_save);
      RunInBackground([job, out]() mutable {
        std::vector<ZipEntry> files;
        if (FinishBuild(job, files) && WriteFile(out, ZipWrite(files))) Log("test: saved " + out);
        g_test_state = 2;
      });
    }
  }

  bool done = false;
  while (!done) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    Draw();
    ImGui::Render();
    const float clear[4] = {0.086f, 0.090f, 0.106f, 1};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap->Present(1, 0);
  }
  if (g_worker.joinable()) g_worker.join();
  editor::Shutdown();
  char_preview::Shutdown();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  return 0;
}
