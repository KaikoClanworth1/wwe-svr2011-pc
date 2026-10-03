// WWE SmackDown vs. Raw 2011 - DLC installation (see dlc.h).

#include "dlc.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

namespace svr2011 {

namespace {

constexpr uint32_t kMarketplaceContent = 0x00000002;

// An STFS package of downloadable content ("LIVE"/"PIRS"/"CON " magic,
// content type at 0x344) - title updates (0x000B0000) and others are skipped.
bool IsDlcPackage(const std::filesystem::path& path, uint32_t* title_id) {
  std::ifstream in(path, std::ios::binary);
  uint8_t head[0x364] = {};
  if (!in.read(reinterpret_cast<char*>(head), sizeof(head))) return false;
  const std::string magic(reinterpret_cast<char*>(head), 4);
  if (magic != "LIVE" && magic != "PIRS" && magic != "CON ") return false;
  auto be32 = [&](size_t o) {
    return uint32_t(head[o]) << 24 | uint32_t(head[o + 1]) << 16 | uint32_t(head[o + 2]) << 8 |
           head[o + 3];
  };
  *title_id = be32(0x360);
  return be32(0x344) == kMarketplaceContent;
}


// The DLC's own pac/string.pac replaces the game's text once it is loaded:
// rewritten as tools/patch_strings.py does the game's (keep the tables in
// sync). Strings only get shorter and are padded with nulls, so offsets
// stay valid. Idempotent.
const std::pair<std::string_view, std::string_view> kStringPatches[] = {
    {"Please don't turn off \nyour Xbox 360 console.", "Please don't close \nthe game."},
    {"Please don't turn off your Xbox 360 console\n", "Please don't close the game\n"},
    {"Please don't turn off your Xbox 360 console.", "Please don't close the game."},
    {"your Xbox 360 console.", "the game."},
    {"If you rip a music CD to the console, you can set your favorite music as background music.",
     "Put .mp3 files in the game's Music folder to use your own music as background music."},
    {"create a Playlist containing\nonly that song through the Xbox Dashboard.",
     "put only that song in its own\nfolder in the game's Music folder."},
    // Online Axxess: none needed on the PC (all content is licensed)
    {" This item is free with redemption of the single-use Online Axxess code found on the back of your game manual. If the code has already been redeemed by a previous owner, then you can purchase an Online Axxess code in the store.",
     ""},
    {"Check out the store to redeem your\nONLINE Axxess code, and purchase\ndownloadable content",
     "Check out the store for\ndownloadable content"},
    {"From now on everything besides\nONLINE AXXESS downloadable content is free!",
     "From now on all downloadable\ncontent is free!"},
    {" Please note: This purchase does not include Online Axxess or WWE Legend Bret Hart.",
     " Please note: This purchase does not include WWE Legend Bret Hart."},
    // (the same in the other languages)
    {"usa la Xbox Dashboard per creare una playlist contenente solo la canzone desiderata.",
     "mettila da sola in una cartella dentro la cartella Music del gioco."},
    {"erstelle \xC3\xBC""ber die Xbox Steuerung eine Wiedergabeliste, die nur diesen Song enth\xC3\xA4lt.",
     "lege nur diesen Song in einen eigenen Ordner im Music-Ordner des Spiels."},
    {"crea una lista de reproducci\xC3\xB3n que solo contenga esa canci\xC3\xB3n con el Interfaz Xbox.",
     "pon solo esa canci\xC3\xB3n en su propia carpeta dentro de la carpeta Music del juego."},
    {"cr\xC3\xA9""ez une s\xC3\xA9lection ne contenant que cette musique\nvia l'Interface Xbox.",
     "mettez-la seule dans un dossier\ndu dossier Music du jeu."},
    {"Xbox \xE3\x83\x80\xE3\x83\x83\xE3\x82\xB7\xE3\x83\xA5\xE3\x83\x9C\xE3\x83\xBC\xE3\x83\x89\xE3\x81\x8B\xE3\x82\x89\xE3\x80\x81\n\xE3\x81\x9D\xE3\x81\xAE\xE6\x9B\xB2\xE3\x81\xA0\xE3\x81\x91\xE3\x81\x8C\xE5\x85\xA5\xE3\x81\xA3\xE3\x81\x9FPlaylist\xE3\x82\x92\xE4\xBD\x9C\xE6\x88\x90\xE3\x81\x97\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xE3\x80\x82",
     "\xE3\x82\xB2\xE3\x83\xBC\xE3\x83\xA0\xE3\x81\xAEMusic\xE3\x83\x95\xE3\x82\xA9\xE3\x83\xAB\xE3\x83\x80\xE3\x81\xAB\xE3\x80\x81\n\xE3\x81\x9D\xE3\x81\xAE\xE6\x9B\xB2\xE3\x81\xA0\xE3\x81\x91\xE3\x81\xAE\xE3\x83\x95\xE3\x82\xA9\xE3\x83\xAB\xE3\x83\x80\xE3\x82\x92\xE4\xBD\x9C\xE3\x81\xA3\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xE3\x80\x82"},
    {"storage device", "save folder"},
    {"A maximum of 2 different High Resolution logos can be",
     "Up to 10 different High Resolution logos can be"},
    // Xbox LIVE features the PC doesn't have (English text; the button help
    // for gamer cards and parties goes by string id, online.cpp)
    {"\xEE\x80\x95VIEW GAMER CARD   ", ""},
    {"the Xbox LIVE Marketplace", "the SHOP"},
    {"the Online Marketplace", "the SHOP"},
    {"Xbox LIVEmenu", "Online menu"},
    {"Onlinemenu", "Online menu"},  // (patched before as one word)
    {"gamer profile", "profile"},
    {"Accessing Gamertag...", "Accessing name..."},
    {"customize your Gamertag display", "customize your name display"},
    {"CUSTOM SEARCH FROM GAMERTAG", "CUSTOM SEARCH FROM NAME"},
    {"play on Xbox LIVE", "play online"},
    {"on Xbox LIVE", "online"},
    {"Xbox LIVE", "Online"},
    {"deine\nXbox 360 Konsole", "deinen\nPC"},
    {"deine Xbox 360 Konsole", "deinen PC"},
    {"Xbox 360 Konsole", "PC"},
    {"tu Consola Xbox 360", "tu PC"},
    {"la Consola Xbox 360", "el PC"},
    {"Consola Xbox 360", "PC"},
    {"la console Xbox 360", "il PC"},
    {"console Xbox 360", "PC"},
    {"votre Console Xbox 360", "votre PC"},
    {"Console Xbox 360", "PC"},
    {"Xbox 360 \xE6\x9C\xAC\xE4\xBD\x93", "PC"},  // Japanese: the console unit
    {"Xbox 360", "PC"},
};
constexpr std::string_view kStringTriggers[] = {"Xbox LIVE", "Xbox 360", "storage device",
                                                "music CD", "Xbox Dashboard",
                                                "A maximum of 2 different High Resolution",
                                                "VIEW GAMER CARD   ", "Marketplace", "Onlinemenu",
                                                "gamer profile", "Gamertag", "GAMERTAG", "Xbox",
                                                "Online Axxess", "ONLINE Axxess", "ONLINE AXXESS"};

// -- Online Axxess ------------------------------------------------------------------
//
// THQ's online pass: the ONLINE_AXXESS item (type 6) of a DLC package's
// info/catalog.dlc ("DLCC" header, 64-byte entries: u16 flags, id, type ...
// and the name at +0x20). Without it the game asks Xbox LIVE's title storage
// for a trial period (XStorageBuildServerPath ...) when ONLINE is chosen and
// says "Information related to your trial period couldn't be retrieved". The
// codes can't be redeemed any more: with online play on, a package of the
// port's own grants it (that one item, nothing else) when no installed
// package has it - whichever region's DLC a player has, or none.

constexpr char kAxxessPackage[] = "5356523230313150434F4E4C494E45415858455353";  // (hex "SVR2011PCONLINEAXXESS")
constexpr uint16_t kAxxessType = 6;

bool HasAxxess(const std::filesystem::path& catalog) {
  std::ifstream in(catalog, std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (d.size() < 0x18 || std::string_view(reinterpret_cast<char*>(d.data()), 4) != "DLCC") return false;
  const uint16_t header = uint16_t(d[6] | d[7] << 8);
  const uint32_t count = uint32_t(d[12] | d[13] << 8 | d[14] << 16 | uint32_t(d[15]) << 24);
  for (uint32_t i = 0; i < count && header + (i + 1) * 0x40 <= d.size(); ++i) {
    const uint8_t* e = d.data() + header + i * 0x40;
    if (uint16_t(e[4] | e[5] << 8) == kAxxessType) return true;
  }
  return false;
}

void GrantOnlineAxxess(const std::filesystem::path& title_root, uint32_t title_id) {
  std::error_code ec;
  const auto installed = title_root / "00000002";
  const auto ours = installed / kAxxessPackage;
  const auto header = title_root / "Headers" / "00000002" / (std::string(kAxxessPackage) + ".header");
  if (!rex::cvar::Query<bool>("online_enabled")) return;
  for (const auto& pkg : std::filesystem::directory_iterator(installed, ec)) {
    if (pkg.path().filename() == kAxxessPackage) continue;
    if (HasAxxess(pkg.path() / "info" / "catalog.dlc")) return;  // (the player's own)
  }
  if (std::filesystem::exists(ours / "info" / "catalog.dlc", ec) && std::filesystem::exists(header, ec)) return;

  // info/catalog.dlc: one entry
  std::vector<uint8_t> cat(0x18 + 0x40, 0);
  std::memcpy(cat.data(), "DLCC", 4);
  cat[4] = 2;     // version
  cat[6] = 0x18;  // header size
  cat[8] = 1;
  cat[12] = 1;    // entries
  uint8_t* e = cat.data() + 0x18;
  e[0] = 0x40, e[2] = 1, e[4] = uint8_t(kAxxessType), e[6] = 1, e[16] = 5;
  std::memcpy(e + 0x20, "ONLINE_AXXESS", 13);
  // the content header (as the SDK keeps an installed package's): device 1,
  // type 2, UTF-16BE display name at 8, file name at 264, title id at 320
  std::vector<uint8_t> head(332, 0);
  head[3] = 1, head[7] = 2;
  const char* name = "Online Axxess (PC)";
  for (size_t i = 0; name[i]; ++i) head[8 + 2 * i + 1] = uint8_t(name[i]);
  std::memcpy(head.data() + 264, kAxxessPackage, sizeof(kAxxessPackage) - 1);
  for (int i = 0; i < 4; ++i) head[320 + i] = uint8_t(title_id >> (24 - 8 * i));
  head[324] = 0xFE, head[325] = 0x7F, head[328] = 0x3F;

  std::filesystem::create_directories(ours / "info", ec);
  std::filesystem::create_directories(header.parent_path(), ec);
  std::ofstream(ours / "info" / "catalog.dlc", std::ios::binary).write(reinterpret_cast<char*>(cat.data()), cat.size());
  std::ofstream(header, std::ios::binary).write(reinterpret_cast<char*>(head.data()), head.size());
  REXLOG_INFO("DLC: Online Axxess granted (no installed package had it)");
}

}  // namespace

void PatchOnlineStrings(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return;
  std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  int count = 0;
  const std::string_view all(data.data(), data.size());
  std::vector<size_t> done;
  for (std::string_view trigger : kStringTriggers) {
    for (size_t at = all.find(trigger); at != std::string_view::npos;) {
      size_t start = all.rfind('\0', at);
      start = start == std::string_view::npos ? 0 : start + 1;
      const size_t end = all.find('\0', at);
      if (end == std::string_view::npos) break;
      if (std::find(done.begin(), done.end(), start) == done.end()) {
        done.push_back(start);
        const std::string old(all.substr(start, end - start));
        std::string now = old;
        if (now == "Xbox LIVE") {
          now = "ONLINE";  // a menu label (upper case)
        } else if (now == "GAMERTAG") {
          now = "NAME";
        } else {
          for (const auto& [from, to] : kStringPatches) {
            for (size_t p = now.find(from); p != std::string::npos; p = now.find(from, p + to.size()))
              now.replace(p, from.size(), to);
          }
        }
        // (a string may grow into the nulls a shorter patch left after it)
        size_t room = end;
        while (room + 1 < all.size() && all[room + 1] == '\0') ++room;
        if (now != old && start + now.size() <= room) {
          std::copy(now.begin(), now.end(), data.begin() + start);
          std::fill(data.begin() + start + now.size(), data.begin() + std::max(end, start + now.size() + 1), '\0');
          ++count;
        }
      }
      at = all.find(trigger, end);
    }
  }
  if (!count) return;
  const auto tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, file, ec);
  if (!ec)
    REXLOG_INFO("{}: {} strings rewritten for the PC", file.string(), count);
}

void InstallDlc(rex::system::KernelState* kernel_state, const std::filesystem::path& dlc_dir,
                const std::filesystem::path& user_data_root) {
  std::error_code ec;
  if (!kernel_state) return;
  auto* content = kernel_state->content_manager();
  if (!content) return;
  const uint32_t game_title = kernel_state->title_id();
  // Installed packages: <user data>/0000000000000000/<title>/00000002/<name>/
  char title_dir[16];
  std::snprintf(title_dir, sizeof(title_dir), "%08X", game_title);
  const std::filesystem::path installed =
      user_data_root / "0000000000000000" / title_dir / "00000002";
  struct PatchStrings {
    const std::filesystem::path& dir;
    ~PatchStrings() {  // whatever happens below: the installed DLC's text
      std::error_code e;
      for (const auto& pkg : std::filesystem::directory_iterator(dir, e))
        PatchOnlineStrings(pkg.path() / "pac" / "string.pac");
    }
  } patch_strings{installed};
  struct Axxess {  // (whatever happens below: once the player's packages are in)
    std::filesystem::path root;
    uint32_t title;
    ~Axxess() { GrantOnlineAxxess(root, title); }
  } axxess{installed.parent_path(), game_title};
  if (!std::filesystem::is_directory(dlc_dir, ec)) return;

  for (const auto& e : std::filesystem::directory_iterator(dlc_dir, ec)) {
    if (!e.is_regular_file()) continue;
    uint32_t title = 0;
    if (!IsDlcPackage(e.path(), &title)) continue;
    const std::string name = e.path().filename().string();
    if (title != game_title) {
      REXLOG_WARN("DLC {}: for title {:08X}, not this game - skipped", name, title);
      continue;
    }
    if (std::filesystem::is_directory(installed / e.path().filename(), ec)) continue;
    REXLOG_INFO("DLC {}: installing", name);
    const auto result = content->InstallContent(e.path());
    if (result == 0) {
      REXLOG_INFO("DLC {}: installed", name);
    } else {
      REXLOG_ERROR("DLC {}: install failed ({:08X})", name, uint32_t(result));
      std::filesystem::remove_all(installed / e.path().filename(), ec);  // retry next start
    }
  }
}

}  // namespace svr2011
