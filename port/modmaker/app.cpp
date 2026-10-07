// SvR2011 Mod Maker: the window, the shell (title bar, rail, status line,
// log, problems), projects, settings and the game data every page shares.
// Pages: arena_page.cpp (arenas, backstage, VS screen), star_page.cpp,
// signs_page.cpp, media_page.cpp, moves_page.cpp, assets_page.cpp,
// anims_page.cpp, icons_page.cpp, help_page.cpp; the 3D editor is editor.cpp.
#include "app.h"

#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#include "char_preview.h"
#include "editor.h"
#include "tool_run.h"
extern "C" {
#include "../launcher/movie_maker.h"
}
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "svrfmt/pac.h"
#include "svrfmt/png.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace mm {

using namespace svrfmt;

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
HWND g_wnd = nullptr;
std::wstring g_game;
float g_scale = 1;
bool g_behind = false;
PageId g_page = PageId::kArena;
std::wstring g_project_file;
PageId g_project_page = PageId::kArena;
Settings g_settings;

namespace {

IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

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

// ---------------------------------------------------------------- log / work

std::mutex g_log_mutex;
std::vector<std::string> g_log;
std::string g_status, g_progress;
std::atomic<bool> g_busy{false};
std::thread g_worker;
std::mutex g_ui_mutex;
std::vector<std::function<void()>> g_ui_queue;
std::map<PageId, int> g_touches;  // edits a page reported with Touch()

}  // namespace

ID3D11ShaderResourceView* MakeTexture(const Image& img) {
  if (img.rgba.empty() || !g_dev) return nullptr;
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

void Picture::Draw(float w, float h) {
  if (Empty()) {
    ImGui::Dummy(ImVec2(w, h));
    return;
  }
  const float k = std::min(w / image.w, h / image.h);
  const ImVec2 size(image.w * k, image.h * k);
  const ImVec2 at = ImGui::GetCursorPos();
  ImGui::SetCursorPos(ImVec2(at.x + (w - size.x) / 2, at.y + (h - size.y) / 2));
  ImGui::Image(Id(), size);
  ImGui::SetCursorPos(ImVec2(at.x, at.y + h));
  ImGui::Dummy(ImVec2(w, 0));
}

// ---------------------------------------------------------------- strings

std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Wide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
  return w;
}

std::string PathStr(const fs::path& p) { return Utf8(p.wstring()); }

#ifndef PORT_VERSION
#define PORT_VERSION "dev"
#endif
std::string MadeWith() { return std::string("made_with=Mod Maker ") + PORT_VERSION + "\n"; }
std::string Lower(std::string s) { for (char& c : s) c = char(std::tolower(uint8_t(c))); return s; }
std::string Upper(std::string s) { for (char& c : s) c = char(std::toupper(uint8_t(c))); return s; }

std::string IdFrom(const std::string& name, const char* fallback) {
  std::string id;
  for (char c : name) {
    if (std::isalnum(uint8_t(c))) id.push_back(char(std::tolower(uint8_t(c))));
    else if (!id.empty() && id.back() != '_') id.push_back('_');
  }
  while (!id.empty() && id.back() == '_') id.pop_back();
  return id.empty() ? fallback : id;
}

std::string FileName(const std::wstring& path) { return Utf8(fs::path(path).filename().wstring()); }

std::string Human(size_t bytes) {
  char b[32];
  if (bytes >= 1048576) std::snprintf(b, sizeof b, "%.1f MB", bytes / 1048576.0);
  else if (bytes >= 1024) std::snprintf(b, sizeof b, "%.0f KB", bytes / 1024.0);
  else std::snprintf(b, sizeof b, "%zu bytes", bytes);
  return b;
}

std::string Value(const std::string& text, const char* key) {
  const std::string k = std::string("\n") + key + "=";
  const std::string t = "\n" + text;
  const size_t at = t.find(k);
  if (at == std::string::npos) return {};
  size_t end = t.find_first_of("\r\n", at + k.size());
  if (end == std::string::npos) end = t.size();
  return t.substr(at + k.size(), end - at - k.size());
}

std::vector<std::string> Values(const std::string& text, const char* key) {
  std::vector<std::string> out;
  const std::string k = std::string("\n") + key + "=";
  const std::string t = "\n" + text;
  for (size_t at = t.find(k); at != std::string::npos; at = t.find(k, at + 1)) {
    size_t end = t.find_first_of("\r\n", at + k.size());
    if (end == std::string::npos) end = t.size();
    out.push_back(t.substr(at + k.size(), end - at - k.size()));
  }
  return out;
}

// ---------------------------------------------------------------- dialogs

const COMDLG_FILTERSPEC kPictureFilter[1] = {{L"Pictures (*.png, *.jpg, *.tga, *.bmp, *.dds)", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.dds"}};
const COMDLG_FILTERSPEC kSoundFilter[1] = {{L"Sounds (*.mp3, *.m4a, *.wav, *.flac, *.wma, *.ogg)", L"*.mp3;*.m4a;*.aac;*.wav;*.flac;*.wma;*.ogg"}};
const COMDLG_FILTERSPEC kBinkFilter[1] = {{L"Bink movie (*.bik)", L"*.bik"}};
const COMDLG_FILTERSPEC kPacFilter[1] = {{L"Character model pac (*.pac)", L"*.pac"}};
const COMDLG_FILTERSPEC kModFilter[1] = {{L"SvR2011 mod (*.svrmod)", L"*.svrmod"}};
const COMDLG_FILTERSPEC kProjectFilter[1] = {{L"Mod Maker project (*.svrproj)", L"*.svrproj"}};
const COMDLG_FILTERSPEC kOpenFilter[1] = {{L"Projects and mods (*.svrproj, *.svrmod)", L"*.svrproj;*.svrmod"}};

std::wstring PickFile(bool save, const wchar_t* title, const COMDLG_FILTERSPEC* spec, UINT nspec,
                      const wchar_t* def_ext, const wchar_t* def_name) {
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

bool LoadPicture(const std::wstring& file, Image& out) {
  if (Lower(FileName(file)).ends_with(".dds")) {
    Bytes d;
    if (ReadFile(Utf8(file), d) && DdsDecode(d, out)) return true;
  } else if (LoadImageFile(Utf8(file), out)) {
    return true;
  }
  Log("That picture could not be read: " + Utf8(file));
  return false;
}

// ---------------------------------------------------------------- log / work

void Log(const std::string& s) {
  std::lock_guard lock(g_log_mutex);
  g_log.push_back(s);
  if (g_log.size() > 2000) g_log.erase(g_log.begin(), g_log.begin() + 500);
}

void WriteLogTo(const std::wstring& file) {
  if (FILE* f = _wfopen(file.c_str(), L"wb")) {
    std::lock_guard lock(g_log_mutex);
    for (const auto& line : g_log) std::fprintf(f, "%s\n", line.c_str());
    std::fclose(f);
  }
}

void Status(const std::string& s) {
  Log(s);
  std::lock_guard lock(g_log_mutex);
  g_status = s;
}

void Progress(const std::string& what) {
  std::lock_guard lock(g_log_mutex);
  g_progress = what;
}

void RunInBackground(std::function<void()> fn) {
  if (g_busy) {
    Status("Still working on the last job: wait a moment.");
    return;
  }
  if (g_worker.joinable()) g_worker.join();
  g_busy = true;
  g_worker = std::thread([fn] {
    fn();
    {
      std::lock_guard lock(g_log_mutex);
      g_progress.clear();
    }
    g_busy = false;
  });
}

bool Busy() { return g_busy; }

void OnUiThread(std::function<void()> fn) {
  std::lock_guard lock(g_ui_mutex);
  g_ui_queue.push_back(std::move(fn));
}

void Touch() { ++g_touches[g_page == PageId::kEditor ? PageId::kArena : g_page]; }

// ---------------------------------------------------------------- problems

int ErrorCount(const std::vector<Problem>& p) {
  int n = 0;
  for (const auto& x : p) n += x.error;
  return n;
}

void DrawProblems(const std::vector<Problem>& problems) {
  for (const auto& p : problems) {
    ImGui::PushStyleColor(ImGuiCol_Text, p.error ? kBad : kWarn);
    ImGui::Bullet();
    ImGui::SameLine();
    ImGui::TextWrapped("%s%s%s", p.text.c_str(), p.fix.empty() ? "" : "  -  ", p.fix.c_str());
    ImGui::PopStyleColor();
  }
}

int ModButtons(const std::vector<Problem>& problems, bool with_test, float width) {
  static int pending = 0;  // (a press waiting for the warnings to be confirmed)
  const int errors = ErrorCount(problems);
  const bool warnings = int(problems.size()) > errors;
  const float w = (width > 0 ? width : 200) * g_scale;
  int pressed = 0;
  ImGui::BeginDisabled(errors > 0 || Busy());
  PushAccent();
  if (ImGui::Button("Save as mod (.svrmod)...", ImVec2(w, 0))) pressed = 1;
  PopAccent();
  ImGui::SameLine();
  if (ImGui::Button("Install into game", ImVec2(w, 0))) pressed = 2;
  if (with_test) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, kGood);
    if (ImGui::Button("Test in game", ImVec2(w, 0))) pressed = 3;
    ImGui::PopStyleColor();
  }
  ImGui::EndDisabled();
  if (errors > 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    std::string tip = "Fix the problems first:";
    for (const auto& p : problems)
      if (p.error) tip += "\n- " + p.text;
    ImGui::SetTooltip("%s", tip.c_str());
  }
  if (pressed && warnings) {
    pending = pressed;
    ImGui::OpenPopup("Warnings");
    pressed = 0;
  }
  if (ImGui::BeginPopupModal("Warnings", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("The mod will work, but:");
    DrawProblems(problems);
    ImGui::Separator();
    const char* go = pending == 1 ? "Save anyway" : pending == 2 ? "Install anyway" : "Test anyway";
    if (ImGui::Button(go, ImVec2(150 * g_scale, 0))) {
      pressed = pending;
      pending = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Go back", ImVec2(150 * g_scale, 0))) {
      pending = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  return pressed;
}

// ---------------------------------------------------------------- game data

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

std::string ArenaPath(int i) {
  char b[32];
  std::snprintf(b, sizeof b, "bg%02d.pac", g_arenas[i].number);
  return PathStr(fs::path(g_game) / L"pac" / L"bg" / b);
}

int ArenaTileOf(int number) {
  for (int i = 0; i < 20; ++i)
    if (g_arenas[i].number == number) return i;
  return -1;
}

namespace {

bool LoadBanners() {
  Bytes d;
  Epac e;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"menu" / L"MatchHD.pac"), d) || !EpacRead(d, e)) return false;
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
            if (t.name == a.banner && a.tile.Empty()) {
              Image img;
              if (DdsDecode(t.data, img)) a.tile.Set(std::move(img));
            }
      }
    }
  return true;
}

