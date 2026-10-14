// Move packs (move_packs.h).
//
// A move is its motions plus a few tables (scratchpad research
// svr10/re_moveport.md):
// - motions: YMKs banks, keyed (id, x = phase/variant, y = track: 0 the
//   attacker, 1 the victim, others props/cameras), in m.pac (MOT/BM.., MOT/RR..
//   and MVMT/.. menu banks) and mpsp.pac MOTP/GAME (the banks a match loads
//   for every superstar). A bank: "YMKs", a 256-byte event-size table at 0x10,
//   u32 count at 0x110, then count x {u8 y, u8 x, u16 id, u32 offset, u32
//   frames, u32 runtime} sorted by key (id<<16 | x<<8 | y) and the motion data
//   in directory order (an entry's size is the next offset minus its own).
// - misc.pac MOVS/WAZE: 160-byte move records (id at +0x90); the first 16
//   bytes are the category bits that make a move selectable in a slot.
// - misc.pac WAZA/DATA children 0 (EXH: 36-byte per-key move data), 1 (per-key
//   sound events: 16-byte records + an event pool) and 2 (MBD: 8-byte records),
//   each a PACH of groups sorted by (u16 id, u8, u8); BATS/INIT and BATH/INIT
//   hold the same three as children 10, 11 and 12.
//
// pack.txt (written by tools/movepack.py), one item a line:
//   motion <m.pac|mpsp.pac> <TYPE/NAME/child/...> <id> <x> <y> <frames> <file> [<20 bytes hex>]
//     (YMBs banks - the ADPCM ones: taunts, submissions, finishers - also take
//     the motion's 20-byte header: type, segments, interval, 0, 4 floats)
//   waze <id> <16 bytes hex>
//   wazename <id> <name>   (the move record's name, e.g. a record 2011 keeps
//     without a motion, given one)
//   wazecopy <id> <from id>   (a new move on a free record: the whole record
//     of another move - category bits, parameters - but its own id; applied
//     before waze / wazename lines)
//   exh <group> <36 bytes hex>
//   evt <group> <16 bytes hex> <events hex>
//   mbd <group> <8 bytes hex>
//   pacentry <pac under pac\, e.g. evt\Nyujyo5.pac> <GROUP> <name> <file>
//     (a whole entry of an EPK8 or EPAC pac, unpacked: e.g. an entrance's script
//     and motions, EVPE/EVTE <number>, or its pyro, prtcls.pac PCE6/LPE6 <0NNN>
//     (4-character names in EPAC); added, or replacing the game's. The pac is
//     copied to the overlay with the entries in - never written in place)
// Keys the game already has are left as they are.
//
// The merged pacs go to <game>/Mods/PacOverlay with copies of pac/plist360.h
// and plist360_4x3.h naming them ("mods\pacoverlay\m.pac"). The game reads
// its pac list from there (arena_mods.cpp: sub_826B9280) and, since the
// pre-built directory plist360.arc only knows the original files, mounts the
// pacs by reading each one's own table (superstar_mods.cpp: sub_825953B0
// instead of sub_82595428). Unchanged entries are streamed from the original
// files; changed ones are re-compressed (real BPE).
#include "move_packs.h"
#include "superstar_mods.h"
#include "startup_pump.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "../modmaker/svrfmt/pac.h"

namespace {

namespace fs = std::filesystem;
using svrfmt::Bytes;

constexpr const char* kFormat = "movepacks 10";  // (2: banks whose sentinel points past the data; 3: WAZE category counts; 4: YMBs banks, record names; 6: WAZA records in id order; 7: of two packs' copies of a motion the first read is used; 8: pacentry lines; 9: YMBs motions appended after the stock ones; 10: pacentry into EPAC pacs (prtcls.pac pyro))
std::string g_folder;  // PacListFolder

struct Motion {
  uint16_t id = 0;
  uint8_t x = 0, y = 0;
  uint32_t frames = 0;
  Bytes data;
  Bytes hdr;  // (YMBs: the 20-byte motion header)
  std::string pack;  // (the pack it came from: its folder, or the superstar mod's)
};
struct Record {
  uint32_t group = 0;
  Bytes rec, events;
};
struct Packs {
  // pac -> "TYPE/NAME" -> child path -> motions
  std::map<std::string, std::map<std::string, std::map<std::vector<uint32_t>, std::vector<Motion>>>> motions;
  std::map<uint16_t, Bytes> waze;
  std::map<uint16_t, std::string> names;
  std::map<uint16_t, uint16_t> copies;  // record id -> the id its record is copied from
  std::vector<Record> exh, evt, mbd;
  // EPK8 pac (under pac\, lower case, e.g. "evt\nyujyo5.pac") -> "GROUP/name" -> unpacked data
  // (pacentry lines: whole entries added to / replacing the pac's, e.g. an entrance's EVPE/EVTE)
  std::map<std::string, std::map<std::string, Bytes>> entries;
  // every pacentry line read (also without the data): for the entrances the packs add (ModEntrances)
  struct EntryLine {
    std::string pac, group, name;
    fs::path dir, file;
  };
  std::vector<EntryLine> entry_lines;
  size_t count = 0;
};

uint32_t Le32(const uint8_t* p) { return svrfmt::Le32(p); }
uint16_t Le16(const uint8_t* p) { return svrfmt::Le16(p); }
void PutLe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }

