// WWE SmackDown vs. Raw 2011 - platform detection (see platform.h).

#include "platform.h"

#include <cstdlib>

#include <windows.h>

namespace svr2011 {

namespace {

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

HMODULE Ntdll() { return GetModuleHandleW(L"ntdll.dll"); }

}  // namespace

bool IsWine() {
  static const bool wine = Ntdll() && GetProcAddress(Ntdll(), "wine_get_version");
  return wine;
}

bool IsSteamDeck() {
  static const bool deck = Env("SteamDeck") == "1";
  return deck;
}

std::string PlatformDescription() {
  std::string s;
  if (IsWine()) {
    using WineVersion = const char*(__cdecl*)();
    using HostVersion = void(__cdecl*)(const char** sysname, const char** release);
    const auto version = reinterpret_cast<WineVersion>(GetProcAddress(Ntdll(), "wine_get_version"));
    const auto host = reinterpret_cast<HostVersion>(GetProcAddress(Ntdll(), "wine_get_host_version"));
    s = std::string("Wine ") + (version ? version() : "?");
    if (host) {
      const char* sysname = nullptr;
      const char* release = nullptr;
      host(&sysname, &release);
      if (sysname) s += std::string(" on ") + sysname + (release ? std::string(" ") + release : "");
    }
    if (!Env("STEAM_COMPAT_DATA_PATH").empty()) s += " (Proton)";
  } else {
    using RtlGetVersion = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v = {sizeof v};
    const auto get = reinterpret_cast<RtlGetVersion>(GetProcAddress(Ntdll(), "RtlGetVersion"));
    if (get && get(&v) == 0) {
      s = "Windows " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." +
          std::to_string(v.dwBuildNumber);
    } else {
      s = "Windows";
    }
  }
  if (IsSteamDeck()) s += " (Steam Deck)";
  return s;
}

}  // namespace svr2011