std::vector<StarInfo> g_stars;
std::map<int, std::array<int, 7>> g_ratings;
bool g_stars_loaded = false;
std::map<int, Picture> g_renders;

// Names and ratings from chEtc.pac CHAR/DAT (260-byte records after a 4-byte
// header: ratings at +0, the full name at +34); the playable ones are those
// with a render in DLC_HD.pac.
void LoadStars() {
  g_stars_loaded = true;
  if (g_game.empty()) return;
  Bytes d;
  Epac e;
  std::map<int, std::string> names;
  if (ReadFile(PathStr(fs::path(g_game) / L"pac" / L"ch" / L"chEtc.pac"), d) && EpacRead(d, e))
    for (const auto& g : e.groups)
      for (const auto& en : g.entries)
        if (g.type == "CHAR" && en.name.rfind("DAT", 0) == 0) {
          std::vector<PachEntry> pe;
          if (!PachRead(en.data, pe)) continue;
          for (const auto& x : pe) {
            const Bytes r = Unpack(x.data);
            if (r.size() < 4 + 66) continue;
            const char* nm = reinterpret_cast<const char*>(&r[4 + 34]);
            names[int(x.id)] = std::string(nm, strnlen(nm, 32));
            std::array<int, 7> v{};
            for (int j = 0; j < 7; ++j) v[j] = r[4 + j];
            g_ratings[int(x.id)] = v;
          }
        }
  Bytes h;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"DLC_HD.pac"), h) || !EpacRead(h, e)) return;
  for (const auto& g : e.groups)
    if (g.type == "SSFA")
      for (const auto& en : g.entries) {
        const int id = std::atoi(en.name.c_str());
        if (id < 100 || id > 321) continue;  // (attires 1000+, DLC ids)
        g_stars.push_back({id, names.count(id) ? names[id] : "Superstar " + std::to_string(id)});
      }
  std::sort(g_stars.begin(), g_stars.end(), [](const StarInfo& a, const StarInfo& b) { return a.name < b.name; });
}

}  // namespace

const std::vector<StarInfo>& Stars() {
  if (!g_stars_loaded) LoadStars();
  return g_stars;
}

const StarInfo* StarById(int id) {
  for (const auto& s : Stars())
    if (s.id == id) return &s;
  return nullptr;
}

int StarRating(int id, int k) {
  Stars();
  const auto it = g_ratings.find(id);
  return it == g_ratings.end() ? 74 : std::clamp(it->second[k], 1, 99);
}

Picture* StarRender(int id) {
  auto it = g_renders.find(id);
  if (it != g_renders.end()) return &it->second;
  Picture& p = g_renders[id];
  Bytes h;
  Epac e;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"DLC_HD.pac"), h) || !EpacRead(h, e)) return &p;
  char key[8];
  std::snprintf(key, sizeof key, "%04d", id);
  for (const auto& g : e.groups)
    if (g.type == "SSFA")
      for (const auto& en : g.entries)
        if (en.name == key) {
          Image img;
          if (DdsDecode(Unpack(en.data), img)) p.Set(std::move(img));  // (BPE-packed DDS)
        }
  return &p;
}

bool ReadPacIndex(const fs::path& file, PacIndex& out) {
  out = PacIndex();
  FILE* f = _wfopen(file.c_str(), L"rb");
  if (!f) return false;
  Bytes idx(0x4000);
  const bool ok = std::fread(idx.data(), 1, idx.size(), f) == idx.size() &&
                  (!std::memcmp(idx.data(), "EPAC", 4) || !std::memcmp(idx.data(), "EPK8", 4));
  std::fclose(f);
  if (!ok) return false;
  out.epk8 = !std::memcmp(idx.data(), "EPK8", 4);
  const size_t esz = out.epk8 ? 16 : 12, nlen = out.epk8 ? 8 : 4;
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!Le32(&idx[p])) break;
    const std::string group(reinterpret_cast<const char*>(&idx[p]), 4);
    const uint32_t cnt = out.epk8 ? Le16(&idx[p + 4]) : Le32(&idx[p + 4]) / 3;
    p += 12;
    for (uint32_t i = 0; i < cnt && p + esz <= 0x4000; ++i, p += esz) {
      PacEntryInfo e;
      e.group = group;
      const char* n = reinterpret_cast<const char*>(&idx[p]);
      e.name.assign(n, strnlen(n, nlen));
      e.offset = 0x4000 + uint64_t(Le32(&idx[p + nlen])) * 0x800;
      e.size = Le32(&idx[p + nlen + 4]) * 0x100;
      out.entries.push_back(std::move(e));
    }
  }
  return true;
}