Bytes Hex(const std::string& s) {
  Bytes out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(std::strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
  return out;
}

bool ReadAll(const fs::path& p, Bytes& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  f.seekg(0, std::ios::end);
  out.resize(size_t(f.tellg()));
  f.seekg(0);
  return bool(f.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
}

// ---- reading the packs

// The motion ids the enabled packs bring as YMBs (streamed) motions - a
// motion line with a 20-byte header (YmbsInsert) - for the game's "streamed
// motion" flag (sub_82154F00, below). Set as the packs are read (each start,
// merged or not), read in matches.
std::atomic<bool> g_ymbs_motion[0x8000] = {};

// data false: the lists and the stamp only (no motion files read - enough to
// see that the overlay is up to date: thousands of small files on a hard
// drive with a cold cache took 35 s). A missing motion file counts in both
// ways, so the stamps match.
void ReadPack(const fs::path& dir, Packs& p, std::string& stamp, bool data = true) {
  std::ifstream f(dir / "pack.txt");
  std::string line;
  size_t n = 0;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream in(line);
    std::string kind;
    in >> kind;
    if (kind == "motion") {
      std::string pac, path, file;
      uint32_t id = 0, x = 0, y = 0, frames = 0;
      std::string hdr;
      in >> pac >> path >> id >> x >> y >> frames >> file >> hdr;
      std::vector<std::string> parts;
      for (size_t a = 0; a <= path.size();) {
        size_t e = path.find('/', a);
        if (e == std::string::npos) e = path.size();
        parts.push_back(path.substr(a, e - a));
        a = e + 1;
      }
      if (parts.size() < 2 || file.empty()) continue;
      std::vector<uint32_t> kids;
      for (size_t k = 2; k < parts.size(); ++k) kids.push_back(uint32_t(std::strtoul(parts[k].c_str(), nullptr, 16)));
      Motion m;
      m.id = uint16_t(id), m.x = uint8_t(x), m.y = uint8_t(y), m.frames = frames;
      m.hdr = Hex(hdr);
      if (m.hdr.size() == 20) g_ymbs_motion[id & 0x7FFF] = true;
      m.pack = (dir.filename() == "moves" ? dir.parent_path() : dir).filename().string();
      // (the motion file in the stamp - size and time, no read: a pack whose
      // motion files changed but not its pack.txt was kept merged as it was)
      {
        std::error_code ec;
        const uintmax_t size = fs::file_size(dir / fs::u8path(file), ec);
        stamp += file + " " + std::to_string(ec ? 0 : size) + " " +
                 std::to_string(static_cast<long long>(fs::last_write_time(dir / fs::u8path(file), ec).time_since_epoch().count())) + "\n";
      }
      if (data) {
        svr2011::KeepWindowAlive();
        if (!ReadAll(dir / fs::u8path(file), m.data)) {
          REXLOG_WARN("[svr2011] move packs: {}: missing {}", dir.filename().string(), file);
          ++n;
          continue;
        }
      }
      std::string key = parts[0];
      key.resize(4, ' ');
      std::string name = parts[1];
      name.resize(4, ' ');
      p.motions[pac][key + "/" + name][kids].push_back(std::move(m));
    } else if (kind == "waze") {
      uint32_t id = 0;
      std::string hex;
      in >> id >> hex;
      if (const Bytes b = Hex(hex); b.size() == 16) p.waze[uint16_t(id)] = b;
    } else if (kind == "wazecopy") {
      uint32_t id = 0, from = 0;
      in >> id >> from;
      if (id && from && id != from) p.copies[uint16_t(id)] = uint16_t(from);
    } else if (kind == "wazename") {
      uint32_t id = 0;
      std::string name;
      in >> id;
      std::getline(in, name);
      name.erase(0, name.find_first_not_of(' '));
      if (id && !name.empty()) p.names[uint16_t(id)] = name.substr(0, 63);
    } else if (kind == "exh" || kind == "evt" || kind == "mbd") {
      Record r;
      std::string rec, ev;
      in >> r.group >> rec >> ev;
      r.rec = Hex(rec), r.events = Hex(ev);
      const size_t want = kind == "exh" ? 36 : kind == "evt" ? 16 : 8;
      if (r.rec.size() != want) continue;
      (kind == "exh" ? p.exh : kind == "evt" ? p.evt : p.mbd).push_back(std::move(r));
    } else if (kind == "pacentry") {
      std::string pac, group, name, file;
      in >> pac >> group >> name >> file;
      if (group.size() == 3) group += ' ';  // ("EMD": the model group "EMD ", attire mods)
      if (group.size() != 4 || name.empty() || name.size() > 8 || file.empty()) continue;
      for (auto& c : pac) c = c == '/' ? '\\' : char(std::tolower(static_cast<unsigned char>(c)));
      if (pac.find("..") != std::string::npos || pac.size() < 5 || pac.compare(pac.size() - 4, 4, ".pac")) continue;
      Bytes bytes;
      std::error_code ec;
      const uintmax_t size = data ? 0 : fs::file_size(dir / fs::u8path(file), ec);
      if (data ? !ReadAll(dir / fs::u8path(file), bytes) : bool(ec)) {
        REXLOG_WARN("[svr2011] move packs: {}: missing {}", dir.filename().string(), file);
        continue;
      }
      // (the file in the stamp: a changed entry rebuilds the overlay)
      stamp += file + " " + std::to_string(data ? uintmax_t(bytes.size()) : size) + " " +
               std::to_string(static_cast<long long>(fs::last_write_time(dir / fs::u8path(file), ec).time_since_epoch().count())) + "\n";
      p.entries[pac].emplace(group + "/" + name, std::move(bytes));  // (the first pack's wins)
      p.entry_lines.push_back({pac, group, name, dir, dir / fs::u8path(file)});
    } else {
      continue;
    }
    ++n;
  }
  p.count += n;
  stamp += dir.string() + " " + std::to_string(n) + "\n";
}

// ---- YMKs banks

uint32_t Key(uint16_t id, uint8_t x, uint8_t y) { return uint32_t(id) << 16 | uint32_t(x) << 8 | y; }

bool YmbsInsert(Bytes& raw, const std::vector<Motion>& add, size_t& added);

// Inserts the motions (keys the bank lacks) and lays the data out in
// directory order. False if the bank can't take them (not YMKs, unsorted).
bool BankInsert(Bytes& raw, const std::vector<Motion>& add, size_t& added) {
  if (raw.size() >= 0x114 && !std::memcmp(raw.data(), "YMBs", 4)) return YmbsInsert(raw, add, added);
  if (raw.size() < 0x114 || std::memcmp(raw.data(), "YMKs", 4)) return false;
  const uint32_t n = Le32(&raw[0x110]);
  const size_t base = 0x114 + size_t(n) * 16;
  if (base > raw.size() || n == 0) return false;
  struct E {
    uint8_t y, x;
    uint16_t id;
    int64_t off;  // -1: new
    uint32_t frames, rt;
    const uint8_t* data;
    size_t size;
  };
  std::vector<E> ents(n);
  std::vector<uint32_t> offs;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* d = &raw[0x114 + 16 * i];
    ents[i] = {d[0], d[1], Le16(d + 2), Le32(d + 4), Le32(d + 8), Le32(d + 12), nullptr, 0};
    offs.push_back(uint32_t(ents[i].off));
  }
  std::sort(offs.begin(), offs.end());
  offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
  for (auto& e : ents) {
    const auto it = std::upper_bound(offs.begin(), offs.end(), uint32_t(e.off));
    // (a bank's closing sentinel, id 32767, can point past the data -
    // MVMT/SPE2: it has none)
    const size_t data = raw.size() - base;
    const size_t end = std::min<size_t>(it == offs.end() ? data : *it, data);
    const size_t off = std::min<size_t>(size_t(e.off), data);
    e.data = raw.data() + base + off;
    e.size = end > off ? end - off : 0;
  }
  for (uint32_t i = 0; i + 1 < n; ++i)
    if (ents[i].off > ents[i + 1].off) return false;
  std::map<uint32_t, int> rts;
  for (const auto& e : ents) ++rts[e.rt];
  const uint32_t rt = std::max_element(rts.begin(), rts.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
  std::vector<uint32_t> keys;
  std::set<uint32_t> present;
  for (const auto& e : ents) keys.push_back(Key(e.id, e.x, e.y)), present.insert(keys.back());
  std::vector<const Motion*> sorted;
  for (const auto& m : add) sorted.push_back(&m);
  std::stable_sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return Key(a->id, a->x, a->y) < Key(b->id, b->x, b->y); });
  added = 0;
  for (const Motion* m : sorted) {
    const uint32_t k = Key(m->id, m->x, m->y);
    if (present.count(k)) continue;
    if (m->id >= ents.back().id) return false;  // (above the bank's sentinel)
    size_t pos = 0;
    while (pos < keys.size() && keys[pos] <= k) ++pos;
    if (pos && keys[pos - 1] > k) return false;
    keys.insert(keys.begin() + long(pos), k);
    ents.insert(ents.begin() + long(pos), E{m->y, m->x, m->id, -1, m->frames, rt, m->data.data(), m->data.size()});
    present.insert(k);
    ++added;
  }
  if (!added) return true;
  Bytes body, dir;
  int64_t last_old = -2;
  uint32_t last_new = 0;
  for (const auto& e : ents) {
    uint32_t off;
    if (e.off >= 0 && e.off == last_old) {
      off = last_new;  // (neighbours sharing one block keep sharing it)
    } else {
      off = uint32_t(body.size());
      body.insert(body.end(), e.data, e.data + e.size);
    }
    last_old = e.off, last_new = off;
    uint8_t d[16];
    d[0] = e.y, d[1] = e.x;
    PutLe16(d + 2, e.id);
    svrfmt::PutLe32(d + 4, off);
    svrfmt::PutLe32(d + 8, e.frames);
    svrfmt::PutLe32(d + 12, e.rt);
    dir.insert(dir.end(), d, d + 16);
  }
  Bytes out(raw.begin(), raw.begin() + 0x110);
  svrfmt::AppLe32(out, uint32_t(ents.size()));
  svrfmt::App(out, dir);
  svrfmt::App(out, body);
  raw = std::move(out);
  return true;
}

// A YMBs bank (re_charmodel.md): the same directory, then a database - u32,
// a 20-byte header per motion (same order), the data - with the directory
// offsets counted from the database. New motions bring their header.
bool YmbsInsert(Bytes& raw, const std::vector<Motion>& add, size_t& added) {
  const uint32_t n = Le32(&raw[0x110]);
  const size_t db = 0x114 + size_t(n) * 16;
  if (n == 0 || db + 4 + 20 * size_t(n) > raw.size()) return false;
  struct E {
    uint8_t y, x;
    uint16_t id;
    int64_t off;  // (from the database; -1: new)
    uint32_t frames, rt;
    const uint8_t* hdr;
    const uint8_t* data;
    size_t size;
  };
  const size_t data_size = raw.size() - db;
  std::vector<E> ents(n);
  std::vector<uint32_t> offs;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* d = &raw[0x114 + 16 * i];
    ents[i] = {d[0], d[1], Le16(d + 2), Le32(d + 4), Le32(d + 8), Le32(d + 12), &raw[db + 4 + 20 * i], nullptr, 0};
    offs.push_back(uint32_t(ents[i].off));
  }
  std::sort(offs.begin(), offs.end());
  offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
  for (auto& e : ents) {
    const auto it = std::upper_bound(offs.begin(), offs.end(), uint32_t(e.off));
    const size_t end = std::min<size_t>(it == offs.end() ? data_size : *it, data_size);
    const size_t off = std::min<size_t>(size_t(e.off), data_size);
    e.data = raw.data() + db + off;
    e.size = end > off ? end - off : 0;
  }
  std::map<uint32_t, int> rts;
  for (const auto& e : ents) ++rts[e.rt];
  const uint32_t rt = std::max_element(rts.begin(), rts.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
  std::vector<uint32_t> keys;
  std::set<uint32_t> present;
  for (const auto& e : ents) keys.push_back(Key(e.id, e.x, e.y)), present.insert(keys.back());
  for (size_t i = 0; i + 1 < keys.size(); ++i)
    if (keys[i] > keys[i + 1]) return false;
  std::vector<const Motion*> sorted;
  for (const auto& m : add) sorted.push_back(&m);
  std::stable_sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return Key(a->id, a->x, a->y) < Key(b->id, b->x, b->y); });
  added = 0;
  for (const Motion* m : sorted) {
    const uint32_t k = Key(m->id, m->x, m->y);
    if (present.count(k) || m->hdr.size() != 20) continue;
    if (m->id >= ents.back().id) return false;  // (above the bank's sentinel)
    // Appended after the stock entries (before the end mark), never between
    // them: the game keeps YMBs motions by their place in the bank, so one put
    // in front shifted every stock motion after it - any superstar's Camel
    // Clutch 2 (21450) then read another motion's data and crashed once
    // Sabu's ported camel clutch (5962) went in ahead of it.
    size_t pos = keys.size();
    while (pos > 0 && ents[pos - 1].id == ents.back().id) --pos;
    keys.insert(keys.begin() + long(pos), k);
    ents.insert(ents.begin() + long(pos), E{m->y, m->x, m->id, -1, m->frames, rt, m->hdr.data(), m->data.data(), m->data.size()});
    present.insert(k);
    ++added;
  }
  if (!added) return true;
  const size_t start = 4 + 20 * ents.size();  // (the data's offset in the new database)
  Bytes body, dir, hdrs;
  int64_t last_old = -2;
  uint32_t last_new = 0;
  for (const auto& e : ents) {
    uint32_t off;
    if (e.off >= 0 && e.off == last_old) {
      off = last_new;  // (neighbours sharing one block keep sharing it)
    } else {
      off = uint32_t(start + body.size());
      body.insert(body.end(), e.data, e.data + e.size);
    }
    last_old = e.off, last_new = off;
    uint8_t d[16];
    d[0] = e.y, d[1] = e.x;
    PutLe16(d + 2, e.id);
    svrfmt::PutLe32(d + 4, off);
    svrfmt::PutLe32(d + 8, e.frames);
    svrfmt::PutLe32(d + 12, e.rt);
    dir.insert(dir.end(), d, d + 16);
    hdrs.insert(hdrs.end(), e.hdr, e.hdr + 20);
  }
  Bytes out(raw.begin(), raw.begin() + 0x110);
  svrfmt::AppLe32(out, uint32_t(ents.size()));
  svrfmt::App(out, dir);
  // (the database's word: the header size << 16 | the motion count - stock
  // 0x140138 for 312 - the new count, not the old one)
  svrfmt::AppLe32(out, (Le32(&raw[db]) & 0xFFFF0000u) | uint32_t(ents.size() & 0xFFFF));
  svrfmt::App(out, hdrs);
  svrfmt::App(out, body);
  raw = std::move(out);
  return true;
}

