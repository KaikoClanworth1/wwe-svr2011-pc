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
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <functional>
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
#include "editor.h"
#include "svrfmt/arena.h"
#include "svrfmt/arena_import.h"
#include "svrfmt/png.h"
#include "svrfmt/ring_kit.h"
#include "svrfmt/zip_write.h"

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
};
Project g_proj;
int g_page = 0;  // 0 arenas, 1 editor
// test aid (--test-edit-save <file>): once the editor has its arena, editor::TestEdit,
// name "Test Edit", save the mod there and quit
std::wstring g_test_save;
std::atomic<int> g_test_state{0};
// an arena loaded in the background, handed to the UI thread (the editor holds g_proj.edited)
std::mutex g_pending_mutex;
std::unique_ptr<Arena> g_pending;
std::string g_pending_fbx;
bool g_pending_to_editor = false;

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
void OpenEditor(int i) {
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

// A saved mod back into the editor: its arena, name, banner and settings.
void OpenModFile(const std::wstring& f) {
  Bytes zip;
  std::vector<ZipEntry> files;
  if (!ReadFile(Utf8(f), zip) || !ZipRead(zip, files)) { Log("That is not a mod the Mod Maker made."); return; }
  const ZipEntry *manifest = nullptr, *pac = nullptr, *banner = nullptr;
  for (const auto& e : files) {
    if (e.name == "manifest.txt") manifest = &e;
    if (e.name == "arena.pac") pac = &e;
    if (e.name == "banner.dds") banner = &e;
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
  std::shared_ptr<Arena> arena;
  std::string manifest, id;
  Image banner;
};

bool PrepareBuild(BuildJob& job) {
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
  const auto halved = job.arena->FitFile({});
  if (!halved.empty())
    Log("  " + std::to_string(halved.size()) + " textures halved to fit the game's room for this arena.");
  std::string err;
  Bytes pac = job.arena->Save(&err);
  if (!err.empty()) { Log("error: " + err); return false; }
  files.push_back({"manifest.txt", Bytes(job.manifest.begin(), job.manifest.end())});
  files.push_back({"arena.pac", std::move(pac)});
  files.push_back({"banner.dds", DdsEncode(job.banner, DxtFormat::kDxt5, false)});
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
  const fs::path dir = fs::path(g_game) / L"Mods" / L"Arenas" / fs::u8path(id);
  std::error_code ec;
  fs::create_directories(dir, ec);
  for (const auto& f : files)
    if (!WriteFile(Utf8((dir / fs::u8path(f.name)).wstring()), f.data)) {
      Log("Could not write into " + Utf8(dir.wstring()) + " (is the game running?)");
      return false;
    }
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

// ---------------------------------------------------------------- UI

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
      if (!g_test_save.empty() && g_test_state == 0) {
        g_test_state = 1;
        editor::TestEdit();
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
  if (g_test_state == 2 && !g_busy) PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  ImGui::BeginChild("rail", ImVec2(rail, -logh), true);
  for (int p = 0; p < 2; ++p) {
    const char* names[] = {"Arenas", "Arena Editor"};
    if (g_page == p) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.06f, 0.18f, 1));
    if (ImGui::Button(names[p], ImVec2(-1, 0))) {
      if (p == 1 && !g_proj.edited) OpenEditor(g_sel);
      else g_page = p;
    }
    if (g_page == p) ImGui::PopStyleColor();
  }
  ImGui::Separator();
  ImGui::BeginDisabled();
  for (const char* t : {"Titantron videos", "Crowd & signs", "Menus & renders", "Audio", "Wrestlers"})
    ImGui::Button(t, ImVec2(-1, 0));
  ImGui::EndDisabled();
  ImGui::TextDisabled("later");
  ImGui::EndChild();
  ImGui::SameLine();
  if (g_page == 1) {
    ImGui::BeginChild("editor", ImVec2(0, -logh), false);
    editor::Draw();
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
  ImGui::EndDisabled();
  ImGui::Separator();
  ImGui::Text("Your arena");
  if (g_proj.arena >= 0) {
    ImGui::TextDisabled("from %s%s", g_arenas[g_proj.arena].name, g_proj.fbx.empty() ? "" : ", edited in Blender");
  } else {
    ImGui::TextDisabled("import an FBX to start");
  }
  ImGui::InputText("Name", g_proj.name, sizeof g_proj.name);
  ImGui::InputText("Author", g_proj.author, sizeof g_proj.author);
  ImGui::InputText("Version", g_proj.version, sizeof g_proj.version);
  if (ImGui::Button("Banner picture...", ImVec2(-1, 0))) PickBanner();
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
  int start_editor = -1, start_view = -1;
  std::wstring start_mod, start_select;
  for (int i = 1; i + 1 < argc; ++i) {
    if (!wcscmp(argv[i], L"--game")) g_game = argv[i + 1];
    if (!wcscmp(argv[i], L"--editor")) start_editor = _wtoi(argv[i + 1]);
    if (!wcscmp(argv[i], L"--editor-view")) start_view = _wtoi(argv[i + 1]);
    if (!wcscmp(argv[i], L"--open")) start_mod = argv[i + 1];
    if (!wcscmp(argv[i], L"--select")) start_select = argv[i + 1];
    if (!wcscmp(argv[i], L"--test-edit-save")) g_test_save = argv[i + 1];
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
  editor::Hooks hooks;
  hooks.log = [](const std::string& s) { Log(s); };
  hooks.test_in_game = [] { TestInGame(); };
  editor::Init(g_dev, g_ctx, hooks);

  if (g_game.empty() || !fs::exists(fs::path(g_game) / L"pac" / L"bg")) {
    Log("Choose the folder where the game is installed (it has the pac folder).");
    g_game = PickFolder(L"The installed game folder (with pac\\bg)");
  }
  if (!g_game.empty() && LoadBanners()) Log("Game folder: " + Utf8(g_game));
  else Log("No game folder: the arena files can't be read. Open the Mod Maker from the launcher's Mods tab.");
  if (start_editor >= 0 && start_editor < 20) {
    g_sel = start_editor;
    OpenEditor(start_editor);
  }
  if (!start_mod.empty()) {
    OpenModFile(start_mod);
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
  editor::TestStart(start_view, Utf8(start_select));

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
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  return 0;
}