bool ReadPacEntry(const fs::path& file, const PacEntryInfo& e, Bytes& out) {
  FILE* f = _wfopen(file.c_str(), L"rb");
  if (!f) return false;
  out.resize(e.size);
  const bool ok = _fseeki64(f, int64_t(e.offset), SEEK_SET) == 0 && std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

bool ReadPacEntry(const fs::path& file, const char* name, Bytes& out, const char* group) {
  PacIndex idx;
  if (!ReadPacIndex(file, idx)) return false;
  for (const auto& e : idx.entries)
    if (e.name == name && (!group || e.group == group)) return ReadPacEntry(file, e, out);
  return false;
}

namespace {
std::map<int, std::map<int, std::string>> g_strings;  // table -> id -> text
bool g_strings_loaded = false;

// string.pac: EPAC group SDB, entry "64LL" = every table as PACH ids 0..13 (LL 00 = English;
// IT's is misnamed, so the entry with 14 tables is taken when 6400 is missing). A table:
// {u32 0, u32 count} then 16-byte records {str_off, str_len, id, 0} sorted by id, UTF-8 text.
void LoadStrings() {
  g_strings_loaded = true;
  if (g_game.empty()) return;
  PacIndex idx;
  const fs::path file = fs::path(g_game) / L"pac" / L"string.pac";
  if (!ReadPacIndex(file, idx)) return;
  const PacEntryInfo* pick = nullptr;
  for (const auto& e : idx.entries)
    if (e.name == "6400") pick = &e;
  Bytes d;
  if (!pick || !ReadPacEntry(file, *pick, d)) return;
  std::vector<PachEntry> tables;
  if (!PachRead(d, tables)) return;
  for (const auto& t : tables) {
    const Bytes& b = t.data;  // (not BPE)
    if (b.size() < 8) continue;
    const uint32_t n = Le32(&b[4]);
    auto& m = g_strings[int(t.id)];
    for (uint32_t k = 0; k < n && 8 + 16 * size_t(k + 1) <= b.size(); ++k) {
      const uint8_t* r = &b[8 + 16 * k];
      const uint32_t off = Le32(r), len = Le32(r + 4), id = Le32(r + 8);
      if (off >= b.size()) continue;
      const char* s = reinterpret_cast<const char*>(&b[off]);
      m[int(id)] = std::string(s, strnlen(s, std::min<size_t>(len ? len : 1, b.size() - off)));
    }
  }
}
}  // namespace

const std::string& GameString(int id) {
  static const std::string none;
  if (!g_strings_loaded) LoadStrings();
  std::vector<int> order;
  if (id >= 50000) order = {1};
  else if (id >= 45000) order = {2};
  else if (id >= 40000) order = {0};
  else { for (int t = 3; t <= 13; ++t) order.push_back(t); order.push_back(0); }
  for (int t : order) {
    const auto tb = g_strings.find(t);
    if (tb == g_strings.end()) continue;
    const auto it = tb->second.find(id);
    if (it != tb->second.end()) return it->second;
  }
  return none;
}

namespace {
std::map<int, std::string> g_move_names;
bool g_move_names_loaded = false;
}  // namespace

const std::map<int, std::string>& MoveNames() {
  if (g_move_names_loaded) return g_move_names;
  g_move_names_loaded = true;
  if (g_game.empty()) return g_move_names;
  Bytes d;
  Epac e;
  if (!ReadFile(PathStr(fs::path(g_game) / L"pac" / L"misc.pac"), d) || !EpacRead(d, e)) return g_move_names;
  for (const auto& g : e.groups) {
    if (g.type != "MOVS") continue;
    for (const auto& en : g.entries) {
      if (en.name.rfind("WAZE", 0) != 0) continue;
      const Bytes raw = Unpack(en.data);
      // the records start 0x10 before "* test motion *" (the first record's name)
      const char* probe = "* test motion *";
      size_t at = std::string::npos;
      for (size_t i = 0; i + 16 < raw.size(); ++i)
        if (!std::memcmp(&raw[i], probe, 15)) { at = i - 0x10; break; }
      if (at == std::string::npos || raw.size() < 8) continue;
      const uint32_t count = Le32(&raw[4]);
      for (uint32_t k = 0; k < count && at + 160 * (k + 1) <= raw.size(); ++k) {
        const uint8_t* r = &raw[at + 160 * k];
        const int id = Le16(r + 0x90);
        const char* nm = reinterpret_cast<const char*>(r + 0x10);
        const std::string name(nm, strnlen(nm, 64));
        if (id && !name.empty() && !g_move_names.count(id)) g_move_names[id] = name;
      }
    }
  }
  return g_move_names;
}

const std::string& MoveName(int id) {
  static const std::string none;
  const auto& m = MoveNames();
  const auto it = m.find(id);
  return it == m.end() ? none : it->second;
}

// A game running from this game folder (another copy's test game doesn't count).
bool GameRunning() {
  if (g_game.empty()) return false;
  static std::wstring last_game;
  static bool last_result = false;
  static double last_time = -10;
  const double now = ImGui::GetTime();
  if (last_game == g_game && now - last_time < 1.0) return last_result;  // (asked every frame)
  last_game = g_game, last_time = now;
  last_result = false;
  const std::wstring want = Lower(PathStr(fs::path(g_game) / L"svr2011.exe")) == "" ? L"" : (fs::path(g_game) / L"svr2011.exe").wstring();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  PROCESSENTRY32W pe = {sizeof pe};
  for (BOOL ok = Process32FirstW(snap, &pe); ok && !last_result; ok = Process32NextW(snap, &pe)) {
    if (_wcsicmp(pe.szExeFile, L"svr2011.exe")) continue;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)) {
      wchar_t path[MAX_PATH * 2];
      DWORD n = DWORD(std::size(path));
      if (QueryFullProcessImageNameW(h, 0, path, &n) && !_wcsicmp(path, want.c_str())) last_result = true;
      CloseHandle(h);
    }
  }
  CloseHandle(snap);
  return last_result;
}

void StartGame(const std::wstring& extra_env) {
  const std::wstring exe = (fs::path(g_game) / L"svr2011.exe").wstring();
  // "NAME=value;NAME=value": set for the child only
  std::vector<std::wstring> set;
  for (size_t at = 0; at < extra_env.size();) {
    size_t end = extra_env.find(L';', at);
    if (end == std::wstring::npos) end = extra_env.size();
    const std::wstring kv = extra_env.substr(at, end - at);
    const size_t eq = kv.find(L'=');
    if (eq != std::wstring::npos) {
      SetEnvironmentVariableW(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str());
      set.push_back(kv.substr(0, eq));
    }
    at = end + 1;
  }
  STARTUPINFOW si = {sizeof si};
  PROCESS_INFORMATION pi = {};
  std::wstring cmd = L"\"" + exe + L"\"";
  if (g_behind) {  // a test: the game behind everything, muted, its log where the test reads it
    SetEnvironmentVariableW(L"SVR2011_WINDOW_BEHIND", L"1");
    set.push_back(L"SVR2011_WINDOW_BEHIND");
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    cmd += L" --monitor=2 --audio_mute=true --fullscreen=false --log_file=\"" + std::wstring(tmp) + L"svr2011_modmaker_play.log\"";
  }
  if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, g_behind ? BELOW_NORMAL_PRIORITY_CLASS : 0, nullptr,
                     g_game.c_str(), &si, &pi)) {
    Log("game started: pid " + std::to_string(pi.dwProcessId));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Status("The game is starting.");
  } else {
    Status("Could not start " + Utf8(exe) + ".");
  }
  for (const auto& n : set) SetEnvironmentVariableW(n.c_str(), nullptr);
}

// ---------------------------------------------------------------- widgets

bool FileRow(const char* label, std::wstring& file, const char* none, const COMDLG_FILTERSPEC* spec, UINT nspec,
             const char* tip, float width) {
  bool changed = false;
  ImGui::PushID(label);
  if (ImGui::Button(label, ImVec2(width * g_scale, 0))) {
    const std::wstring p = PickFile(false, Wide(label).c_str(), spec, nspec);
    if (!p.empty()) file = p, changed = true;
  }
  if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  ImGui::SameLine();
  if (file.empty()) ImGui::TextDisabled("%s", none);
  else ImGui::TextUnformatted(FileName(file).c_str());
  if (!file.empty()) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Utf8(file).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) file.clear(), changed = true;
  }
  ImGui::PopID();
  return changed;
}

bool TextField(const char* label, char* buf, size_t size, const char* hint, float width) {
  ImGui::SetNextItemWidth(width * g_scale);
  return hint ? ImGui::InputTextWithHint(label, hint, buf, size) : ImGui::InputText(label, buf, size);
}

void Hint(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) {
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(420 * g_scale);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }
}

void Heading(const char* title, const char* about) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.35f);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  if (about) ImGui::TextDisabled("%s", about);
  ImGui::Spacing();
}

void PushAccent() {
  ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.1f, 0.25f, 1));
}
void PopAccent() { ImGui::PopStyleColor(2); }

void MakeRenders(const Image& src, Image& render, Image& bust, Image& icon) {
  // fitted into 512 x 512 (top aligned, centred); the bust (SSFC): that at
  // 3/4 size, its top 256 rows around the figure's middle; the face icon:
  // the bust's top middle at 64 x 64
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
  double sx = 0, sa = 0;
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 384; ++x) {
      const double a = three.rgba[(size_t(y) * 384 + x) * 4 + 3];
      sx += a * x, sa += a;
    }
  const int cx = sa > 0 ? int(sx / sa) : 192;
  const int left = std::clamp(cx - 128, 0, 384 - 256);
  bust.w = bust.h = 256;
  bust.rgba.resize(size_t(256) * 256 * 4);
  for (int y = 0; y < 256; ++y)
    std::memcpy(&bust.rgba[size_t(y) * 256 * 4], &three.rgba[(size_t(y) * 384 + left) * 4], 256 * 4);
  Image head;
  head.w = head.h = 128;
  head.rgba.resize(size_t(128) * 128 * 4);
  for (int y = 0; y < 128; ++y)
    std::memcpy(&head.rgba[size_t(y) * 128 * 4], &bust.rgba[(size_t(y) * 256 + 64) * 4], 128 * 4);
  icon = Resize(head, 64, 64);
  render = std::move(big);
}