// ---- PACH trees

Bytes Repack(const Bytes& raw, bool was_bpe) { return was_bpe ? svrfmt::BpeEncode(raw) : raw; }

// Applies `edit` to the descendant of `blob` at child ids `path` (each level
// re-packed as it was). False if a level is missing.
template <typename F>
bool EditTree(Bytes& blob, const std::vector<uint32_t>& path, size_t depth, F&& edit) {
  const bool bpe = svrfmt::IsBpe(blob);
  Bytes raw = svrfmt::Unpack(blob);
  if (depth == path.size()) {
    if (!edit(raw)) return false;
    blob = Repack(raw, bpe);
    return true;
  }
  std::vector<svrfmt::PachEntry> kids;
  if (!svrfmt::PachRead(raw, kids)) return false;
  bool done = false;
  for (auto& k : kids)
    if (k.id == path[depth]) done = EditTree(k.data, path, depth + 1, edit);
  if (!done) return false;
  blob = Repack(svrfmt::PachWrite(kids), bpe);
  return true;
}

// ---- record tables (misc.pac)

uint32_t RecKey(const uint8_t* r) { return uint32_t(Le16(r)) << 16 | uint32_t(r[2]) << 8 | r[3]; }

// Inserts records (header of `head` bytes, `size`-byte records, count u32 at
// +4) in move id order (after the id's own records; appended only if the
// table isn't in id order); keys (id, x, b3) present stay. The game finds a
// record by a binary search on the u16 id alone (sub_826A4960): its tables
// are sorted by id, not always by the whole key (EXH group 16 has 845/0/1
// before 845/0/0), and records appended there after the last id - ported
// strikes - left them and every later stock id unfound: no hit reaction or
// impact sound (2.0.4 report, the SvR2010 superstars' strikes).
void InsertRecords(Bytes& t, size_t head, size_t size, std::vector<Bytes> add, Bytes* pool = nullptr,
                   std::vector<Bytes>* events = nullptr) {
  const uint32_t n = Le32(&t[4]);
  std::vector<Bytes> recs;
  for (uint32_t i = 0; i < n; ++i) recs.emplace_back(t.begin() + long(head + i * size), t.begin() + long(head + (i + 1) * size));
  Bytes tail(t.begin() + long(head + n * size), t.end());
  bool sorted = true;
  for (size_t i = 0; i + 1 < recs.size(); ++i) sorted &= Le16(recs[i].data()) <= Le16(recs[i + 1].data());
  std::set<uint32_t> have;
  for (const auto& r : recs) have.insert(RecKey(r.data()));
  for (size_t a = 0; a < add.size(); ++a) {
    Bytes r = add[a];
    if (have.count(RecKey(r.data()))) continue;
    if (pool && events) {  // (an event record: its events appended to the pool)
      PutLe16(&r[8], uint16_t(tail.size() / 4));
      r[10] = uint8_t((*events)[a].size() / 4);
      svrfmt::App(tail, (*events)[a]);
    }
    have.insert(RecKey(r.data()));
    if (!sorted) {
      recs.push_back(r);
      continue;
    }
    size_t pos = 0;
    while (pos < recs.size() && Le16(recs[pos].data()) <= Le16(r.data())) ++pos;
    recs.insert(recs.begin() + long(pos), r);
  }
  Bytes out(t.begin(), t.begin() + long(head));
  svrfmt::PutLe32(&out[4], uint32_t(recs.size()));
  for (const auto& r : recs) svrfmt::App(out, r);
  svrfmt::App(out, tail);
  t = std::move(out);
}

// One WAZA/DATA child (a PACH of groups) with the pack's records added.
bool EditGroups(Bytes& child, const std::vector<Record>& recs, int kind) {
  std::map<uint32_t, std::vector<const Record*>> by;
  for (const auto& r : recs) by[r.group].push_back(&r);
  if (by.empty()) return true;
  const bool bpe = svrfmt::IsBpe(child);
  std::vector<svrfmt::PachEntry> groups;
  if (!svrfmt::PachRead(svrfmt::Unpack(child), groups)) return false;
  for (auto& g : groups) {
    auto it = by.find(g.id);
    if (it == by.end()) continue;
    const bool gb = svrfmt::IsBpe(g.data);
    Bytes t = svrfmt::Unpack(g.data);
    std::vector<Bytes> add, events;
    for (const Record* r : it->second) add.push_back(r->rec), events.push_back(r->events);
    if (kind == 0 && t.size() >= 8 && !std::memcmp(t.data(), "EXH", 4)) InsertRecords(t, 8, 36, add);
    else if (kind == 1 && t.size() >= 8 && Le32(t.data()) == 16) InsertRecords(t, 8, 16, add, &t, &events);
    else if (kind == 2 && t.size() >= 8 && !std::memcmp(t.data(), "MBD", 4)) InsertRecords(t, 8, 8, add);
    else continue;
    g.data = Repack(t, gb);
  }
  child = Repack(svrfmt::PachWrite(groups), bpe);
  return true;
}

// ---- EPAC streaming

struct TocEntry {
  std::string type, name;
  uint32_t sector, size256;
};

bool ReadToc(std::ifstream& f, Bytes& head, std::vector<TocEntry>& toc) {
  head.resize(0x4000);
  f.seekg(0);
  if (!f.read(reinterpret_cast<char*>(head.data()), 0x4000) || std::memcmp(head.data(), "EPAC", 4)) return false;
  for (size_t p = 0x800; p + 12 <= 0x4000 && Le32(&head[p]);) {
    const std::string type(reinterpret_cast<char*>(&head[p]), 4);
    const uint32_t cnt = Le32(&head[p + 4]) / 3;
    p += 12;
    for (uint32_t i = 0; i < cnt && p + 12 <= 0x4000; ++i, p += 12)
      toc.push_back({type, std::string(reinterpret_cast<char*>(&head[p]), 4), Le32(&head[p + 4]), Le32(&head[p + 8])});
  }
  return true;
}

// Writes `src` to `dst` with the entries in `changed` ("TYPE/NAME") replaced,
// laid out as svrfmt::EpacWrite does.
bool WritePac(const fs::path& src, const fs::path& dst, const std::map<std::string, Bytes>& changed) {
  std::ifstream in(src, std::ios::binary);
  Bytes head;
  std::vector<TocEntry> toc;
  if (!in || !ReadToc(in, head, toc)) return false;
  in.seekg(0, std::ios::end);
  const uint64_t src_size = uint64_t(in.tellg());
  uint64_t src_data_end = 0x4000 + uint64_t(Le32(&head[8]));
  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  Bytes zeros(0x800, 0);
  out.write(reinterpret_cast<char*>(head.data()), 0x4000);  // (the table is rewritten at the end)
  uint64_t pos = 0;  // within the data
  Bytes newtoc;
  std::string group;
  size_t group_at = 0;
  std::vector<char> buf(1 << 20);
  for (size_t i = 0; i < toc.size(); ++i) {
    const auto& e = toc[i];
    if (e.type != group) {
      group = e.type;
      group_at = newtoc.size();
      svrfmt::AppName(newtoc, e.type, 4);
      svrfmt::AppLe32(newtoc, 0);
      svrfmt::AppLe32(newtoc, 0);
    }
    svrfmt::PutLe32(&newtoc[group_at + 4], Le32(&newtoc[group_at + 4]) + 3);
    if (pos % 0x800) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x800 - pos % 0x800)), pos += 0x800 - pos % 0x800;
    svrfmt::AppName(newtoc, e.name, 4);
    svrfmt::AppLe32(newtoc, uint32_t(pos / 0x800));
    const auto it = changed.find(e.type + "/" + e.name);
    uint64_t size;
    if (it != changed.end()) {
      out.write(reinterpret_cast<const char*>(it->second.data()), std::streamsize(it->second.size()));
      size = it->second.size();
    } else {
      size = uint64_t(e.size256) * 0x100;
      in.seekg(std::streamoff(0x4000 + uint64_t(e.sector) * 0x800));
      for (uint64_t left = size; left;) {
        const size_t n = size_t(std::min<uint64_t>(left, buf.size()));
        if (!in.read(buf.data(), std::streamsize(n))) return false;
        out.write(buf.data(), std::streamsize(n));
        left -= n;
      }
    }
    svrfmt::AppLe32(newtoc, uint32_t((size + 0xFF) / 0x100));
    pos += size;
    if (pos % 0x100) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x100 - pos % 0x100)), pos += 0x100 - pos % 0x100;
  }
  if (pos % 0x800) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x800 - pos % 0x800)), pos += 0x800 - pos % 0x800;
  // the packer's footer after the data, as the original has it
  if (src_data_end < src_size) {
    in.seekg(std::streamoff(src_data_end));
    std::vector<char> tail(size_t(src_size - src_data_end));
    if (in.read(tail.data(), std::streamsize(tail.size()))) out.write(tail.data(), std::streamsize(tail.size()));
  } else {
    out.write(reinterpret_cast<char*>(zeros.data()), 0x800);
  }
  newtoc.resize(0x3800, 0);
  std::memcpy(&head[0x800], newtoc.data(), 0x3800);
  svrfmt::PutLe32(&head[8], uint32_t(pos));
  out.seekp(0);
  out.write(reinterpret_cast<char*>(head.data()), 0x4000);
  return bool(out);
}

