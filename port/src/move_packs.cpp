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

#include <algorithm>
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

constexpr const char* kFormat = "movepacks 6";  // (2: banks whose sentinel points past the data; 3: WAZE category counts; 4: YMBs banks, record names; 6: WAZA records in id order)
std::string g_folder;  // PacListFolder

struct Motion {
  uint16_t id = 0;
  uint8_t x = 0, y = 0;
  uint32_t frames = 0;
  Bytes data;
  Bytes hdr;  // (YMBs: the 20-byte motion header)
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

void ReadPack(const fs::path& dir, Packs& p, std::string& stamp) {
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
      if (!ReadAll(dir / fs::u8path(file), m.data)) {
        REXLOG_WARN("[svr2011] move packs: {}: missing {}", dir.filename().string(), file);
        continue;
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
  std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return Key(a->id, a->x, a->y) < Key(b->id, b->x, b->y); });
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
  std::sort(sorted.begin(), sorted.end(), [](auto a, auto b) { return Key(a->id, a->x, a->y) < Key(b->id, b->x, b->y); });
  added = 0;
  for (const Motion* m : sorted) {
    const uint32_t k = Key(m->id, m->x, m->y);
    if (present.count(k) || m->hdr.size() != 20) continue;
    if (m->id >= ents.back().id) return false;  // (above the bank's sentinel)
    size_t pos = 0;
    while (pos < keys.size() && keys[pos] <= k) ++pos;
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
  out.insert(out.end(), raw.begin() + long(db), raw.begin() + long(db) + 4);
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
    if (const auto nm = names.find(Le16(r + 0x90)); nm != names.end()) {  // (the name: 64 bytes at +0x10)
      uint8_t buf[64] = {};
      std::memcpy(buf, nm->second.data(), std::min<size_t>(nm->second.size(), 63));
      if (std::memcmp(r + 0x10, buf, 64)) std::memcpy(r + 0x10, buf, 64), ++set;
    }
    const auto f = waze.find(Le16(r + 0x90));
    if (f == waze.end()) continue;
    if (std::memcmp(r, f->second.data(), 16)) std::memcpy(r, f->second.data(), 16), ++set;
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
  for (const auto& e : fs::directory_iterator(overlay, ec)) {
    if (!e.is_regular_file()) continue;
    const std::string path = "\\Device\\Harddisk0\\Partition1\\Mods\\PacOverlay\\" + e.path().filename().string();
    if (auto* entry = vfs->ResolvePath(path)) entry->update();
    else REXLOG_WARN("[svr2011] move packs: the game can't see {}", path);
  }
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
  for (const fs::path& root : {game / "Mods" / "Superstars", game / "Mods" / "Moves"})
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
    return;
  }
  Packs packs;
  std::string stamp = std::string(kFormat) + "\n";
  for (const auto& d : dirs) {
    ReadPack(d, packs, stamp);
    stamp += FileStamp(d / "pack.txt");
  }
  for (const char* f : {"m.pac", "misc.pac", "mpsp.pac", "plist360.h", "plist360_4x3.h"})
    stamp += FileStamp(game / "pac" / f);
  if (!gm.empty()) stamp += "gimmick " + gm.string() + " " + FileStamp(gm);
  std::set<std::string> pacs;
  for (const auto& [pac, e] : packs.motions) pacs.insert(Lower(pac));
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
    for (const auto& p : pacs) have &= fs::exists(overlay / p, ec);
    if (!gm.empty()) have &= fs::exists(overlay / kGimmickPac, ec);
    if (have) {
      Register(vfs, overlay);
      g_folder = "Mods\\PacOverlay";
      REXLOG_INFO("[svr2011] move packs: {} packs ({} items), overlay up to date", dirs.size(), packs.count);
      return;
    }
  }
  REXLOG_INFO("[svr2011] move packs: merging {} packs ({} items) into {} ...", dirs.size(), packs.count, overlay.string());
  fs::create_directories(overlay, ec);
  fs::remove(overlay / "stamp.txt", ec);
  bool ok = true;
  for (const auto& [pac, entries] : packs.motions)
    ok = ok && BuildMotionPac(game, overlay, Lower(pac), entries);
  if (ok && pacs.count("misc.pac")) ok = BuildMisc(game, overlay, packs);
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
    return;
  }
  std::ofstream(overlay / "stamp.txt", std::ios::binary) << stamp;
  Register(vfs, overlay);
  g_folder = "Mods\\PacOverlay";
  REXLOG_INFO("[svr2011] move packs: merged");
}

}  // namespace svr2011

// MOVS/WAZE loaded (sub_8237ED68): sub_8237E708(data) swaps its header,
// then its records are swapped and the category lists built. The game can
// load another file's than the overlay's misc.pac (with the DLCs installed,
// one without the packs' moves - Jeff Hardy's ported finisher wasn't in
// CREATE A MOVESET's list): each is patched here, as the file still is.
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