void MakeBink(const std::wstring& video, const std::wstring& out, int fit, int seconds, std::function<void(std::wstring)> done) {
  if (Busy()) {
    Status("Still working on the last job: wait a moment.");
    return;
  }
  RunInBackground([video, out, fit, seconds, done] {
    Progress("Making the Bink movie (" + FileName(video) + ") ...");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MovieJob job = {};
    wcsncpy_s(job.video, video.c_str(), _TRUNCATE);
    wcsncpy_s(job.out, out.c_str(), _TRUNCATE);
    job.fit = fit;
    job.max_seconds = seconds;
    const bool ok = movie_make(&job) != 0;
    CoUninitialize();
    const std::wstring err = job.err;
    const int frames = job.frames;
    OnUiThread([ok, out, err, frames, done] {
      if (ok) Status("Bink movie made: " + FileName(out) + " (" + std::to_string(frames) + " frames).");
      else Status("The movie could not be made: " + Utf8(err));
      if (done) done(ok ? out : L"");
    });
  });
}

bool BinkFromVideoButton(const char* label, std::wstring& movie, const std::string& stem) {
  ImGui::PushID(label);
  bool pressed = false;
  if (ImGui::Button(label, ImVec2(220 * g_scale, 0))) {
    const COMDLG_FILTERSPEC spec[] = {{L"Videos and pictures", L"*.mp4;*.mov;*.m4v;*.avi;*.wmv;*.mkv;*.webm;*.png;*.jpg;*.jpeg;*.bmp"}};
    const std::wstring f = PickFile(false, L"A video (or a picture) for the entrance movie", spec, 1);
    if (!f.empty()) {
      const fs::path work = fs::path(g_game) / L"Mods" / L".convert";
      std::error_code ec;
      fs::create_directories(work, ec);
      const std::wstring out = (work / (Wide(IdFrom(stem, "movie")) + L"_" + fs::path(f).stem().wstring() + L".bik")).wstring();
      std::wstring* target = &movie;
      MakeBink(f, out, 0, 0, [target](std::wstring made) { if (!made.empty()) *target = made, Touch(); });
      pressed = true;
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Any video or picture -> a 320 x 320 Bink laid out like the game's titantron movies (the "
                      "launcher's Movies tab does the same, with a preview and the bottom strip).");
  ImGui::PopID();
  return pressed;
}

// ---------------------------------------------------------------- settings

namespace {

fs::path SettingsDir() {
  wchar_t* p = nullptr;
  fs::path dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = fs::path(p) / L"SvR2011 Mod Maker";
  if (p) CoTaskMemFree(p);
  std::error_code ec;
  fs::create_directories(dir, ec);
  return dir;
}

void LoadSettings() {
  Bytes d;
  if (!ReadFile(PathStr(SettingsDir() / L"settings.txt"), d)) return;
  const std::string t(d.begin(), d.end());
  g_settings.game = Wide(Value(t, "game"));
  for (const auto& r : Values(t, "recent")) g_settings.recent.push_back(Wide(r));
  g_settings.win_x = std::atoi(Value(t, "win_x").c_str());
  g_settings.win_y = std::atoi(Value(t, "win_y").c_str());
  g_settings.win_w = std::atoi(Value(t, "win_w").c_str());
  g_settings.win_h = std::atoi(Value(t, "win_h").c_str());
  g_settings.maximized = Value(t, "maximized") == "1";
  g_settings.log_open = Value(t, "log_open") == "1";
  if (!Value(t, "font").empty()) g_settings.font = std::clamp(std::atoi(Value(t, "font").c_str()), 12, 28);
}

}  // namespace

void SaveSettings() {
  std::string t = "game=" + Utf8(g_settings.game) + "\n";
  for (const auto& r : g_settings.recent) t += "recent=" + Utf8(r) + "\n";
  t += "win_x=" + std::to_string(g_settings.win_x) + "\nwin_y=" + std::to_string(g_settings.win_y) +
       "\nwin_w=" + std::to_string(g_settings.win_w) + "\nwin_h=" + std::to_string(g_settings.win_h) +
       "\nmaximized=" + (g_settings.maximized ? "1" : "0") + "\nlog_open=" + (g_settings.log_open ? "1" : "0") +
       "\nfont=" + std::to_string(g_settings.font) + "\n";
  WriteFile(PathStr(SettingsDir() / L"settings.txt"), Bytes(t.begin(), t.end()));
}

void AddRecent(const std::wstring& file) {
  auto& r = g_settings.recent;
  r.erase(std::remove(r.begin(), r.end(), file), r.end());
  r.insert(r.begin(), file);
  if (r.size() > 8) r.resize(8);
  SaveSettings();
}

// ---------------------------------------------------------------- pages

namespace {

struct PageDesc {
  PageId id;
  const char* name;
  const char* section;  // rail heading ("" = continues the last)
  PageHooks* hooks;     // nullptr: not a mod type
  void (*draw)();
};

void DrawEditor() {
  editor::Draw();
}

const PageDesc kPages[] = {
    {PageId::kArena, "Arena", "Make", &arena_page::hooks, nullptr},
    {PageId::kEditor, "   3D editor", "", nullptr, DrawEditor},
    {PageId::kBackstage, "Backstage", "", &backstage_page::hooks, nullptr},
    {PageId::kStar, "Superstar", "", &star_page::hooks, nullptr},
    {PageId::kMoves, "Moves", "", &moves_page::hooks, nullptr},
    {PageId::kSigns, "Crowd signs", "", &signs_page::hooks, nullptr},
    {PageId::kMedia, "Media", "", &media_page::hooks, nullptr},
    {PageId::kMatch, "Match types", "", &match_page::hooks, nullptr},
    {PageId::kOther, "Other games", "", nullptr, other_page::Draw},
    {PageId::kCaw, "CAW pictures", "Saves", nullptr, caw_page::Draw},
    {PageId::kAssets, "Game assets", "Look", nullptr, assets_page::Draw},
    {PageId::kAnims, "Animations", "", nullptr, anims_page::Draw},
    {PageId::kIcons, "Icons & renders", "", nullptr, icons_page::Draw},
    {PageId::kHelp, "Manual", "Help", nullptr, help_page::Draw},
};

const PageDesc& Desc(PageId p) {
  for (const auto& d : kPages)
    if (d.id == p) return d;
  return kPages[0];
}

PageHooks* Hooks(PageId p) { return Desc(p).hooks; }

// Each mod page is its own project: its state when last saved / opened, its file.
std::map<PageId, std::string> g_saved_state;
std::map<PageId, int> g_saved_touches;
std::map<PageId, std::wstring> g_project_files;

std::string CurrentState(PageId page) {
  PageHooks* h = Hooks(page);
  std::string s = h && h->state ? h->state() : std::string();
  if (page == PageId::kArena || page == PageId::kBackstage) s += "|" + std::to_string(editor::EditCount());
  return s;
}

void MarkSaved(PageId page) {
  g_saved_state[page] = CurrentState(page);
  g_saved_touches[page] = g_touches[page];
}

void MarkAllSaved() {
  for (const auto& d : kPages)
    if (d.hooks) MarkSaved(d.id);
}

// The shown page decides the project (the 3D editor belongs to the arena /
// backstage page whose arena it holds).
void FollowPage() {
  PageId p = g_page;
  if (p == PageId::kEditor) p = Value(CurrentState(PageId::kBackstage), "backstage") != "-1" ? PageId::kBackstage : PageId::kArena;
  if (Hooks(p)) {
    g_project_page = p;
    g_project_file = g_project_files[p];
  }
}

}  // namespace

const char* PageName(PageId p) {
  const char* n = Desc(p).name;
  while (*n == ' ') ++n;
  return n;
}

void GoTo(PageId p) { g_page = p; }

bool ProjectDirty() {
  return g_saved_state.count(g_project_page) && (CurrentState(g_project_page) != g_saved_state[g_project_page] ||
                                                 g_touches[g_project_page] != g_saved_touches[g_project_page]);
}

void ProjectReset() {
  for (const auto& d : kPages)
    if (d.hooks && d.hooks->reset) d.hooks->reset();
  g_project_files.clear();
  g_project_file.clear();
  MarkAllSaved();
}

// A new project of one type: that page emptied (the others keep their work).
void ProjectNew(PageId type) {
  if (PageHooks* h = Hooks(type); h && h->reset) h->reset();
  if (type == PageId::kArena || type == PageId::kBackstage) {
    if (PageHooks* o = Hooks(type == PageId::kArena ? PageId::kBackstage : PageId::kArena); o && o->reset) o->reset();
  }
  g_project_files[type].clear();
  g_page = type;
  FollowPage();
  MarkSaved(type);
  Status(std::string("New ") + PageName(type) + " project.");
}

// ---- ProjectOut / ProjectIn

std::string ProjectOut::File(const std::string& key, const std::wstring& path, const std::string& as) {
  if (path.empty()) return {};
  Bytes d;
  if (!ReadFile(Utf8(path), d)) {
    Log("Could not read " + Utf8(path) + " (left out of the project).");
    return {};
  }
  std::string name = as + Utf8(fs::path(path).extension().wstring());
  files.push_back({name, std::move(d)});
  text += key + "=" + name + "\n";
  text += key + ".from=" + Utf8(path) + "\n";
  return name;
}

void ProjectOut::Png(const std::string& key, const Image& img, const std::string& as) {
  if (img.rgba.empty()) return;
  // (PNG through WIC needs a file: the project keeps pictures as RGBA DDS instead - lossless, quick)
  files.push_back({as + ".dds", DdsEncode(img, DxtFormat::kArgb, false)});
  text += key + "=" + as + ".dds\n";
}

const ZipEntry* ProjectIn::Find(const std::string& name) const {
  for (const auto& e : files)
    if (e.name == name) return &e;
  return nullptr;
}

std::wstring ProjectIn::Extract(const std::string& name) const {
  const ZipEntry* e = Find(name);
  if (!e) return {};
  wchar_t* p = nullptr;
  fs::path cache;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)))
    cache = fs::path(p) / L"SvR2011 Mod Maker" / L"projects" / dir.stem();
  if (p) CoTaskMemFree(p);
  std::error_code ec;
  fs::create_directories(cache, ec);
  const fs::path out = cache / fs::u8path(name);
  if (!WriteFile(PathStr(out), e->data)) return {};
  return out.wstring();
}