// ---- EPK8 (evt\Nyujyo*.pac): pacentry entries added
// Table at 0x800-0x4000: groups {4CC, u16 count * 4, u16 1, u32 0} each followed
// by 16-byte entries {char name[8] (space padded), u32 sector (0x800 units from
// 0x4000), u32 size / 0x100}; header +4 = the table's size, +8 = the data's.
// Names are numbers kept in numeric order, "65535" (an end mark) last. The
// added entries are BPE-packed as the game's are.

// A pac list name ("evt\nyujyo5.pac") as a path on this system (Android: '/').
fs::path PacPath(std::string pac) {
  std::replace(pac.begin(), pac.end(), '\\', '/');
  return fs::u8path(pac);
}

uint64_t Epk8Order(const std::string& name) {
  const std::string n = name.substr(0, name.find(' '));
  if (n == "65535") return ~0ull;
  return n.empty() || n.find_first_not_of("0123456789") != std::string::npos ? ~0ull - 1 : std::stoull(n);
}

bool BuildEntryPac(const fs::path& game, const fs::path& overlay, const std::string& pac,
                   const std::map<std::string, Bytes>& add) {
  const fs::path src = game / "pac" / PacPath(pac), dst = overlay / PacPath(pac);
  std::ifstream in(src, std::ios::binary);
  Bytes head(0x4000);
  if (!in || !in.read(reinterpret_cast<char*>(head.data()), 0x4000) ||
      (std::memcmp(head.data(), "EPK8", 4) && std::memcmp(head.data(), "EPAC", 4))) {
    REXLOG_WARN("[svr2011] move packs: {} is not an EPK8 / EPAC pac", pac);
    return false;
  }
  // EPAC (prtcls.pac: an entrance's pyro PCE6/LPE6 <0NNN>): groups {4CC, u32 count * 3, u32 0},
  // 12-byte entries {char name[4], u32 sector, u32 size / 0x100}; added entries are packed only
  // when the group's own are (prtcls.pac's are plain PACH)
  const bool epac = !std::memcmp(head.data(), "EPAC", 4);
  const size_t name_len = epac ? 4 : 8, ent_len = name_len + 8;
  struct E {
    std::string name;
    uint32_t sector = 0, size256 = 0;
    const Bytes* data = nullptr;  // (an added entry, packed)
  };
  struct G {
    std::string type;
    Bytes extra;
    std::vector<E> ents;
  };
  std::vector<G> groups;
  for (size_t q = 0x800; q + 12 <= 0x4000 && Le32(&head[q]);) {
    G g;
    g.type.assign(reinterpret_cast<char*>(&head[q]), 4);
    const uint32_t n = epac ? Le32(&head[q + 4]) / 3 : Le16(&head[q + 4]) / 4;
    g.extra.assign(head.begin() + long(q + (epac ? 8 : 6)), head.begin() + long(q + 12));
    q += 12;
    for (uint32_t i = 0; i < n && q + ent_len <= 0x4000; ++i, q += ent_len)
      g.ents.push_back({std::string(reinterpret_cast<char*>(&head[q]), name_len), Le32(&head[q + name_len]),
                        Le32(&head[q + name_len + 4])});
    groups.push_back(std::move(g));
  }
  std::map<std::string, Bytes> packed;
  for (const auto& [key, raw] : add) {
    const std::string type = key.substr(0, 4), name = (key.substr(5) + "        ").substr(0, name_len);
    auto g = std::find_if(groups.begin(), groups.end(), [&](const G& x) { return x.type == type; });
    if (g == groups.end()) {
      REXLOG_WARN("[svr2011] move packs: {} has no group {}", pac, type);
      continue;
    }
    bool bpe = !epac;
    // (stored as the group's own entries are: evt's EPK8 entries are BPE-packed, a
    // model pac's EMD entries plain PACHs - BPE-packed, the game couldn't load one)
    if (!g->ents.empty()) {
      char magic[4] = {};
      in.seekg(std::streamoff(0x4000 + uint64_t(g->ents[0].sector) * 0x800));
      bpe = in.read(magic, 4) && !std::memcmp(magic, "BPE ", 4);
      in.clear();
    }
    const Bytes& data = packed[key] = bpe ? svrfmt::BpeEncode(raw) : raw;
    auto e = std::find_if(g->ents.begin(), g->ents.end(), [&](const E& x) { return x.name == name; });
    if (e == g->ents.end()) g->ents.push_back({name, 0, 0, &data});
    else e->data = &data;
    std::stable_sort(g->ents.begin(), g->ents.end(), [](const E& a, const E& b) { return Epk8Order(a.name) < Epk8Order(b.name); });
  }
  in.seekg(0, std::ios::end);
  const uint64_t src_size = uint64_t(in.tellg()), src_end = 0x4000 + uint64_t(Le32(&head[8]));
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(reinterpret_cast<char*>(head.data()), 0x4000);  // (the table is rewritten at the end)
  Bytes table, zeros(0x800, 0);
  std::vector<char> buf(1 << 20);
  uint64_t pos = 0;
  for (const auto& g : groups) {
    svrfmt::AppName(table, g.type, 4);
    if (epac) svrfmt::AppLe32(table, uint32_t(g.ents.size() * 3));
    else table.push_back(uint8_t(g.ents.size() * 4)), table.push_back(uint8_t(g.ents.size() * 4 >> 8));
    table.insert(table.end(), g.extra.begin(), g.extra.end());
    for (const auto& e : g.ents) {
      if (pos % 0x800) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x800 - pos % 0x800)), pos += 0x800 - pos % 0x800;
      table.insert(table.end(), e.name.begin(), e.name.end());
      svrfmt::AppLe32(table, uint32_t(pos / 0x800));
      uint64_t size;
      if (e.data) {
        out.write(reinterpret_cast<const char*>(e.data->data()), std::streamsize(e.data->size()));
        size = e.data->size();
      } else {
        size = uint64_t(e.size256) * 0x100;
        in.seekg(std::streamoff(0x4000 + uint64_t(e.sector) * 0x800));
        for (uint64_t left = size; left;) {
          const size_t n = size_t(std::min<uint64_t>(left, buf.size()));
          if (!in.read(buf.data(), std::streamsize(n))) return false;
          out.write(buf.data(), std::streamsize(n));
          left -= n;
        }
      }
      svrfmt::AppLe32(table, uint32_t((size + 0xFF) / 0x100));
      pos += size;
      if (pos % 0x100) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x100 - pos % 0x100)), pos += 0x100 - pos % 0x100;
    }
  }
  if (table.size() > 0x3800) {
    REXLOG_WARN("[svr2011] move packs: {}: too many entries for the table", pac);
    return false;
  }
  if (pos % 0x800) out.write(reinterpret_cast<char*>(zeros.data()), std::streamsize(0x800 - pos % 0x800)), pos += 0x800 - pos % 0x800;
  if (src_end < src_size) {  // (the packer's footer, as the original has it)
    in.seekg(std::streamoff(src_end));
    std::vector<char> tail(size_t(src_size - src_end));
    if (in.read(tail.data(), std::streamsize(tail.size()))) out.write(tail.data(), std::streamsize(tail.size()));
  } else {
    out.write(reinterpret_cast<char*>(zeros.data()), 0x800);
  }
  const uint32_t table_size = uint32_t(table.size());
  table.resize(0x3800, 0);
  std::memcpy(&head[0x800], table.data(), 0x3800);
  svrfmt::PutLe32(&head[4], table_size);
  svrfmt::PutLe32(&head[8], uint32_t(pos));
  out.seekp(0);
  out.write(reinterpret_cast<char*>(head.data()), 0x4000);
  REXLOG_INFO("[svr2011] move packs: {}: {} entries added or replaced", pac, add.size());
  return bool(out);
}

