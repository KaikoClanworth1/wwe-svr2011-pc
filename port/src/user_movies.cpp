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
#include <cstdio>
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
constexpr uint32_t kSubListRow = 254;  // the movie list row that opens the sub-list
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
void Wr32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

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

// A guest string that lives for the whole run (menu rows keep the pointer).
uint32_t Text(const std::string& label) {
  if (auto it = g_labels.find(label); it != g_labels.end()) return it->second;
  const uint32_t s = g_memory->SystemHeapAlloc(uint32_t(label.size() + 1));
  if (!s) return 0;
  std::memcpy(g_memory->TranslateVirtual<char*>(s), label.c_str(), label.size() + 1);
  g_labels[label] = s;
  return s;
}

// The menu label of a movie: its file name without .bik, upper case.
uint32_t Label(const std::string& file) {
  std::string label = std::filesystem::path(file).stem().string();
  std::transform(label.begin(), label.end(), label.begin(), ::toupper);
  if (label.size() > 40) label.resize(40);
  return Text(label);
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

// CREATE AN ENTRANCE -> FINALIZE -> MOVIE. Its first row (value 254,
// HIGHLIGHT REEL) opens a sub-list: the list right after the movie list,
// which sub_8287CFB0 fills with the 20 highlight reel slots; picking one
// keeps 254 in the movie list and the pick in the sub-list (like MUSIC ->
// USER PLAYLIST). That row becomes USER MOVIES and its sub-list holds the
// user movies, then the saved highlight reels. The entrance then plays the
// picked id (sub_828B52D8 below).

// The movie list (sub_8287DFD0(list, r4, table, category, base)): the 254
// row is named USER MOVIES.
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
  const uint32_t count = Rd32(base, list), rows_at = Rd32(base, list + 8);
  // Tools: SVR2011_DUMP_MOVIE_NAMES=<file> writes "<id><TAB><name>" for the
  // game's movies (the launcher's list of superstars' movies).
  char* dump = nullptr;
  size_t dump_len = 0;
  if (_dupenv_s(&dump, &dump_len, "SVR2011_DUMP_MOVIE_NAMES") == 0 && dump) {
    if (FILE* f = std::fopen(dump, "w")) {
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t row = rows_at + i * kListItemSize, s = Rd32(base, row + 12);
        if (s) std::fprintf(f, "%d\t%s\n", int(Rd32(base, row)), reinterpret_cast<const char*>(base + s));
      }
      std::fclose(f);
    }
    free(dump);
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t row = rows_at + i * kListItemSize;
    if (Rd32(base, row) != kSubListRow) continue;
    std::lock_guard lock(g_mutex);
    if (const uint32_t s = Text("USER MOVIES")) Wr32(base, row + 12, s);  // {value, flags, unlock, text}
    break;
  }
}

// The sub-list: sub_8287CFB0(reels, list) adds the highlight reel slots
// (1000 + i, empty ones disabled) and returns how many reels are saved (the
// 254 row is disabled without any). Here: the user movies first, then the
// saved reels (empty slots dropped); the count includes the movies.
REX_EXTERN(__imp__sub_8287CFB0);
REX_HOOK_RAW(sub_8287CFB0) {
  const uint32_t list = ctx.r4.u32;
  __imp__sub_8287CFB0(ctx, base);
  if (!g_memory) return;
  const uint32_t reels = ctx.r3.u32;
  std::vector<std::pair<int, uint32_t>> rows;
  {
    std::lock_guard lock(g_mutex);
    Refresh();
    for (const auto& [id, file] : g_ids) {
      if (const uint32_t s = Label(file)) rows.emplace_back(id, s);
    }
  }
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
  // {count, capacity, rows}: [movies][saved reels].
  const uint32_t count = Rd32(base, list), rows_at = Rd32(base, list + 8);
  const size_t n = std::min<size_t>(rows.size(), count);
  std::vector<uint8_t> out;
  const uint8_t* all = base + rows_at;
  out.insert(out.end(), all + (count - n) * kListItemSize, all + count * kListItemSize);
  for (uint32_t i = 0; i < count - n; ++i) {
    const uint8_t* row = all + i * kListItemSize;
    if (Rd32(base, rows_at + i * kListItemSize + 4) == 0) out.insert(out.end(), row, row + kListItemSize);
  }
  std::memcpy(base + rows_at, out.data(), out.size());
  Wr32(base, list, uint32_t(out.size() / kListItemSize));
  ctx.r3.u64 = reels + uint32_t(n);
}

// The path of entrance movie `id`: sub_828B52D8(id) -> (r3) the path.
REX_EXTERN(__imp__sub_828B52D8);
REX_HOOK_RAW(sub_828B52D8) {
  const int id = ctx.r3.s32;
  __imp__sub_828B52D8(ctx, base);
  // Tests: SVR2011_TEST_MOVIE=<file in the movies folder> plays it for every
  // entrance (e.g. a test pattern, to see what the titantron shows).
  static const std::string test_movie = [] {
    char* v = nullptr;
    size_t n = 0;
    std::string s;
    if (_dupenv_s(&v, &n, "SVR2011_TEST_MOVIE") == 0 && v) {
      s = v;
      free(v);
    }
    return s;
  }();
  if (!test_movie.empty() && g_memory) {
    const std::string path = "umovie:\\" + test_movie;
    std::memcpy(base + ctx.r3.u32, path.c_str(), path.size() + 1);
    return;
  }
  if (id < kFirstId || id > kLastId || !g_memory) return;
  std::string file;
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_ids.find(id); it != g_ids.end()) file = it->second;
  }
  const std::string path = file.empty() ? "GAME:\\movies\\titantron\\999.bik" : "umovie:\\" + file;
  if (path.size() < 250) std::memcpy(base + ctx.r3.u32, path.c_str(), path.size() + 1);
}

// The layout of entrance movie `id`: sub_828B5250(id) reads a byte table of
// the game's movies (ids 0-999); the arena shows the movie's top on the big
// screen and its bottom strip on the stage when it is 1 (sub_8275FD20), else
// the whole frame on the big screen and the arena's own movie below. User
// movies aren't in the table (0): they get the superstar movies' layout.
REX_EXTERN(__imp__sub_828B5250);
REX_HOOK_RAW(sub_828B5250) {
  const int id = ctx.r3.s32;
  __imp__sub_828B5250(ctx, base);
  static bool logged = false;
  if (!logged && g_memory) {  // (once: the values the game's movies have)
    logged = true;
    std::map<uint32_t, int> counts;
    const uint32_t table = Rd32(base, 0x82EDE960);
    if (table) {
      for (int i = 0; i < 1000; ++i) counts[base[table + i]]++;
      std::string s;
      for (const auto& [v, n] : counts) s += fmt::format(" {}x{}", v, n);
      REXLOG_INFO("user movies: movie layout table values:{} (id {} -> {})", s, id, ctx.r3.u32);
    }
  }
  if (id >= kFirstId && id <= kLastId) ctx.r3.u64 = 1;
  // Tests: SVR2011_TEST_LAYOUT=<value> for every movie.
  static const int test_layout = [] {
    char* v = nullptr;
    size_t n = 0;
    int r = -1;
    if (_dupenv_s(&v, &n, "SVR2011_TEST_LAYOUT") == 0 && v) {
      r = std::atoi(v);
      free(v);
    }
    return r;
  }();
  if (test_layout >= 0) ctx.r3.u64 = uint32_t(test_layout);
}