bool ProjectIn::Png(const char* key, Image& out) const {
  const std::string n = Get(key);
  const ZipEntry* e = n.empty() ? nullptr : Find(n);
  return e && DdsDecode(e->data, out);
}

bool ProjectSave(bool ask_name) {
  PageHooks* h = Hooks(g_project_page);
  if (!h || !h->write) {
    Status("This page has nothing to save as a project.");
    return false;
  }
  std::wstring f = g_project_file;
  if (ask_name || f.empty()) {
    const std::string stem = IdFrom(h->state ? Value(h->state(), "name") : "", PageName(g_project_page));
    f = PickFile(true, L"Save the project", kProjectFilter, 1, L"svrproj", (Wide(stem) + L".svrproj").c_str());
    if (f.empty()) return false;
  }
  if (Busy()) {
    Status("Still working on the last job: save in a moment.");
    return false;
  }
  ProjectOut out;
  out.text = std::string("svrproj=1\ntype=") + h->type + "\n";
  h->write(out);
  out.files.insert(out.files.begin(), ZipEntry{"project.txt", Bytes(out.text.begin(), out.text.end())});
  g_project_file = f;
  g_project_files[g_project_page] = f;
  AddRecent(f);
  const std::string path = Utf8(f);
  if (out.deferred.empty()) {
    if (!WriteFile(path, ZipWrite(out.files))) {
      Status("The project could not be written: " + path);
      return false;
    }
    MarkSaved(g_project_page);
    Status("Project saved: " + path);
    return true;
  }
  // the slow part in the background; the page counts as saved once the file is written
  // (the state it holds is the one gathered now)
  auto shared = std::make_shared<ProjectOut>(std::move(out));
  const PageId page = g_project_page;
  const std::string state = CurrentState(page);
  const int touches = g_touches[page];
  RunInBackground([shared, path, page, state, touches] {
    Progress("Saving the project ...");
    for (auto& fn : shared->deferred) fn(shared->files);
    const bool ok = WriteFile(path, ZipWrite(shared->files));
    OnUiThread([ok, page, state, touches, path] {
      if (ok) g_saved_state[page] = state, g_saved_touches[page] = touches;
      Status(ok ? "Project saved: " + path : "The project could not be written: " + path);
    });
  });
  return true;
}

bool ProjectOpen(const std::wstring& file) {
  Bytes zip;
  ProjectIn in;
  in.dir = fs::path(file);
  in.dir = in.dir.parent_path() / in.dir.stem();
  if (!ReadFile(Utf8(file), zip) || !ZipRead(zip, in.files)) {
    Status("That is not a project or a mod the Mod Maker can open: " + Utf8(file));
    return false;
  }
  const bool mod = Lower(FileName(file)).ends_with(".svrmod");
  const ZipEntry* txt = in.Find(mod ? "manifest.txt" : "project.txt");
  if (!txt) {
    Status(mod ? "That mod has no manifest.txt." : "That project has no project.txt.");
    return false;
  }
  in.text.assign(txt->data.begin(), txt->data.end());
  const std::string type = Value(in.text, "type");
  const PageDesc* page = nullptr;
  for (const auto& d : kPages)
    if (d.hooks && d.hooks->type && type == d.hooks->type) page = &d;
  if (!page) {
    Status("The Mod Maker doesn't know the mod type \"" + type + "\".");
    return false;
  }
  ProjectNew(page->id);
  const auto& fn = mod ? page->hooks->read_mod : page->hooks->read;
  if (!fn || !fn(in)) {
    Status("Could not open " + Utf8(file) + ".");
    return false;
  }
  g_project_files[page->id] = mod ? L"" : file;
  FollowPage();
  AddRecent(file);
  MarkSaved(page->id);
  Status("Opened " + Utf8(file) + (mod ? " (a mod: save it as a project to keep working on it)." : "."));
  return true;
}

// ---------------------------------------------------------------- the shell

namespace {

void Style() {
  ImGuiStyle& s = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  s.WindowRounding = 0;
  s.FrameRounding = 4;
  s.ChildRounding = 6;
  s.PopupRounding = 6;
  s.FramePadding = ImVec2(10, 6);
  s.ItemSpacing = ImVec2(10, 8);
  s.ScrollbarSize = 12;
  s.WindowBorderSize = 0;
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.086f, 0.090f, 0.106f, 1);
  c[ImGuiCol_ChildBg] = ImVec4(0.110f, 0.114f, 0.133f, 1);
  c[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.125f, 0.15f, 1);
  c[ImGuiCol_Button] = ImVec4(0.165f, 0.176f, 0.208f, 1);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.28f, 0.33f, 1);
  c[ImGuiCol_ButtonActive] = ImVec4(0.78f, 0.06f, 0.18f, 1);
  c[ImGuiCol_Header] = ImVec4(0.78f, 0.06f, 0.18f, 0.6f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.78f, 0.06f, 0.18f, 0.8f);
  c[ImGuiCol_FrameBg] = ImVec4(0.165f, 0.176f, 0.208f, 1);
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.24f, 0.28f, 1);
  c[ImGuiCol_Tab] = ImVec4(0.14f, 0.15f, 0.18f, 1);
  c[ImGuiCol_TabHovered] = ImVec4(0.5f, 0.08f, 0.16f, 1);
  c[ImGuiCol_TabSelected] = ImVec4(0.78f, 0.06f, 0.18f, 1);
  c[ImGuiCol_CheckMark] = ImVec4(1, 0.3f, 0.4f, 1);
  c[ImGuiCol_SliderGrab] = ImVec4(0.78f, 0.06f, 0.18f, 1);
  c[ImGuiCol_SliderGrabActive] = ImVec4(1, 0.2f, 0.35f, 1);
  c[ImGuiCol_Separator] = ImVec4(0.25f, 0.26f, 0.3f, 1);
}

bool g_show_problems = false;
std::vector<Problem> g_problems;  // the project page's, this frame

void PickGameFolder() {
  const std::wstring d = PickFolder(L"The installed game folder (it has the pac folder)");
  if (d.empty()) return;
  if (!fs::exists(fs::path(d) / L"pac" / L"bg")) {
    Status("That folder has no pac\\bg: it isn't the game folder.");
    return;
  }
  g_game = d;
  g_settings.game = d;
  SaveSettings();
  for (auto& a : g_arenas) a.tile.Clear();
  LoadBanners();
  g_stars_loaded = false;
  g_stars.clear();
  g_renders.clear();
  Status("Game folder: " + Utf8(d));
}

void NewMenu() {
  if (ImGui::BeginPopup("new_menu")) {
    ImGui::TextDisabled("A new mod:");
    for (const auto& d : kPages)
      if (d.hooks && ImGui::MenuItem(PageName(d.id))) ProjectNew(d.id);
    ImGui::EndPopup();
  }
}