bool ReadEntry(const fs::path& pac, const std::string& type_name, Bytes& out) {
  std::ifstream in(pac, std::ios::binary);
  Bytes head;
  std::vector<TocEntry> toc;
  if (!in || !ReadToc(in, head, toc)) return false;
  for (const auto& e : toc)
    if (e.type + "/" + e.name == type_name) {
      out.resize(size_t(e.size256) * 0x100);
      in.seekg(std::streamoff(0x4000 + uint64_t(e.sector) * 0x800));
      return bool(in.read(reinterpret_cast<char*>(out.data()), std::streamsize(out.size())));
    }
  return false;
}

// Two packs with the same motion key: the first read is used (packs are read
// in path order - Mods\Moves\<pack> before Mods\Superstars\<mod>\moves, then
// A-Z), the other is ignored. Copies that are the same (one ported move in
// several superstar mods) are normal; different ones are logged, once per
// pair of packs, with the move ids: an old exported move pack can hide a
// mod's fixed motion that way.
void WarnOverrides(const Packs& p) {
  std::map<std::pair<std::string, std::string>, std::set<uint16_t>> diff;  // (used, ignored) -> move ids
  for (const auto& [pac, entries] : p.motions)
    for (const auto& [type_name, banks] : entries)
      for (const auto& [path, motions] : banks) {
        std::map<uint32_t, const Motion*> first;
        for (const Motion& m : motions) {
          const auto [it, fresh] = first.emplace(Key(m.id, m.x, m.y), &m);
          if (!fresh && it->second->pack != m.pack && it->second->data != m.data)
            diff[{it->second->pack, m.pack}].insert(m.id);
        }
      }
  for (const auto& [packs, ids] : diff) {
    std::string list;
    for (uint16_t id : ids) list += (list.empty() ? "" : " ") + std::to_string(id);
    REXLOG_WARN("[svr2011] move packs: \"{}\" and \"{}\" carry different motions for moves {}: \"{}\"'s are used",
                packs.first, packs.second, list, packs.first);
  }
}

// ---- building

bool BuildMotionPac(const fs::path& game, const fs::path& overlay, const std::string& pac,
                    const std::map<std::string, std::map<std::vector<uint32_t>, std::vector<Motion>>>& entries) {
  std::map<std::string, Bytes> changed;
  size_t total = 0;
  for (const auto& [type_name, banks] : entries) {
    Bytes blob;
    if (!ReadEntry(game / "pac" / pac, type_name, blob)) {
      REXLOG_WARN("[svr2011] move packs: {} has no {}", pac, type_name);
      continue;
    }
    for (const auto& [path, motions] : banks) {
      size_t added = 0;
      if (!EditTree(blob, path, 0, [&](Bytes& raw) { return BankInsert(raw, motions, added); })) {
        REXLOG_WARN("[svr2011] move packs: {} {}: bank not changed", pac, type_name);
        continue;
      }
      total += added;
    }
    changed[type_name] = std::move(blob);
  }
  if (!WritePac(game / "pac" / pac, overlay / pac, changed)) return false;
  REXLOG_INFO("[svr2011] move packs: {}: {} motions added", pac, total);
  return true;
}

// MOVS/WAZE as in the file (little-endian: u32 255, u32 record count, 128
// u16 category counts, then 160-byte records from +272 - the first "* test
// motion *" - each with its category bits, u64s at +0 / +8, and its move id
// at +0x90): the packs' moves get their 16 bytes of bits, and the category
// counts are recounted. Those counts size the game's move lists (sub_8237BA68
// allocates by them, then writes every member): stock counts with new bits
// overran the lists and lost the ported moves at their ends. Returns the
// records set (-1: not that layout); *fixed: the counts changed.
int PatchWaze(uint8_t* w, size_t size, const std::map<uint16_t, Bytes>& waze, int* fixed,
              const std::map<uint16_t, std::string>& names = {}, const std::map<uint16_t, uint16_t>& copies = {}) {
  static const char kMark[] = "* test motion *";
  if (size < 272 + 160 || std::memcmp(w + 272 + 16, kMark, sizeof kMark - 1)) return -1;
  const uint32_t records = Le32(w + 4);
  if (size_t(records) * 160 + 272 > size) return -1;
  int set = 0;
  if (!copies.empty()) {  // (whole records of other moves, the id kept)
    std::map<uint16_t, uint8_t*> by_id;
    for (uint32_t k = 0; k < records; ++k) {
      uint8_t* r = w + 272 + size_t(k) * 160;
      if (!std::all_of(r, r + 160, [](uint8_t b) { return b == 0; })) by_id.emplace(Le16(r + 0x90), r);
    }
    for (const auto& [id, from] : copies) {
      const auto d = by_id.find(id), f = by_id.find(from);
      if (d == by_id.end() || f == by_id.end()) continue;
      uint8_t rec[160];
      std::memcpy(rec, f->second, 160);
      std::memcpy(rec + 0x90, d->second + 0x90, 2);
      if (std::memcmp(d->second, rec, 160)) std::memcpy(d->second, rec, 160), ++set;
    }
  }
  for (uint32_t k = 0; k < records; ++k) {
    uint8_t* r = w + 272 + size_t(k) * 160;
    if (std::all_of(r, r + 160, [](uint8_t b) { return b == 0; })) continue;
    const uint16_t id = Le16(r + 0x90);
    if (const auto nm = names.find(id); nm != names.end()) {  // (the name: 64 bytes at +0x10)
      uint8_t buf[64] = {};
      std::memcpy(buf, nm->second.data(), std::min<size_t>(nm->second.size(), 63));
      if (std::memcmp(r + 0x10, buf, 64)) std::memcpy(r + 0x10, buf, 64), ++set;
    }
    const auto f = waze.find(id);
    if (f != waze.end() && std::memcmp(r, f->second.data(), 16)) std::memcpy(r, f->second.data(), 16), ++set;
  }
  uint32_t count[128] = {};
  for (uint32_t k = 0; k < records; ++k)
    for (int c = 0; c < 128; ++c)
      if (w[272 + size_t(k) * 160 + (c >> 3)] >> (c & 7) & 1) ++count[c];
  *fixed = 0;
  for (int c = 0; c < 128; ++c) {
    const uint16_t n = uint16_t(std::min<uint32_t>(count[c], 0xFFFF));
    *fixed += Le16(w + 8 + 2 * c) != n;
    PutLe16(w + 8 + 2 * c, n);
  }
  return set;
}

std::map<uint16_t, Bytes> g_waze;  // the packs' moves' category bits (PatchWaze at each MOVS/WAZE load)
std::map<uint16_t, std::string> g_names;  // and their record names
std::map<uint16_t, uint16_t> g_copies;     // and records copied from other moves

bool BuildMisc(const fs::path& game, const fs::path& overlay, const Packs& p) {
  const fs::path src = game / "pac" / "misc.pac";
  std::map<std::string, Bytes> changed;
  if (!p.waze.empty() || !p.names.empty() || !p.copies.empty()) {
    Bytes blob;
    if (!ReadEntry(src, "MOVS/WAZE", blob)) return false;
    const bool bpe = svrfmt::IsBpe(blob);
    Bytes w = svrfmt::Unpack(blob);
    int fixed = 0;
    const int set = PatchWaze(w.data(), w.size(), p.waze, &fixed, p.names, p.copies);
    if (set < 0) return false;
    changed["MOVS/WAZE"] = Repack(w, bpe);
    REXLOG_INFO("[svr2011] move packs: {} move records made selectable, {} category counts updated", set, fixed);
  }
  if (!p.exh.empty() || !p.evt.empty() || !p.mbd.empty()) {
    Bytes waza;
    if (!ReadEntry(src, "WAZA/DATA", waza)) return false;
    Bytes c[3];
    {
      const bool bpe = svrfmt::IsBpe(waza);
      std::vector<svrfmt::PachEntry> kids;
      if (!svrfmt::PachRead(svrfmt::Unpack(waza), kids)) return false;
      for (auto& k : kids)
        if (k.id < 3) {
          if (!EditGroups(k.data, k.id == 0 ? p.exh : k.id == 1 ? p.evt : p.mbd, int(k.id))) return false;
          c[k.id] = k.data;
        }
      changed["WAZA/DATA"] = Repack(svrfmt::PachWrite(kids), bpe);
    }
    for (const char* nm : {"BATS/INIT", "BATH/INIT"}) {  // (the same three tables, children 10-12)
      Bytes blob;
      if (!ReadEntry(src, nm, blob)) continue;
      const bool bpe = svrfmt::IsBpe(blob);
      std::vector<svrfmt::PachEntry> kids;
      if (!svrfmt::PachRead(svrfmt::Unpack(blob), kids)) continue;
      for (auto& k : kids)
        if (k.id >= 10 && k.id <= 12 && !c[k.id - 10].empty()) k.data = c[k.id - 10];
      changed[nm] = Repack(svrfmt::PachWrite(kids), bpe);
    }
  }
  return WritePac(src, overlay / "misc.pac", changed);
}

std::string Lower(std::string s) {
  for (char& ch : s) ch = char(std::tolower(uint8_t(ch)));
  return s;
}

