// WWE SmackDown vs. Raw 2011 - app setup (paths and settings defaults).

#include "generated/default/svr2011_init.h"

#include "svr2011_app.h"

#include <cstdlib>
#include <fstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>

#include <imgui.h>

#include <rex/audio/sdl/sdl_audio_system.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>

#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>

#include "caw_logos.h"
#include "crash_report.h"
#include "dlc.h"
#include "fps_overlay.h"
#include "game_files.h"
#include "graphics_page.h"
#include "menu_hooks.h"
#include "music.h"
#include "saves.h"
#include "user_movies.h"
#include "native/native_renderer.h"
#include "frame_stats.h"
#include "keyboard_typing.h"
#include "platform.h"
#include "script_input.h"
#include "xaudio2_audio.h"

REXCVAR_DEFINE_STRING(audio_backend, "xaudio2", "Audio",
                      "Audio output: xaudio2 (default) or sdl")
    .allowed({"xaudio2", "sdl"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

// Written when svr2011.toml is missing: the settings a fresh install starts
// with. The launcher's Settings tab rewrites this file.
constexpr char kDefaultConfig[] =
    "# WWE SmackDown vs. Raw 2011 - settings (the launcher rewrites this file)\n"
    "gpu_plugin = \"xenos\"\n"
    "native_renderer = \"main\"\n"  // as the launcher's defaults (Emulated: "off")
    "input_backend = \"xinput\"\n"
    "resolution = \"720p\"\n"
    "resolution_scale = 1\n"
    "fullscreen = false\n"
    "vsync = true\n";

std::filesystem::path g_config_path;  // svr2011.toml (the GRAPHICS page saves there)

// Value of an environment variable, or "" when unset.
std::string Env(const char* name) {
  char* value = nullptr;
  size_t len = 0;
  std::string result;
  if (_dupenv_s(&value, &len, name) == 0 && value) {
    result = value;
    free(value);
  }
  return result;
}

// The runtime's user data (paths.user_data_root) and the saves folder.
std::filesystem::path g_user_data;
std::filesystem::path g_saves;

}  // namespace

void Svr2011App::OnConfigurePaths(rex::PathConfig& paths) {
  // 1 ms timer resolution. Without it Windows rounds every Sleep up to the
  // 15.6 ms default tick, and the runtime sleeps while the emulated GPU waits
  // on the game's fences (and for the game's own Sleep calls) - that alone
  // held the training ring to ~39 fps.
  timeBeginPeriod(1);

  // The game asks the music player (XMP) for its playback controller twice a
  // frame from its simulation thread; the runtime's anti-spin delay for that
  // call (10 ms each, meant for titles that poll it in a tight loop) held the
  // training ring to ~39 fps. Not a user setting, so it is forced here.
  rex::cvar::SetFlagByName("xmp_throttle", "false");

  const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();

  // The disc files live beside the program unless --game_data_root says
  // otherwise.
  if (paths.game_data_root.empty()) {
    paths.game_data_root = exe_dir;
  }
  paths.user_data_root = exe_dir / "UserData";
  // Tests: SVR2011_USER_DATA=<folder> keeps their saves away from the player's.
  if (std::string dir = Env("SVR2011_USER_DATA"); !dir.empty()) {
    paths.user_data_root = dir;
  }
  g_user_data = paths.user_data_root;
  // Saves: <game>\Saves (tests: beside their user data).
  g_saves = (Env("SVR2011_USER_DATA").empty() ? exe_dir : g_user_data) / "Saves";
  paths.cache_root = exe_dir / "UserData" / "cache";
  svr2011::InstallCrashReporter(exe_dir / "UserData" / "crashes");

  // Tests: SVR2011_CONFIG=<file> keeps settings changes away from the player's.
  if (std::string config = Env("SVR2011_CONFIG"); !config.empty()) {
    paths.config_path = config;
  }
  g_config_path = paths.config_path;
  if (!std::filesystem::exists(paths.config_path)) {
    std::string config = kDefaultConfig;
    if (svr2011::IsSteamDeck()) {  // its screen is the window
      config.replace(config.find("fullscreen = false"), 18, "fullscreen = true");
    }
    std::ofstream(paths.config_path) << config;
  }
}

void Svr2011App::OnPreSetup(rex::RuntimeConfig& config) {
  // Automated tests: the only controller is one driven by a command file.
  if (std::string file = Env("SVR2011_INPUT_FILE"); !file.empty()) {
    config.input_factory = [file](bool) -> std::unique_ptr<rex::system::IInputSystem> {
      auto input = std::make_unique<rex::input::InputSystem>(nullptr);
      input->AddDriver(std::make_unique<svr2011::ScriptInputDriver>(file));
      input->SetDeviceAssignment(std::make_unique<rex::input::SlotAssignment>());
      return input;
    };
  }

  if (REXCVAR_GET(audio_backend) == "sdl") {
    return;  // the SDK default
  }
  // XAudio2, falling back to SDL if it cannot start (no audio device, ...).
  config.audio_factory = [](rex::runtime::FunctionDispatcher* dispatcher)
      -> std::unique_ptr<rex::system::IAudioSystem> {
    if (svr2011::XAudio2AudioSystem::IsAvailable()) {
      return svr2011::XAudio2AudioSystem::Create(dispatcher);
    }
    REXLOG_WARN("XAudio2 unavailable, using SDL audio");
    return rex::audio::sdl::SDLAudioSystem::Create(dispatcher);
  };
}

void Svr2011App::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  SetGuestFrameStats(svr2011::GetFrameStats);  // F3 overlay
  svr2011::InstallFpsOverlay(drawer);          // F2 counter
  if (window()) {
    window()->SetTitle("WWE SmackDown vs. Raw 2011");
  }
}