void TitleBar() {
  ImGui::BeginChild("title", ImVec2(0, 44 * g_scale), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::SetCursorPosY(6 * g_scale);
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.2f);
  ImGui::TextUnformatted("SvR2011 Mod Maker");
  ImGui::PopFont();
  ImGui::SameLine(0, 24 * g_scale);
  const bool dirty = ProjectDirty();
  if (ImGui::Button("New")) ImGui::OpenPopup("new_menu");
  NewMenu();
  ImGui::SameLine();
  if (ImGui::Button("Open...")) {
    const std::wstring f = PickFile(false, L"Open a project or a mod", kOpenFilter, 1);
    if (!f.empty()) ProjectOpen(f);
  }
  if (!g_settings.recent.empty()) {
    ImGui::SameLine(0, 2);
    if (ImGui::ArrowButton("recent", ImGuiDir_Down)) ImGui::OpenPopup("recent_menu");
    if (ImGui::BeginPopup("recent_menu")) {
      ImGui::TextDisabled("Recent:");
      for (const auto& r : std::vector<std::wstring>(g_settings.recent))
        if (ImGui::MenuItem(Utf8(r).c_str())) ProjectOpen(r);
      ImGui::EndPopup();
    }
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!Hooks(g_project_page));
  if (ImGui::Button(dirty ? "Save *" : "Save")) ProjectSave(false);
  ImGui::SameLine();
  if (ImGui::Button("Save as...")) ProjectSave(true);
  ImGui::EndDisabled();
  ImGui::SameLine(0, 24 * g_scale);
  {
    PageHooks* h = Hooks(g_project_page);
    const std::string name = h && h->state ? Value(h->state(), "name") : "";
    ImGui::TextDisabled("%s project:", PageName(g_project_page));
    ImGui::SameLine();
    ImGui::TextUnformatted(g_project_file.empty() ? (name.empty() ? "(unsaved)" : name.c_str())
                                                  : FileName(g_project_file).c_str());
    if (!g_project_file.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Utf8(g_project_file).c_str());
  }
  // right: the text size and the game folder
  const std::string game = g_game.empty() ? "No game folder" : Utf8(fs::path(g_game).filename().wstring());
  const float w = ImGui::CalcTextSize(game.c_str()).x + 40 * g_scale;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - w - 60 * g_scale));
  if (ImGui::Button("Aa")) ImGui::OpenPopup("text_size");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Text size (applies the next time the Mod Maker starts).");
  if (ImGui::BeginPopup("text_size")) {
    const int sizes[] = {14, 15, 17, 19, 22, 26};
    for (int sz : sizes) {
      char label[32];
      std::snprintf(label, sizeof label, "%d px%s", sz, sz == 17 ? "  (normal)" : "");
      if (ImGui::MenuItem(label, nullptr, g_settings.font == sz)) {
        g_settings.font = sz;
        SaveSettings();
        Status("Text size saved: it applies the next time the Mod Maker starts.");
      }
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  if (g_game.empty()) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
  if (ImGui::Button(game.c_str())) PickGameFolder();
  if (g_game.empty()) ImGui::PopStyleColor();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", g_game.empty() ? "Choose the folder where the game is installed."
                                            : ("The game folder the arenas, superstars and renders are read from:\n" +
                                               Utf8(g_game) + "\nClick to change it.")
                                                  .c_str());
  ImGui::EndChild();
}

void Rail(float height) {
  ImGui::BeginChild("rail", ImVec2(170 * g_scale, height), true);
  const char* last = nullptr;
  for (const auto& d : kPages) {
    if (d.section[0] && (!last || std::strcmp(last, d.section))) {
      if (last) ImGui::Spacing();
      ImGui::TextDisabled("%s", Upper(d.section).c_str());
      last = d.section;
    }
    const bool on = g_page == d.id;
    const bool project_type = d.hooks && d.id == g_project_page;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    const bool editor_off = d.id == PageId::kEditor && !editor::HasArena();
    ImGui::BeginDisabled(editor_off);
    if (ImGui::Button(d.name, ImVec2(-1, 0))) g_page = d.id;
    ImGui::EndDisabled();
    if (editor_off && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("Open an arena or a backstage area in 3D first (Arena or Backstage page).");
    if (project_type && !on) {  // (a dot: the current project's type)
      const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(b.x - 10 * g_scale, (a.y + b.y) / 2), 3 * g_scale,
                                                  IM_COL32(230, 25, 50, 255));
    }
    if (on) ImGui::PopStyleColor();
  }
  ImGui::EndChild();
}

void StatusBar() {
  ImGui::BeginChild("status", ImVec2(0, 0), false, ImGuiWindowFlags_NoScrollbar);
  std::string status, progress;
  {
    std::lock_guard lock(g_log_mutex);
    status = g_status, progress = g_progress;
  }
  if (Busy()) {
    const char spin[] = "|/-\\";
    ImGui::TextColored(kWarn, "%c  %s", spin[int(ImGui::GetTime() * 8) & 3], progress.empty() ? "Working..." : progress.c_str());
  } else {
    ImGui::TextUnformatted(status.c_str());
  }
  // right: problems and the log
  const int errors = ErrorCount(g_problems), warnings = int(g_problems.size()) - errors;
  char pb[64];
  if (g_problems.empty()) std::snprintf(pb, sizeof pb, "No problems");
  else std::snprintf(pb, sizeof pb, "%d error%s, %d warning%s", errors, errors == 1 ? "" : "s", warnings, warnings == 1 ? "" : "s");
  const float w = ImGui::CalcTextSize(pb).x + ImGui::CalcTextSize("Log").x + 60 * g_scale;
  ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - w));
  if (!g_problems.empty()) ImGui::PushStyleColor(ImGuiCol_Text, errors ? kBad : kWarn);
  if (ImGui::SmallButton(pb)) g_show_problems = !g_show_problems, g_settings.log_open = false;
  if (!g_problems.empty()) ImGui::PopStyleColor();
  ImGui::SameLine();
  if (ImGui::SmallButton(g_settings.log_open ? "Log v" : "Log ^")) {
    g_settings.log_open = !g_settings.log_open;
    g_show_problems = false;
    SaveSettings();
  }
  ImGui::EndChild();
}

void LogPanel(float height) {
  ImGui::BeginChild("log", ImVec2(0, height), true);
  if (g_show_problems) {
    ImGui::TextDisabled("Problems on the %s page:", PageName(g_project_page));
    if (g_problems.empty()) ImGui::TextUnformatted("None.");
    DrawProblems(g_problems);
  } else {
    std::lock_guard lock(g_log_mutex);
    for (const auto& l : g_log) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();
}

void Draw() {
  {  // work handed to the UI thread
    std::vector<std::function<void()>> q;
    {
      std::lock_guard lock(g_ui_mutex);
      q.swap(g_ui_queue);
    }
    for (auto& fn : q) fn();
  }
  arena_page::Tick();
  FollowPage();
  g_problems.clear();
  if (PageHooks* h = Hooks(g_project_page); h && h->problems) h->problems(g_problems);

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::Begin("Mod Maker", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
  TitleBar();
  const float status_h = 26 * g_scale;
  const float log_h = (g_settings.log_open || g_show_problems) ? 140 * g_scale : 0;
  const float body_h = -(status_h + log_h + (log_h ? ImGui::GetStyle().ItemSpacing.y : 0));
  Rail(body_h);
  ImGui::SameLine();
  const PageDesc& d = Desc(g_page);
  ImGui::BeginChild("page", ImVec2(0, body_h), d.id != PageId::kEditor,
                    d.id == PageId::kEditor ? ImGuiWindowFlags_None : ImGuiWindowFlags_None);
  if (d.draw) d.draw();
  else if (d.hooks && d.hooks->draw) d.hooks->draw();
  ImGui::EndChild();
  if (log_h) LogPanel(log_h);
  StatusBar();
  ImGui::End();
  // Ctrl+S saves the project
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S) && Hooks(g_project_page)) ProjectSave(false);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) {
    const std::wstring f = PickFile(false, L"Open a project or a mod", kOpenFilter, 1);
    if (!f.empty()) ProjectOpen(f);
  }
}