// pac/plist360(_4x3).h with the merged pacs named in the overlay.
bool WriteList(const fs::path& game, const fs::path& overlay, const std::string& name, const std::set<std::string>& pacs,
               const std::vector<std::string>& extra = {}) {
  std::ifstream in(game / "pac" / name, std::ios::binary);
  if (!in) return true;  // (the 4:3 list may not exist)
  std::ostringstream out;
  std::string line;
  while (std::getline(in, line)) {
    const bool cr = !line.empty() && line.back() == '\r';
    if (cr) line.pop_back();
    const std::string l = Lower(line);
    for (const auto& p : pacs)
      if (l == "pac\\" + p) line = "mods\\pacoverlay\\" + p;
    out << line << (cr ? "\r\n" : "\n");
  }
  for (const auto& e : extra) out << e << "\r\n";
  std::ofstream o(overlay / name, std::ios::binary | std::ios::trunc);
  o << out.str();
  return bool(o);
}

// The game folder's file device indexed the folder at start-up, before the
// overlay was made: the game opens the pacs through directory handles, which
// only walk that index (no lookup on the disk), so files made now would not
// be found - an endless loading screen - or would keep a size from before.
// Resolving each through the file system adds it to the index; update()
// refreshes a size it already had.
void Register(rex::filesystem::VirtualFileSystem* vfs, const fs::path& overlay) {
  if (!vfs) return;
  std::error_code ec;
  for (const auto& e : fs::recursive_directory_iterator(overlay, ec)) {  // (entry pacs sit in evt\)
    if (!e.is_regular_file()) continue;
    std::string rel = fs::relative(e.path(), overlay, ec).string();
    std::replace(rel.begin(), rel.end(), '/', '\\');
    const std::string path = "\\Device\\Harddisk0\\Partition1\\Mods\\PacOverlay\\" + rel;
    if (auto* entry = vfs->ResolvePath(path)) entry->update();
    else REXLOG_WARN("[svr2011] move packs: the game can't see {}", path);
  }
}

// pack.txt by its content (FNV-1a): a same-size edit keeps its size, and a
// copied pack can keep an old time.
std::string ContentStamp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  uint64_t h = 1469598103934665603ull;
  char c;
  while (in.get(c)) h = (h ^ uint8_t(c)) * 1099511628211ull;
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
  return p.filename().string() + " " + hex + "\n";
}

std::string FileStamp(const fs::path& p) {
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  const auto time = fs::last_write_time(p, ec).time_since_epoch().count();
  return p.filename().string() + " " + std::to_string(static_cast<unsigned long long>(size)) + " " + std::to_string(static_cast<long long>(time)) + "\n";
}

}  // namespace

namespace svr2011 {

const std::string& PacListFolder() { return g_folder; }

// A backstage mod with its own row and gimmicks (Mods/Backstage/<id>:
// manifest row= and gimmick=<entry>, gimmick.pac = a small pac holding only
// GMGB/<entry>; arena_mods.cpp) - the first one by folder name, else empty.
// It is mounted as one more pac: its line goes at the end of the overlay pac
// list (pacs mount in list order and a lookup takes the first match, so the
// game's own entries stay as they are). (File-level links aren't followed by
// the game's opens, only folder links: a gm.pac served in place of
// pac\gm.pac never was.)
constexpr const char* kGimmickPac = "bsgimmick.pac";
fs::path BackstageGimmickPac(const fs::path& game) {
  std::error_code ec;
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(game / "Mods" / "Backstage", ec))
    if (e.is_directory() && !fs::exists(e.path() / "disabled", ec) && fs::exists(e.path() / "gimmick.pac", ec) &&
        fs::exists(e.path() / "arena.pac", ec))
      dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  for (const auto& d : dirs) {
    std::ifstream m(d / "manifest.txt", std::ios::binary);
    std::string line;
    bool row = false, gimmick = false;
    while (std::getline(m, line)) {
      row |= line.rfind("row=", 0) == 0;
      gimmick |= line.rfind("gimmick=", 0) == 0;
    }
    if (row && gimmick) return d / "gimmick.pac";
  }
  return {};
}

void LoadMoveTables(const fs::path& game);  // (below: the move tables for a match's fallback, cam_mod_moves.cpp)

// ---- The entrances the packs add (CREATE AN ENTRANCE's MODS rows, cae_mod_motions.cpp):
// pacentry lines EVP? <n> (n < 10000) are entrances, EVP? <10000 + 1000k + n> their CAE parts.
// The label: the superstar mod's name (manifest name=) for a mod's own moves/, else the script's
// own name (EVP +0x22) and the game it came from (the pack.txt's first line, as the converters
// write it: "# <game> entrance <n> as 2011 <m>").
namespace {
std::mutex g_ent_mutex;
std::vector<ModEntrance> g_mod_entrances;
std::set<int> g_mod_scripts;  // every EVP number the packs add (entrances and parts)

std::string ManifestName(const fs::path& mod) {
  std::ifstream in(mod / "manifest.txt");
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("name=", 0) == 0) return line.substr(5);
  }
  return mod.filename().string();
}

std::string ScriptName(const fs::path& evp) {  // (EVP +0x22: 30 bytes, NUL padded; '_' as space)
  std::ifstream in(evp, std::ios::binary);
  char buf[0x40] = {};
  if (!in.read(buf, sizeof(buf))) return "";
  std::string s(buf + 0x22, strnlen(buf + 0x22, 30));
  std::replace(s.begin(), s.end(), '_', ' ');
  return s;
}

std::string PackGame(const fs::path& dir) {  // ("# WWE '13 entrance 246 as 2011 497" -> "WWE '13")
  std::ifstream in(dir / "pack.txt");
  std::string line;
  if (!std::getline(in, line) || line.rfind("# ", 0) != 0) return "";
  const size_t e = line.find(" entrance ");
  return e == std::string::npos ? "" : line.substr(2, e - 2);
}
}  // namespace

void SetModEntrances(const std::vector<Packs::EntryLine>& lines) {
  std::map<int, ModEntrance> ents;
  std::set<int> scripts;
  std::map<fs::path, int> per_dir;  // (entrances per pack: a mod with several gets the script name too)
  for (const auto& l : lines) {
    if (l.group.rfind("EVP", 0) != 0 || l.name.empty() || l.name.find_first_not_of("0123456789") != std::string::npos)
      continue;
    const int n = std::atoi(l.name.c_str());
    scripts.insert(n);
    if (n < 10000 && !ents.count(n)) per_dir[l.dir]++;
  }
  for (const auto& l : lines) {
    if (l.group.rfind("EVP", 0) != 0 || l.name.empty() || l.name.find_first_not_of("0123456789") != std::string::npos)
      continue;
    const int n = std::atoi(l.name.c_str());
    if (n >= 10000 || ents.count(n)) continue;
    ModEntrance e;
    e.number = n;
    const std::string script = ScriptName(l.file);
    const bool mod_moves = l.dir.filename() == "moves" && l.dir.parent_path().parent_path().filename() == "Superstars";
    std::error_code ec;
    if (mod_moves || fs::exists(l.dir / "manifest.txt", ec)) {  // (a pack with a manifest: its name=, as a mod's - the
      // Mod Maker's Simple entrance porting names it after the superstar)
      e.label = ManifestName(mod_moves ? l.dir.parent_path() : l.dir);
      if (per_dir[l.dir] > 1 && !script.empty()) e.label += " - " + script;
    } else {
      const std::string game = PackGame(l.dir);
      e.label = script.empty() ? l.dir.filename().string() : script;
      if (!game.empty()) e.label += " (" + game + ")";
    }
    if (e.label.size() > 40) e.label.resize(40);
    for (int k = 0; k < 5; ++k)
      if (scripts.count(10000 + 1000 * k + n)) e.parts |= uint8_t(1u << k);
    ents[n] = e;
  }
  std::lock_guard lock(g_ent_mutex);
  g_mod_entrances.clear();
  for (auto& [n, e] : ents) g_mod_entrances.push_back(std::move(e));
  g_mod_scripts = std::move(scripts);
  if (!g_mod_entrances.empty()) REXLOG_INFO("[svr2011] move packs: {} mod entrances", g_mod_entrances.size());
}

std::vector<ModEntrance> ModEntrances() {
  std::lock_guard lock(g_ent_mutex);
  return g_mod_entrances;
}

bool EntranceScriptExists(int n) {
  // the game's own: the EVP groups of pac\evt\*.pac (EPK8 tables: groups {4CC, u16 count * 4, u16, u32},
  // 16-byte entries {name[8], sector, size}), read once
  static const std::set<int> stock = [] {
    std::set<int> s;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(rex::filesystem::GetExecutableFolder() / "pac" / "evt", ec)) {
      std::ifstream in(e.path(), std::ios::binary);
      Bytes head(0x4000);
      if (!in.read(reinterpret_cast<char*>(head.data()), 0x4000) || std::memcmp(head.data(), "EPK8", 4)) continue;
      for (size_t q = 0x800; q + 12 <= 0x4000 && Le32(&head[q]);) {
        const bool evp = !std::memcmp(&head[q], "EVP", 3);
        const uint32_t cnt = Le16(&head[q + 4]) / 4;
        q += 12;
        for (uint32_t i = 0; i < cnt && q + 16 <= 0x4000; ++i, q += 16) {
          const std::string nm(reinterpret_cast<char*>(&head[q]), 8);
          if (evp && std::isdigit(static_cast<unsigned char>(nm[0]))) s.insert(std::atoi(nm.c_str()));
        }
      }
    }
    REXLOG_INFO("[svr2011] move packs: {} stock entrance scripts", s.size());
    return s;
  }();
  if (stock.count(n)) return true;
  std::lock_guard lock(g_ent_mutex);
  return g_mod_scripts.count(n) != 0;
}