// The GRAPHICS page's fonts (bold, like the game's menus); ImGui's own
// stays the default for the overlays.
void Svr2011App::OnConfigureFonts(ImFontAtlas* atlas) {
  const char* kBold = "C:\\Windows\\Fonts\\segoeuib.ttf";
  const char* kBoldItalic = "C:\\Windows\\Fonts\\segoeuiz.ttf";
  ImFont* menu = std::filesystem::exists(kBold) ? atlas->AddFontFromFileTTF(kBold, 30.0f) : nullptr;
  ImFont* title =
      std::filesystem::exists(kBoldItalic) ? atlas->AddFontFromFileTTF(kBoldItalic, 40.0f) : nullptr;
  svr2011::SetGraphicsPageFonts(menu, title ? title : menu);
}

void Svr2011App::OnPostLoadXexImage() {
  // Entrances, finishers and other post-processed scenes sample the frame's
  // resolved colour texture through fetch constants whose type field is 0
  // ("invalid"); the Xbox 360 samples them as textures, the emulator's default
  // treats them as unbound (black) - a black screen with only the 2D overlay.
  // (Set here: the flag lives in the GPU plugin, loaded after OnConfigurePaths;
  // it is read at every draw.)
  if (!rex::cvar::SetFlagByName("gpu_allow_invalid_fetch_constants", "true")) {
    REXLOG_WARN("could not enable gpu_allow_invalid_fetch_constants");
  }
#ifndef SVR2011_D3D_TRACE  // the census build has no native renderer
  svr2011::native::Attach(runtime()->memory());
  if (rex::ui::Window* w = window()) {
    svr2011::native::SetWindowSizeSource([w] {
      return std::pair<uint32_t, uint32_t>(w->GetActualPhysicalWidth(), w->GetActualPhysicalHeight());
    });
  }
#endif

  // The utility drive (cache:\): the game keeps created content there -
  // EditEx.cac (Create modes: superstars, paint tool, ...) and Spectacular.rps
  // (highlight reels); without it those are never written. The SDK leaves
  // cache: unmounted; mount it on a folder next to the saves. (Registered
  // before the game's own XMountUtilityDrive link, which then doesn't replace it.)
  // SVR2011_NO_UTILITY_DRIVE=1 leaves it unmounted (for comparing).
  if (Env("SVR2011_NO_UTILITY_DRIVE").empty()) {
    const std::filesystem::path dir = g_user_data / "UtilityDrive";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\UTILITY", dir, false);
    auto* fs = runtime()->file_system();
    if (device->Initialize() && fs->RegisterDevice(std::move(device))) {
      fs->RegisterSymbolicLink("cache:", "\\UTILITY");
    } else {
      REXLOG_WARN("could not mount the utility drive (cache:) at {}", dir.string());
    }
  }

  // The port's text and menu changes to the installed disc files (once).
  svr2011::PatchGameFiles(rex::filesystem::GetExecutableFolder());

  // Saves as plain files in the Saves folder (saves.h).
  svr2011::UseFlatSaves(runtime()->kernel_state(), g_user_data, g_saves);

  // DLC packages the launcher put in "DLC" (dlc.h).
  {
    const std::filesystem::path exe_dir = rex::filesystem::GetExecutableFolder();
    svr2011::InstallDlc(runtime()->kernel_state(), exe_dir / "DLC", exe_dir / "UserData");
  }

  // USER PLAYLIST entrance music from the Music folder (music.h); tests:
  // SVR2011_MUSIC=<folder>.
  {
    std::filesystem::path music = rex::filesystem::GetExecutableFolder() / "Music";
    if (std::string dir = Env("SVR2011_MUSIC"); !dir.empty()) music = dir;
    svr2011::InstallUserMusic(music);
  }

  // USER MOVIES entrance movies from the Custom Movies folder, mounted as
  // umovie: (user_movies.h); tests: SVR2011_MOVIES=<folder>.
  {
    std::filesystem::path movies = rex::filesystem::GetExecutableFolder() / "Custom Movies";
    if (std::string dir = Env("SVR2011_MOVIES"); !dir.empty()) movies = dir;
    std::error_code ec;
    std::filesystem::create_directories(movies, ec);
    auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\USERMOVIES", movies, true);
    auto* fs = runtime()->file_system();
    if (device->Initialize() && fs->RegisterDevice(std::move(device))) {
      fs->RegisterSymbolicLink("umovie:", "\\USERMOVIES");
      svr2011::InstallUserMovies(runtime()->memory(), movies);
    } else {
      REXLOG_WARN("could not mount the Custom Movies folder {}", movies.string());
    }
  }

  REXLOG_INFO("platform: {}", svr2011::PlatformDescription());

  // MY WWE -> OPTIONS -> GRAPHICS (menu_hooks.cpp, graphics_page.h).
  svr2011::InstallMenuHooks(runtime()->memory());
  svr2011::InstallCawLogos(runtime()->memory(), g_saves);
  // PC keyboard typing in the game's on-screen keyboard (keyboard_typing.h).
  svr2011::InstallKeyboardTyping(window());
  if (imgui_drawer()) {
    svr2011::InstallGraphicsPage(
        imgui_drawer(), window(),
        static_cast<rex::input::InputSystem*>(runtime()->input_system()), g_config_path);
  }

  // Developer aid: SVR2011_DUMP_IMAGE=<file> writes the loaded (decrypted,
  // decompressed) executable image for analysis tools (port/tools/).
  if (std::string dump = Env("SVR2011_DUMP_IMAGE"); !dump.empty()) {
    const uint8_t* image = runtime()->virtual_membase() + REX_IMAGE_BASE;
    std::ofstream(dump, std::ios::binary)
        .write(reinterpret_cast<const char*>(image), REX_IMAGE_SIZE);
  }
}