bool g_quit_asked = false;

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
    case WM_CLOSE:
      if (ProjectDirty() && !g_quit_asked && IsWindowVisible(w)) {
        const int r = MessageBoxW(w, L"Save the project before closing?", L"SvR2011 Mod Maker",
                                  MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        if (r == IDYES && !ProjectSave(false)) return 0;
      }
      DestroyWindow(w);
      return 0;
    case WM_DESTROY: {
      WINDOWPLACEMENT pl = {sizeof pl};
      if (!(GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_NOACTIVATE) && GetWindowPlacement(w, &pl)) {
        g_settings.win_x = pl.rcNormalPosition.left;
        g_settings.win_y = pl.rcNormalPosition.top;
        g_settings.win_w = pl.rcNormalPosition.right - pl.rcNormalPosition.left;
        g_settings.win_h = pl.rcNormalPosition.bottom - pl.rcNormalPosition.top;
        g_settings.maximized = pl.showCmd == SW_SHOWMAXIMIZED;
        SaveSettings();
      }
      PostQuitMessage(0);
      return 0;
    }
  }
  return DefWindowProcW(w, m, wp, lp);
}

}  // namespace
}  // namespace mm

using namespace mm;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  LoadSettings();
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  // test aids: --editor <arena tile 0-19> opens the editor on it, --open <mod> opens a mod,
  // --editor-view <0-2> a camera preset, --select <name> an object; --page <n>; the
  // superstar / signs / media pages' --star*, --sign, --media-* and --test-*-save
  int start_editor = -1, start_view = -1, start_new = -1, start_page = -1, start_backstage = -1, test_lib = -1;
  std::wstring start_mod, start_select, test_save, project, test_project, moves_pack, page_name;
  bool behind = false;  // --behind: tests - the window is never shown at all (frames still run)
  std::vector<std::wstring> run_tool_args;
  std::wstring make_bink_in, make_bink_out, moves_save;
  std::wstring shot;    // --shot <png> [--shot-after <s>]: the frame saved from the back buffer, then quit
  double shot_after = 6;
  int star_id = 0, star_call = -1;
  std::wstring star_model, star_song, star_movie, star_picture, star_voice, star_save, sign_save, media_save, media_video;
  std::string star_name;
  std::vector<std::wstring> sign_files, media_pictures;
  int media_arena = -1;
  for (int i = 1; i < argc; ++i) {
    const bool more = i + 1 < argc;
    if (!wcscmp(argv[i], L"--game") && more) g_game = argv[++i];
    else if (!wcscmp(argv[i], L"--editor") && more) start_editor = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--editor-view") && more) start_view = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--open") && more) start_mod = argv[++i];
    else if (!wcscmp(argv[i], L"--project") && more) project = argv[++i];
    else if (!wcscmp(argv[i], L"--behind")) behind = true, g_behind = true;
    else if (!wcscmp(argv[i], L"--test-play") && more) {  // <star id>,<dummy id>,<move id>: Play in game
      int a[3] = {0, 0, 0};
      swscanf_s(argv[++i], L"%d,%d,%d", &a[0], &a[1], &a[2]);
      anims_page::TestPlay(a[0], a[1], a[2]);
      g_page = PageId::kAnims;
    }
    else if (!wcscmp(argv[i], L"--shot") && more) shot = argv[++i];
    else if (!wcscmp(argv[i], L"--shot-after") && more) shot_after = _wtof(argv[++i]);
    else if (!wcscmp(argv[i], L"--test-project") && more) test_project = argv[++i];
    else if (!wcscmp(argv[i], L"--moves-pack") && more) moves_pack = argv[++i];
    else if (!wcscmp(argv[i], L"--test-moves-save") && more) moves_save = argv[++i];
    else if (!wcscmp(argv[i], L"--caw") && more) {  // <index>[,<attire>,<picture>[,save]]
      std::wstring a = argv[++i];
      std::vector<std::wstring> parts;
      for (size_t at = 0; at <= a.size();) {
        size_t e = a.find(L',', at);
        if (e == std::wstring::npos) e = a.size();
        parts.push_back(a.substr(at, e - at));
        at = e + 1;
      }
      caw_page::TestStart(_wtoi(parts[0].c_str()), parts.size() > 1 ? _wtoi(parts[1].c_str()) : 0,
                          parts.size() > 2 ? parts[2] : L"", parts.size() > 3 && parts[3] == L"save");
      g_page = PageId::kCaw;
    }
    else if (!wcscmp(argv[i], L"--convert-w13") && more) {  // <wwe13 pac>,<host tile>
      const std::wstring a = argv[++i];
      const size_t c = a.rfind(L',');
      other_page::TestConvertW13(a.substr(0, c), c == std::wstring::npos ? 1 : _wtoi(a.c_str() + c + 1));
    }
    else if (!wcscmp(argv[i], L"--make-bink") && more) {  // <video or picture>,<out.bik>
      const std::wstring a = argv[++i];
      const size_t c = a.rfind(L',');
      if (c != std::wstring::npos) make_bink_in = a.substr(0, c), make_bink_out = a.substr(c + 1);
    }
    else if (!wcscmp(argv[i], L"--run-tool") && more) {  // <script>,<arg>,<arg>...: the Python tool, output in the log
      std::vector<std::wstring> parts;
      const std::wstring a = argv[++i];
      for (size_t at = 0; at <= a.size();) {
        size_t e = a.find(L',', at);
        if (e == std::wstring::npos) e = a.size();
        parts.push_back(a.substr(at, e - at));
        at = e + 1;
      }
      run_tool_args = parts;
      g_page = PageId::kOther;
    }
    else if (!wcscmp(argv[i], L"--match-tab") && more) {  // <tab>[,<rule>]
      int a[2] = {0, -1};
      swscanf_s(argv[++i], L"%d,%d", &a[0], &a[1]);
      match_page::TestTab(a[0], a[1]);
      g_page = PageId::kMatch;
    }
    else if (!wcscmp(argv[i], L"--assets") && more) assets_page::TestOpen(Utf8(argv[++i])), g_page = PageId::kAssets;
    else if (!wcscmp(argv[i], L"--icons") && more) icons_page::TestTab(_wtoi(argv[++i])), g_page = PageId::kIcons;
    else if (!wcscmp(argv[i], L"--anims") && more) {  // <star id>,<bank index>,<motion id>[,<dummy id>]
      int a[4] = {0, -1, -1, 0};
      swscanf_s(argv[++i], L"%d,%d,%d,%d", &a[0], &a[1], &a[2], &a[3]);
      anims_page::TestStart(a[0], a[1], a[2], a[3]);
      g_page = PageId::kAnims;
    }
    else if (!wcscmp(argv[i], L"--page-name") && more) page_name = argv[++i];  // arena editor backstage superstar moves signs media assets anims icons help
    else if (!wcscmp(argv[i], L"--select") && more) start_select = argv[++i];
    else if (!wcscmp(argv[i], L"--test-edit-save") && more) test_save = argv[++i];
    else if (!wcscmp(argv[i], L"--new-arena") && more) start_new = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--test-lib") && more) test_lib = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--page") && more) start_page = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--backstage") && more) start_backstage = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--star") && more) star_id = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--star-song") && more) star_song = argv[++i];
    else if (!wcscmp(argv[i], L"--star-movie") && more) star_movie = argv[++i];
    else if (!wcscmp(argv[i], L"--star-picture") && more) star_picture = argv[++i];
    else if (!wcscmp(argv[i], L"--star-call") && more) star_call = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--star-voice") && more) star_voice = argv[++i];
    else if (!wcscmp(argv[i], L"--star-model") && more) star_model = argv[++i];
    else if (!wcscmp(argv[i], L"--star-name") && more) star_name = Utf8(argv[++i]);
    else if (!wcscmp(argv[i], L"--sign") && more) sign_files.push_back(argv[++i]);
    else if (!wcscmp(argv[i], L"--test-sign-save") && more) sign_save = argv[++i];
    else if (!wcscmp(argv[i], L"--media-arena") && more) media_arena = _wtoi(argv[++i]);
    else if (!wcscmp(argv[i], L"--media-video") && more) media_video = argv[++i];
    else if (!wcscmp(argv[i], L"--media-picture") && more) media_pictures.push_back(argv[++i]);
    else if (!wcscmp(argv[i], L"--test-media-save") && more) media_save = argv[++i];
    else if (!wcscmp(argv[i], L"--test-star-save") && more) star_save = argv[++i];
    else if (argv[i][0] != L'-') project = argv[i];  // a project or mod file (double-clicked)
  }
  // The old numbering of --page (0 arenas, 1 editor, 2 VS screen, 3 superstars, 4 signs, 5-7 media).
  const PageId page_of[] = {PageId::kArena, PageId::kEditor, PageId::kArena, PageId::kStar,
                            PageId::kSigns, PageId::kMedia,  PageId::kMedia, PageId::kMedia};
  if (g_game.empty()) g_game = g_settings.game;
  if (!g_game.empty() && !fs::exists(fs::path(g_game) / L"pac" / L"bg")) g_game.clear();

  WNDCLASSEXW wc = {sizeof wc, CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)), nullptr,
                    nullptr, nullptr, L"SvR2011ModMaker", nullptr};
  RegisterClassExW(&wc);
  const UINT dpi = GetDpiForSystem();
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = MulDiv(1360, dpi, 96), h = MulDiv(860, dpi, 96);
  if (g_settings.win_w > 400 && g_settings.win_h > 300) {
    x = g_settings.win_x, y = g_settings.win_y, w = g_settings.win_w, h = g_settings.win_h;
    // (still on a screen?)
    const RECT r = {x, y, x + w, y + h};
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) x = y = CW_USEDEFAULT;
  }
  if (behind) x = 0, y = 0, w = MulDiv(1280, dpi, 96), h = MulDiv(800, dpi, 96);
  g_wnd = CreateWindowExW(behind ? WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW : 0, wc.lpszClassName, L"SvR2011 Mod Maker",
                          WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
  if (!CreateDevice(g_wnd)) {
    if (!behind) MessageBoxW(nullptr, L"Direct3D 11 is not available.", L"SvR2011 Mod Maker", MB_ICONERROR);
    return 1;
  }
  if (behind) {
    // never shown: nothing on any screen, nothing on the taskbar (the swap chain still renders)
  } else {
    ShowWindow(g_wnd, g_settings.maximized && show == SW_SHOWNORMAL ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(g_wnd);
  }
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  {
    wchar_t fonts[MAX_PATH];
    GetWindowsDirectoryW(fonts, MAX_PATH);
    const std::string seg = Utf8(std::wstring(fonts) + L"\\Fonts\\segoeui.ttf");
    if (fs::exists(seg)) io.Fonts->AddFontFromFileTTF(seg.c_str(), g_settings.font * dpi / 96.0f);
  }
  g_scale = g_settings.font * dpi / 96.0f / 17.0f;
  Style();
  ImGui::GetStyle().ScaleAllSizes(g_scale);
  ImGui_ImplWin32_Init(g_wnd);
  ImGui_ImplDX11_Init(g_dev, g_ctx);
  if (g_game.empty()) {
    Status("Choose the folder where the game is installed (it has the pac folder).");
    g_game = PickFolder(L"The installed game folder (with pac\\bg)");
    if (!g_game.empty() && !fs::exists(fs::path(g_game) / L"pac" / L"bg")) g_game.clear();
    if (!g_game.empty()) g_settings.game = g_game, SaveSettings();
  }
  if (!g_game.empty() && LoadBanners()) Status("Game folder: " + Utf8(g_game));
  else if (!g_game.empty()) Status("The game folder's menu files could not be read: " + Utf8(g_game));
  else Status("No game folder: nothing can be read. Click the game folder button at the top right.");
  editor::Hooks hooks;
  hooks.log = [](const std::string& s) { Log(s); };
  hooks.test_in_game = [] { arena_page::TestInGame(); };
  hooks.pick_picture = [](const char* title) { return Utf8(PickFile(false, Wide(title).c_str(), kPictureFilter, 1)); };
  if (!g_game.empty())
    for (int i = 0; i < 20; ++i) hooks.library.push_back({g_arenas[i].name, ArenaPath(i)});
  editor::Init(g_dev, g_ctx, hooks);
  char_preview::Init(g_dev, g_ctx);
  ProjectReset();

  // the test aids
  star_page::TestStart(star_id, star_model, star_name, star_picture, star_save);
  if (star_call >= 0 || !star_song.empty() || !star_movie.empty() || !star_voice.empty())
    star_page::TestFiles(star_song, star_movie, star_voice, star_call);
  signs_page::TestStart(sign_files, sign_save);
  media_page::TestStart(media_arena, media_video, media_pictures, media_save);
  arena_page::SetTestSave(test_save, test_lib);
  if (start_new >= 0 && start_new < 20) arena_page::StartNew(start_new);
  if (start_editor >= 0 && start_editor < 20) arena_page::OpenEditor(start_editor);
  editor::TestStart(start_view, Utf8(start_select));  // (before the arena is set)
  if (start_page >= 0 && start_page < 8 && start_mod.empty()) g_page = page_of[start_page];
  if (start_backstage >= 0 && start_backstage < 7) arena_page::StartBackstage(start_backstage);
  if (!start_mod.empty()) {
    arena_page::OpenModFile(start_mod);
    if (start_page >= 0 && start_page < 8) g_page = page_of[start_page];
  }
  if (!moves_pack.empty()) moves_page::TestOpen(moves_pack), g_page = PageId::kMoves;
  if (!moves_save.empty()) {
    moves_page::TestSave(moves_save);
    WriteLogTo(moves_save + L".log");
    PostMessageW(g_wnd, WM_CLOSE, 0, 0);
  }
  if (!make_bink_in.empty()) MakeBink(make_bink_in, make_bink_out, 0, 3, [](std::wstring out) { Status(out.empty() ? "test: bink failed" : "test: bink made"); });
  if (!run_tool_args.empty()) {
    const std::wstring script = run_tool_args[0];
    run_tool_args.erase(run_tool_args.begin());
    RunTool(script, run_tool_args, [](int code) { Status("test: tool exit code " + std::to_string(code)); });
  }
  if (!page_name.empty()) {
    const wchar_t* names[] = {L"arena", L"editor", L"backstage", L"superstar", L"moves", L"signs", L"media", L"match", L"other", L"caw", L"assets", L"anims", L"icons", L"help"};
    for (int k = 0; k < int(PageId::kCount); ++k)
      if (page_name == names[k]) g_page = PageId(k);
  }
  if (!project.empty()) ProjectOpen(project);
  // test aid: --test-project <file.svrproj>: the shown page saved as a project, everything
  // reset, the project opened again; its state text goes to <file>.txt, the log to <file>.log
  bool test_project_pending = !test_project.empty();

  bool done = false;
  while (!done) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;
    if (IsIconic(g_wnd)) Sleep(30);  // (minimized: slow down; frames still run for the test aids)
    if (behind && IsWindowVisible(g_wnd)) ShowWindow(g_wnd, SW_HIDE);  // (stays unseen)
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    Draw();
    ImGui::Render();
    if (test_project_pending && !Busy() && ImGui::GetFrameCount() > 3) {
      test_project_pending = false;
      FollowPage();
      const std::string before = CurrentState(g_project_page);
      g_project_file = test_project;
      g_project_files[g_project_page] = test_project;
      const bool saved = ProjectSave(false);
      ProjectReset();
      const bool opened = saved && ProjectOpen(test_project);
      const std::string after = CurrentState(g_project_page);
      const std::string report = std::string("saved=") + (saved ? "1" : "0") + "\nopened=" + (opened ? "1" : "0") +
                                 "\nsame=" + (before == after ? "1" : "0") + "\n--- before\n" + before +
                                 "\n--- after\n" + after + "\n";
      WriteFile(Utf8(test_project + L".txt"), Bytes(report.begin(), report.end()));
      WriteLogTo(test_project + L".log");
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
    const float clear[4] = {0.086f, 0.090f, 0.106f, 1};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (!shot.empty() && ImGui::GetTime() >= shot_after && !Busy()) {  // the frame to a PNG, then quit
      ID3D11Texture2D* back = nullptr;
      g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
      D3D11_TEXTURE2D_DESC td;
      back->GetDesc(&td);
      td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ, td.MiscFlags = 0;
      ID3D11Texture2D* staging = nullptr;
      if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &staging))) {
        g_ctx->CopyResource(staging, back);
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(g_ctx->Map(staging, 0, D3D11_MAP_READ, 0, &ms))) {
          Image img;
          img.w = int(td.Width), img.h = int(td.Height);
          img.rgba.resize(size_t(img.w) * img.h * 4);
          for (int yy = 0; yy < img.h; ++yy) {
            std::memcpy(&img.rgba[size_t(yy) * img.w * 4], static_cast<const uint8_t*>(ms.pData) + size_t(yy) * ms.RowPitch, size_t(img.w) * 4);
            for (int xx = 0; xx < img.w; ++xx) img.rgba[(size_t(yy) * img.w + xx) * 4 + 3] = 255;
          }
          g_ctx->Unmap(staging, 0);
          SavePng(Utf8(shot), img);
        }
        staging->Release();
      }
      back->Release();
      WriteLogTo(shot + L".log");
      shot.clear();
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
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