void InstallMovePacks(rex::filesystem::VirtualFileSystem* vfs) {
  if (const char* v = std::getenv("SVR2011_TEST_PLIST"); v && *v) {  // (test aid: a folder made by hand)
    g_folder = v;
    return;
  }
  const fs::path game = rex::filesystem::GetExecutableFolder();
  const fs::path overlay = game / "Mods" / "PacOverlay";
  std::error_code ec;
  // the packs: superstar mods' moves/ folders and Mods/Moves/<pack>
  std::vector<fs::path> dirs;
  // (attire mods replacing an attire the game's pac has: generated packs, a pacentry line each)
  PrepareAttirePacks(game);
  for (const fs::path& root : {game / "Mods" / "Superstars", game / "Mods" / "Moves",
                               game / "Mods" / "SuperstarOverlay" / "attirepacks"})
    for (const auto& e : fs::directory_iterator(root, ec)) {
      if (!e.is_directory() || fs::exists(e.path() / "disabled", ec)) continue;
      const fs::path d = root.filename() == "Superstars" ? e.path() / "moves" : e.path();
      if (fs::exists(d / "pack.txt", ec)) dirs.push_back(d);
    }
  std::sort(dirs.begin(), dirs.end());
  const fs::path gm = BackstageGimmickPac(game);
  if (dirs.empty() && gm.empty()) {
    for (const char* f : {"m.pac", "misc.pac", "mpsp.pac", "gm.pac", kGimmickPac, "plist360.h", "plist360_4x3.h", "stamp.txt"})
      fs::remove(overlay / f, ec);  // (nothing to serve: free the space)
    fs::remove_all(overlay / "evt", ec);
    LoadMoveTables(game);  // (a removed pack's moves in movesets: the match fallback still needs the tables)
    return;
  }
  Packs packs;
  std::string stamp;
  // (data false: the stamp and lists only; the motion files are read when merging)
  auto read_packs = [&](bool data) {
    packs = Packs{};
    stamp = std::string(kFormat) + "\n";
    for (const auto& d : dirs) {
      ReadPack(d, packs, stamp, data);
      stamp += ContentStamp(d / "pack.txt");
    }
    // (the game's own files too - in both reads, or the saved stamp never
    // matches the next start's and every start merges again)
    for (const char* f : {"m.pac", "misc.pac", "mpsp.pac", "plist360.h", "plist360_4x3.h"})
      stamp += FileStamp(game / "pac" / f);
    for (const auto& [pac, e] : packs.entries) stamp += FileStamp(game / "pac" / PacPath(pac));
    if (!gm.empty()) stamp += "gimmick " + gm.string() + " " + FileStamp(gm);
  };
  read_packs(false);
  SetModEntrances(packs.entry_lines);
  WarnOverrides(packs);
  std::set<std::string> pacs;
  for (const auto& [pac, e] : packs.motions) pacs.insert(Lower(pac));
  for (const auto& [pac, e] : packs.entries) pacs.insert(pac);  // ("evt\nyujyo5.pac": WriteList maps pac\evt\... to it)
  if (!packs.waze.empty() || !packs.names.empty() || !packs.copies.empty() || !packs.exh.empty() || !packs.evt.empty() ||
      !packs.mbd.empty())
    pacs.insert("misc.pac");
  g_waze = packs.waze;
  g_names = packs.names;
  g_copies = packs.copies;
  {
    std::ifstream old(overlay / "stamp.txt", std::ios::binary);
    std::string was((std::istreambuf_iterator<char>(old)), std::istreambuf_iterator<char>());
    bool have = was == stamp;
    for (const auto& p : pacs) have &= fs::exists(overlay / PacPath(p), ec);
    if (!gm.empty()) have &= fs::exists(overlay / kGimmickPac, ec);
    if (have) {
      Register(vfs, overlay);
      g_folder = "Mods\\PacOverlay";
      REXLOG_INFO("[svr2011] move packs: {} packs ({} items), overlay up to date", dirs.size(), packs.count);
      LoadMoveTables(game);
      return;
    }
  }
  REXLOG_INFO("[svr2011] move packs: merging {} packs ({} items) into {} ...", dirs.size(), packs.count, overlay.string());
  {
    const std::string checked = stamp;
    read_packs(true);  // (now the motion files too)
    if (stamp != checked) REXLOG_WARN("[svr2011] move packs: a pack changed while it was read");
  }
  fs::create_directories(overlay, ec);
  fs::remove(overlay / "stamp.txt", ec);
  bool ok = true;
  for (const auto& [pac, entries] : packs.motions) {
    svr2011::KeepWindowAlive();
    ok = ok && BuildMotionPac(game, overlay, Lower(pac), entries);
  }
  svr2011::KeepWindowAlive();
  if (ok && pacs.count("misc.pac")) ok = BuildMisc(game, overlay, packs);
  fs::remove_all(overlay / "evt", ec);  // (entry pacs: copies of the game's with the packs' entries)
  for (const auto& [pac, entries] : packs.entries) {
    svr2011::KeepWindowAlive();
    ok = ok && BuildEntryPac(game, overlay, pac, entries);
  }
  fs::remove(overlay / "gm.pac", ec);  // (an older build served a whole gm.pac)
  fs::remove(overlay / kGimmickPac, ec);
  std::vector<std::string> extra;
  if (ok && !gm.empty() && !fs::exists(overlay / kGimmickPac, ec)) {  // (a hard link to the mod's file, else a copy)
    fs::create_hard_link(gm, overlay / kGimmickPac, ec);
    if (ec) {
      ec.clear();
      // (never over an entry left in place: an old link would be written through)
      fs::copy_file(gm, overlay / kGimmickPac, fs::copy_options::none, ec);
    }
    ok = !ec;
    extra.push_back(std::string("mods\\pacoverlay\\") + kGimmickPac);
    REXLOG_INFO("[svr2011] move packs: backstage gimmicks from {}", gm.string());
  }
  ok = ok && WriteList(game, overlay, "plist360.h", pacs, extra) && WriteList(game, overlay, "plist360_4x3.h", pacs, extra);
  if (!ok) {
    REXLOG_WARN("[svr2011] move packs: merging failed - the game plays without them");
    LoadMoveTables(game);
    return;
  }
  std::ofstream(overlay / "stamp.txt", std::ios::binary) << stamp;
  Register(vfs, overlay);
  g_folder = "Mods\\PacOverlay";
  REXLOG_INFO("[svr2011] move packs: merged");
  LoadMoveTables(game);
}

}  // namespace svr2011

// MOVS/WAZE loaded (sub_8237ED68): sub_8237E708(data) swaps its header,
// then its records are swapped and the category lists built. The game can
// load another file's than the overlay's misc.pac (with the DLCs installed,
// one without the packs' moves - Jeff Hardy's ported finisher wasn't in
// CREATE A MOVESET's list): each is patched here, as the file still is.
namespace {
// The moves a moveset can name (MoveUsable): a record with category bits, as
// the last MOVS/WAZE loaded (after the packs' patch).
std::mutex g_usable_mutex;
std::vector<bool> g_usable;  // (empty: not loaded yet)
std::vector<uint16_t> g_defaults;  // category -> its fallback move (CategoryDefaultMove)
std::vector<bool> g_record, g_bits;  // a record with the id at all / with category bits (MoveMissing)
std::vector<bool> g_past_pack;       // ids a pack has given (Mods\PacOverlay\pack_moves.txt, kept across starts)
std::vector<bool> g_stock_ok;        // the stock game plays the id without packs (a record with bits, or EXH data)

// MoveUsable's list and CategoryDefaultMove's table from MOVS/WAZE records as in the file (the packs' patch
// applied): at start-up from misc.pac (a match never loads MOVS/WAZE itself - CREATE A MOVE-SET does) and
// again at each load (sub_8237E708).
void SetMoveTables(const uint8_t* w) {
  std::vector<bool> usable(65536, false), record(65536, false);
  for (uint32_t k = 0, n = Le32(w + 4); k < n; ++k) {
    const uint8_t* r = w + 272 + size_t(k) * 160;
    record[Le16(r + 0x90)] = true;
    if (std::any_of(r, r + 16, [](uint8_t b) { return b != 0; })) usable[Le16(r + 0x90)] = true;
  }
  std::vector<bool> bits = usable;
  // (a move pack's own moves count whatever the records say: Sabu's Arabian
  // Facebuster 6861 has 2011's motions and the pack's bits but no record of
  // its own in the file - it plays)
  for (const auto& [id, b] : g_waze) usable[id] = true;
  for (const auto& [id, from] : g_copies) usable[id] = true;
  for (const auto& [id, n] : g_names) usable[id] = true;
  // per category, the move a slot falls back to in a match when its own has no record (cam_mod_moves.cpp):
  // the first game move (not a pack's) of that category the game always shows (unlock 512 or 1-3), else
  // its first game move
  std::vector<uint16_t> defaults(128, 0), any(128, 0);
  for (uint32_t k = 0, n = Le32(w + 4); k < n; ++k) {
    const uint8_t* r = w + 272 + size_t(k) * 160;
    const uint16_t id = Le16(r + 0x90), unlock = Le16(r + 0x78);
    if (!id || g_waze.count(id) || g_copies.count(id) || g_names.count(id)) continue;
    const bool shown = unlock == 512 || (unlock >= 1 && unlock <= 3);
    for (int c = 0; c < 128; ++c) {
      if (!(r[c >> 3] >> (c & 7) & 1)) continue;
      if (!any[c]) any[c] = id;
      if (shown && !defaults[c]) defaults[c] = id;
    }
  }
  for (int c = 0; c < 128; ++c)
    if (!defaults[c]) defaults[c] = any[c];
  std::lock_guard lock(g_usable_mutex);
  g_usable = std::move(usable);
  g_defaults = std::move(defaults);
  g_record = std::move(record);
  g_bits = std::move(bits);
}
}  // namespace

