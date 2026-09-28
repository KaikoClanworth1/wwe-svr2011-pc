// USER MOVIES - see user_movies.h.
//
// The game names entrance movies by number (movies\titantron\<id>.bik,
// sub_828B52D8(id) builds the path). The CREATE AN ENTRANCE movie list is
// category 0 of the list tables sub_8287DFD0 turns into menu rows (records
// {value = movie id, name string, unlock}); 254 is HIGHLIGHT REEL, 999 NONE.
// The game uses ids up to 518 and 901-999; user movies get 700-899, kept in
// Custom Movies\ids.txt ("<id><TAB><file>") so a saved entrance keeps its
// movie when other movies are added or removed.
#include "user_movies.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"

namespace {

constexpr int kFirstId = 700, kLastId = 899;
constexpr int kNoneId = 999;
constexpr uint32_t kMovieCategory = 0;
constexpr uint32_t kListItemSize = 24;  // {value, flags, unlock, text[12]}

rex::memory::Memory* g_memory = nullptr;
std::filesystem::path g_folder;
std::mutex g_mutex;
std::map<int, std::string> g_ids;             // id -> file name (ASCII)
std::map<std::string, uint32_t> g_labels;     // label -> guest string

uint32_t Rd32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
int16_t Rd16(uint8_t* base, uint32_t a) { return int16_t(base[a] << 8 | base[a + 1]); }

bool Ascii(const std::wstring& s) {
  return std::all_of(s.begin(), s.end(), [](wchar_t c) { return c >= 32 && c < 127; });
}

// Rescans the folder: ids for new .bik files, drops files that are gone.
void Refresh() {
  std::map<int, std::string> ids;
  {
    std::ifstream in(g_folder / "ids.txt");
    std::string line;
    while (std::getline(in, line)) {
      const size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      const int id = std::atoi(line.substr(0, tab).c_str());
      if (id >= kFirstId && id <= kLastId) ids[id] = line.substr(tab + 1);
    }
  }
  std::vector<std::string> files;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(g_folder, ec)) {
    if (!e.is_regular_file(ec)) continue;
    const auto p = e.path();
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    if (ext != L".bik") continue;
    if (!Ascii(p.filename().wstring())) {
      REXLOG_WARN("user movies: skipping {} (use plain letters in the name)", p.filename().string());
      continue;
    }
    files.push_back(p.filename().string());
  }
  for (auto it = ids.begin(); it != ids.end();) {
    it = std::find(files.begin(), files.end(), it->second) == files.end() ? ids.erase(it) : std::next(it);
  }
  std::sort(files.begin(), files.end());
  bool changed = false;
  for (const auto& f : files) {
    const bool known = std::any_of(ids.begin(), ids.end(), [&](const auto& kv) { return kv.second == f; });
    if (known) continue;
    for (int id = kFirstId; id <= kLastId; ++id) {
      if (!ids.count(id)) {
        ids[id] = f;
        changed = true;
        break;
      }
    }
  }
  if (changed || ids.size() != g_ids.size()) {
    std::ofstream out(g_folder / "ids.txt", std::ios::trunc);
    for (const auto& [id, f] : ids) out << id << '\t' << f << '\n';
  }
  g_ids = std::move(ids);
}

// The menu label of a movie (its file name without .bik, upper case) as a
// guest string that lives for the whole run (menu rows keep the pointer).
uint32_t Label(const std::string& file) {
  std::string label = std::filesystem::path(file).stem().string();
  std::transform(label.begin(), label.end(), label.begin(), ::toupper);
  if (label.size() > 40) label.resize(40);
  if (auto it = g_labels.find(label); it != g_labels.end()) return it->second;
  const uint32_t s = g_memory->SystemHeapAlloc(uint32_t(label.size() + 1));
  if (!s) return 0;
  std::memcpy(g_memory->TranslateVirtual<char*>(s), label.c_str(), label.size() + 1);
  g_labels[label] = s;
  return s;
}

}  // namespace

namespace svr2011 {

void InstallUserMovies(rex::memory::Memory* memory, const std::filesystem::path& folder) {
  g_memory = memory;
  g_folder = folder;
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  std::lock_guard lock(g_mutex);
  Refresh();
  REXLOG_INFO("user movies: {} in {}", g_ids.size(), folder.string());
}

}  // namespace svr2011

// Menu rows from a list table: sub_8287DFD0(list, r4, table, category, base).
// After the game's MOVIE rows, the user movies go in after NONE.
REX_EXTERN(__imp__sub_8287DFD0);
REX_HOOK_RAW(sub_8287DFD0) {
  const uint32_t list = ctx.r3.u32, table = ctx.r5.u32, category = ctx.r6.u32;
  bool movies = false;
  if (category == kMovieCategory && g_memory) {
    const uint32_t items = Rd32(base, table + (category + 30) * 4);
    movies = items && Rd16(base, table + (category + 96) * 2) > 1 && Rd16(base, items + 6) == kNoneId;
  }
  __imp__sub_8287DFD0(ctx, base);
  if (!movies) return;
  std::vector<std::pair<int, uint32_t>> rows;
  {
    std::lock_guard lock(g_mutex);
    Refresh();
    for (const auto& [id, file] : g_ids) {
      if (const uint32_t s = Label(file)) rows.emplace_back(id, s);
    }
  }
  if (rows.empty()) return;
  const auto saved = ctx;
  for (const auto& [id, label] : rows) {  // sub_828829D8(list, value, text, 0, 0, unlock)
    ctx.r3.u64 = list;
    ctx.r4.u64 = uint32_t(id);
    ctx.r5.u64 = label;
    ctx.r6.u64 = 0;
    ctx.r7.u64 = 0;
    ctx.r8.u64 = uint64_t(int64_t(-1));
    sub_828829D8(ctx, base);
  }
  ctx = saved;
  // Move them up after the NONE row: the list is {count, capacity, rows}.
  const uint32_t count = Rd32(base, list), rows_at = Rd32(base, list + 8);
  size_t n = rows.size(), none = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (Rd32(base, rows_at + i * kListItemSize) == uint32_t(kNoneId)) {
      none = i;
      break;
    }
  }
  if (count < n || none + 1 >= count - n) return;
  uint8_t* first = base + rows_at + (none + 1) * kListItemSize;
  uint8_t* added = base + rows_at + (count - n) * kListItemSize;
  std::vector<uint8_t> tmp(added, added + n * kListItemSize);
  std::memmove(first + n * kListItemSize, first, added - first);
  std::memcpy(first, tmp.data(), tmp.size());
}

// The path of entrance movie `id`: sub_828B52D8(id) -> (r3) the path.
REX_EXTERN(__imp__sub_828B52D8);
REX_HOOK_RAW(sub_828B52D8) {
  const int id = ctx.r3.s32;
  __imp__sub_828B52D8(ctx, base);
  if (id < kFirstId || id > kLastId || !g_memory) return;
  std::string file;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_ids.find(id); it != g_ids.end()) file = it->second;
  }
  const std::string path = file.empty() ? "GAME:\\movies\\titantron\\999.bik" : "umovie:\\" + file;
  if (path.size() < 250) std::memcpy(base + ctx.r3.u32, path.c_str(), path.size() + 1);
}