namespace svr2011 {
void LoadMoveTables(const fs::path& game) {
  const fs::path overlay = game / "Mods" / "PacOverlay" / "misc.pac";
  std::error_code ec;
  const fs::path src = fs::exists(overlay, ec) ? overlay : game / "pac" / "misc.pac";
  Bytes blob;
  if (!ReadEntry(src, "MOVS/WAZE", blob)) {
    REXLOG_WARN("[svr2011] move packs: no MOVS/WAZE in {} - no match fallback for removed moves", src.string());
    return;
  }
  Bytes w = svrfmt::Unpack(blob);
  if (w.size() < 272 + 160 || std::memcmp(w.data() + 272 + 16, "* test motion *", 15)) return;
  int fixed = 0;
  PatchWaze(w.data(), w.size(), g_waze, &fixed, g_names, g_copies);  // (the overlay's is patched already: no change)
  SetMoveTables(w.data());
  // the ids packs have given so far (this start's added): a removed pack's move keeps a cut record without
  // bits, which stock movesets' moves can have too - only a past pack's id there is "missing"
  std::vector<bool> past(65536, false);
  const fs::path list = game / "Mods" / "PacOverlay" / "pack_moves.txt";
  {
    std::ifstream in(list);
    for (int id; in >> id;)
      if (id > 0 && id < 65536) past[id] = true;
  }
  bool added = false;
  auto mark = [&](uint16_t id) {
    if (!past[id]) past[id] = true, added = true;
  };
  for (const auto& [id, b] : g_waze) mark(id);
  for (const auto& [id, nm] : g_names) mark(id);
  for (const auto& [id, from] : g_copies) mark(id);
  if (added) {
    fs::create_directories(list.parent_path(), ec);
    std::ofstream out(list, std::ios::trunc);
    for (int id = 1; id < 65536; ++id)
      if (past[id]) out << id << '\n';
  }
  // what the stock game can play without packs: its own misc.pac's records with bits and EXH data
  std::vector<bool> stock_ok(65536, false);
  Bytes sw, sd;
  if (ReadEntry(game / "pac" / "misc.pac", "MOVS/WAZE", sw)) {
    const Bytes w0 = svrfmt::Unpack(sw);
    if (w0.size() >= 272)
      for (uint32_t k = 0, n = Le32(w0.data() + 4); k < n && 272 + size_t(k + 1) * 160 <= w0.size(); ++k) {
        const uint8_t* r = w0.data() + 272 + size_t(k) * 160;
        if (std::any_of(r, r + 16, [](uint8_t b) { return b != 0; })) stock_ok[Le16(r + 0x90)] = true;
      }
  }
  if (ReadEntry(game / "pac" / "misc.pac", "WAZA/DATA", sd)) {
    std::vector<svrfmt::PachEntry> kids, groups;
    if (svrfmt::PachRead(svrfmt::Unpack(sd), kids) && !kids.empty() && svrfmt::PachRead(svrfmt::Unpack(kids[0].data), groups))
      for (const auto& g : groups) {
        const Bytes t = svrfmt::Unpack(g.data);
        if (t.size() < 8 || std::memcmp(t.data(), "EXH", 3)) continue;
        for (size_t o = 8; o + 36 <= t.size(); o += 36) stock_ok[Le16(t.data() + o)] = true;
      }
  }
  {
    std::lock_guard lock(g_usable_mutex);
    g_past_pack = std::move(past);
    g_stock_ok = std::move(stock_ok);
  }
  REXLOG_INFO("[svr2011] move packs: move tables from {} ({} records)", src.filename().string(), Le32(w.data() + 4));
}
}  // namespace svr2011

namespace svr2011 {
// (g_waze / g_names / g_copies are set once at start-up, before the game runs)
bool PackMove(uint16_t id) { return g_waze.count(id) || g_names.count(id) || g_copies.count(id); }

uint16_t CategoryDefaultMove(int category) {
  std::lock_guard lock(g_usable_mutex);
  return category >= 0 && size_t(category) < g_defaults.size() ? g_defaults[category] : 0;
}

bool MoveMissing(uint16_t id) {
  // only a move a pack gave once, no pack gives now and the stock game can't play: stock movesets name moves
  // without records, bits or EXH data that play fine, so nothing wider can be trusted
  if (!id || PackMove(id)) return false;
  std::lock_guard lock(g_usable_mutex);
  if (g_past_pack.empty() || !g_past_pack[id]) return false;
  return g_stock_ok.empty() || !g_stock_ok[id];
}

int MoveUsable(uint16_t id) {
  std::lock_guard lock(g_usable_mutex);
  if (g_usable.empty()) return -1;
  return g_usable[id] ? 1 : 0;
}
}  // namespace svr2011

REX_EXTERN(__imp__sub_8237E708);
REX_HOOK_RAW(sub_8237E708) {
  const uint32_t data = ctx.r3.u32;
  static const bool off = [] { const char* v = std::getenv("SVR2011_TEST_WAZE_LOAD"); return v && *v == '0'; }();
  if (data && (!g_waze.empty() || !g_names.empty() || !g_copies.empty()) && !off) {
    uint8_t* w = base + data;
    const size_t size = 272 + size_t(Le32(w + 4)) * 160;
    int fixed = 0;
    const int set = Le32(w + 4) < 20000 ? PatchWaze(w, size, g_waze, &fixed, g_names, g_copies) : -1;
    static int logged = 0;
    if (logged++ < 4)
      REXLOG_INFO("[svr2011] move packs: MOVS/WAZE loaded ({} records): {} moves set, {} category counts updated",
                  Le32(w + 4), set, fixed);
  }
  if (data && Le32(base + data + 4) < 20000 && !std::memcmp(base + data + 272 + 16, "* test motion *", 15))
    SetMoveTables(base + data);  // (records still as in the file: id at +0x90, bits +0..+15)
  __imp__sub_8237E708(ctx, base);
}

// A moveset's move by slot (r3 slot, r4 moveset; sub_8237D4C8 looks it up in
// its category's list): NULL when the move isn't in that list, and these
// callers read the record without a check (+115 / +116: CREATE A MOVESET's
// signature / finisher slots crashed, read of 0x74, on a mod whose profile
// was its slot's placeholder - move 15271, which doesn't exist). They
// get record 0 instead ("* test motion *": its bytes there are 0, as the code
// takes for none, and it is in no category). Other callers check for NULL.
REX_EXTERN(__imp__sub_82379158);
REX_HOOK_RAW(sub_82379158) {
  const uint32_t lr = uint32_t(ctx.lr), slot = ctx.r3.u32, moveset = ctx.r4.u32;
  __imp__sub_82379158(ctx, base);
  if (ctx.r3.u32) return;
  switch (lr) {
    case 0x82B9E1DC: case 0x82BA69E4: case 0x82B9C2E4: case 0x823797FC: case 0x82BA6424: break;
    default: return;
  }
  auto rd = [&](uint32_t a) { const uint8_t* p = base + a; return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
  const uint32_t table = rd(0x82E3C214);
  const uint32_t first = table ? rd(table + 60) : 0;
  if (!first) return;
  ctx.r3.u64 = first;
  static int logged = 0;
  if (logged++ < 8) {
    const uint32_t id = moveset ? (uint32_t(base[moveset + (slot + 2) * 2]) << 8 | base[moveset + (slot + 2) * 2 + 1]) : 0;
    REXLOG_WARN("[svr2011] moveset: slot {} move {} not in its list (caller {:08X}) - none shown", slot, id, lr);
  }
}

// "Is this motion streamed (YMBs)": sub_82154F00(id) -> its byte in the game's
// motion flag table (*0x82DDBBE4, bound *0x82DDBBE0, loaded from a data file:
// set for the stock YMBs ids, 9000-9779). The motion code goes by it - 0 sends
// a YMBs motion down the keyframe path (sub_82309798 -> 82394360, sub_823B8F30):
// a pack's new YMBs taunt (3894, even with stock 9602's own data) held one
// pose its whole length. A pack's YMBs motion ids answer 1.
REX_EXTERN(__imp__sub_82154F00);
REX_HOOK_RAW(sub_82154F00) {
  const uint32_t id = ctx.r3.u32 & 0x7FFF;
  __imp__sub_82154F00(ctx, base);
  if ((ctx.r3.u32 & 0xFF) || !g_ymbs_motion[id].load(std::memory_order_relaxed)) return;
  ctx.r3.u64 = 1;
  static std::atomic<bool> told[0x8000] = {};
  if (!told[id].exchange(true)) REXLOG_INFO("[svr2011] move packs: motion {} streamed (YMBs, a pack's)", id);
}
