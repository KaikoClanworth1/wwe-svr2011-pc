#define _CRT_SECURE_NO_WARNINGS
#include "pac.h"

#include <algorithm>
#include <cstdio>
#include <functional>

namespace svrfmt {

bool ReadFile(const std::string& path, Bytes& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? size_t(n) : 0);
  bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

bool WriteFile(const std::string& path, const Bytes& data) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  bool ok = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
  return std::fclose(f) == 0 && ok;
}

// ------------------------------------------------------------------ BPE

// The game decodes into a 4000-byte block buffer (every original block
// unpacks to at most 4000 bytes); larger blocks hang the arena load.
constexpr size_t kBpeBlock = 4000;

bool BpeDecode(const Bytes& in, Bytes& out) {
  if (!IsBpe(in)) return false;
  const uint32_t packed = Le32(&in[8]), unpacked = Le32(&in[12]);
  if (16 + size_t(packed) > in.size()) return false;
  out.clear();
  out.reserve(unpacked);
  size_t pos = 16;
  const size_t end = 16 + packed;
  uint8_t left[256], right[256], stack[256];
  while (pos < end) {
    for (int i = 0; i < 256; ++i) left[i] = uint8_t(i);
    int c = 0;
    int count = in[pos++];
    for (;;) {
      if (count > 127) { c += count - 127; count = 0; }
      if (c == 256) break;
      for (int i = 0; i <= count; ++i, ++c) {
        if (pos >= end) return false;
        left[c] = in[pos++];
        if (c != left[c]) right[c] = in[pos++];
      }
      if (c == 256) break;
      count = in[pos++];
    }
    if (pos + 2 > end) return false;
    size_t size = in[pos] | in[pos + 1] << 8;
    pos += 2;
    if (pos + size > end) return false;
    for (size_t k = 0; k < size; ++k) {
      int sp = 0;
      stack[sp++] = in[pos + k];
      while (sp) {
        uint8_t s = stack[--sp];
        if (s == left[s]) {
          out.push_back(s);
        } else {
          if (sp > 254) return false;
          stack[sp++] = right[s];
          stack[sp++] = left[s];
        }
      }
    }
    pos += size;
  }
  return out.size() == unpacked;
}

Bytes BpeEncodeStored(const Bytes& raw) {
  Bytes body;
  for (size_t at = 0; at < raw.size(); at += kBpeBlock) {
    size_t n = raw.size() - at < kBpeBlock ? raw.size() - at : kBpeBlock;
    body.push_back(255);  // skip 128 identity codes,
    body.push_back(128);  // code 128 is itself,
    body.push_back(254);  // skip the remaining 127
    body.push_back(uint8_t(n));
    body.push_back(uint8_t(n >> 8));
    App(body, &raw[at], n);
  }
  Bytes out = {'B', 'P', 'E', ' '};
  AppLe32(out, 0x100);
  AppLe32(out, uint32_t(body.size()));
  AppLe32(out, uint32_t(raw.size()));
  App(out, body);
  return out;
}

namespace {

// Pair table as Gage writes it: a byte > 127 skips (byte - 127) codes that
// stand for themselves and is followed by one entry; a byte <= 127 is
// followed by byte + 1 entries; an entry is left[c] (+ right[c] if a pair).
void AppendPairTable(Bytes& out, const uint8_t* left, const uint8_t* right) {
  int c = 0;
  while (c < 256) {
    int skip = 0;
    while (c + skip < 256 && left[c + skip] == c + skip && skip < 128) ++skip;
    if (skip) {
      out.push_back(uint8_t(127 + skip));
      c += skip;
      if (c == 256) break;
      out.push_back(left[c]);
      if (left[c] != c) out.push_back(right[c]);
      ++c;
      continue;
    }
    int run = 0;
    while (c + run < 256 && left[c + run] != c + run && run < 128) ++run;
    out.push_back(uint8_t(run - 1));
    for (int k = c; k < c + run; ++k) {
      out.push_back(left[k]);
      out.push_back(right[k]);
    }
    c += run;
  }
}

void CompressBlock(const uint8_t* src, size_t n, Bytes& out) {
  std::vector<uint8_t> buf(src, src + n), next;
  next.reserve(n);
  uint8_t left[256], right[256];
  bool used[256] = {};
  for (int i = 0; i < 256; ++i) { left[i] = uint8_t(i); right[i] = 0; }
  for (uint8_t b : buf) used[b] = true;
  std::vector<uint16_t> counts(65536);
  for (;;) {
    int code = -1;
    for (int k = 0; k < 256; ++k)
      if (!used[k]) { code = k; break; }
    if (code < 0 || buf.size() < 2) break;
    std::fill(counts.begin(), counts.end(), 0);
    int best = 0, best_pair = 0;
    for (size_t i = 0; i + 1 < buf.size(); ++i) {
      const int p = buf[i] << 8 | buf[i + 1];
      if (++counts[p] > best) { best = counts[p]; best_pair = p; }
    }
    if (best < 3) break;  // Gage: a pair must occur 3+ times to pay for its table entry
    const uint8_t a = uint8_t(best_pair >> 8), b = uint8_t(best_pair);
    next.clear();
    for (size_t i = 0; i < buf.size(); ++i) {
      if (i + 1 < buf.size() && buf[i] == a && buf[i + 1] == b) { next.push_back(uint8_t(code)); ++i; }
      else next.push_back(buf[i]);
    }
    buf.swap(next);
    left[code] = a;
    right[code] = b;
    used[code] = true;
  }
  AppendPairTable(out, left, right);
  out.push_back(uint8_t(buf.size()));
  out.push_back(uint8_t(buf.size() >> 8));
  App(out, buf.data(), buf.size());
}

}  // namespace

Bytes BpeEncode(const Bytes& raw) {
  Bytes body;
  for (size_t at = 0; at < raw.size(); at += kBpeBlock) {
    const size_t n = raw.size() - at < kBpeBlock ? raw.size() - at : kBpeBlock;
    CompressBlock(&raw[at], n, body);
  }
  Bytes out = {'B', 'P', 'E', ' '};
  AppLe32(out, 0x100);
  AppLe32(out, uint32_t(body.size()));
  AppLe32(out, uint32_t(raw.size()));
  App(out, body);
  return out;
}

Bytes Unpack(const Bytes& entry) {
  if (!IsBpe(entry)) return entry;
  Bytes out;
  if (!BpeDecode(entry, out)) return {};
  return out;
}

// ------------------------------------------------------------------ PACH

bool PachRead(const Bytes& in, std::vector<PachEntry>& out) {
  if (!IsPach(in)) return false;
  const uint32_t n = Le32(&in[4]);
  const size_t base = 8 + size_t(n) * 12;
  if (base > in.size()) return false;
  out.clear();
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = &in[8 + i * 12];
    const size_t off = base + Le32(e + 4), size = Le32(e + 8);
    if (off + size > in.size()) return false;
    out.push_back({Le32(e), Bytes(in.begin() + off, in.begin() + off + size)});
  }
  return true;
}

Bytes PachWrite(const std::vector<PachEntry>& entries) {
  Bytes table = {'P', 'A', 'C', 'H'};
  AppLe32(table, uint32_t(entries.size()));
  Bytes body;
  for (const auto& e : entries) {
    AppLe32(table, e.id);
    AppLe32(table, uint32_t(body.size()));
    AppLe32(table, uint32_t(e.data.size()));
    App(body, e.data);
    Pad(body, 4);
  }
  App(table, body);
  return table;
}

// ------------------------------------------------------------------ EPAC

bool EpacRead(const Bytes& in, Epac& out) {
  if (in.size() < 0x4000 || std::memcmp(in.data(), "EPAC", 4)) return false;
  out = {};
  out.header.assign(in.begin(), in.begin() + 0x800);
  size_t p = 0x800;
  while (p + 12 <= 0x4000) {
    if (!Le32(&in[p])) break;
    EpacGroup g;
    g.type.assign(reinterpret_cast<const char*>(&in[p]), 4);
    const uint32_t cnt = Le32(&in[p + 4]) / 3;
    p += 12;
    for (uint32_t i = 0; i < cnt; ++i, p += 12) {
      const size_t off = 0x4000 + size_t(Le32(&in[p + 4])) * 0x800;
      const size_t size = size_t(Le32(&in[p + 8])) * 0x100;
      if (off + size > in.size()) return false;
      g.entries.push_back({std::string(reinterpret_cast<const char*>(&in[p]), 4),
                           Bytes(in.begin() + off, in.begin() + off + size)});
    }
    out.groups.push_back(std::move(g));
  }
  const size_t data_end = 0x4000 + size_t(Le32(&in[8]));
  if (data_end <= in.size()) out.trailer.assign(in.begin() + data_end, in.end());
  return true;
}

Bytes EpacWrite(const Epac& e) {
  Bytes out(e.header);
  out.resize(0x800, 0);
  Bytes toc, body;
  for (const auto& g : e.groups) {
    AppName(toc, g.type, 4);
    AppLe32(toc, uint32_t(g.entries.size() * 3));
    AppLe32(toc, 0);
    for (const auto& en : g.entries) {
      Pad(body, 0x800);
      AppName(toc, en.name, 4);
      AppLe32(toc, uint32_t(body.size() / 0x800));
      AppLe32(toc, uint32_t((en.data.size() + 0xFF) / 0x100));
      App(body, en.data);
      Pad(body, 0x100);
    }
  }
  Pad(body, 0x800);
  toc.resize(0x3800, 0);
  App(out, toc);
  PutLe32(&out[8], uint32_t(body.size()));
  App(out, body);
  if (!e.trailer.empty()) App(out, e.trailer);
  else out.resize(out.size() + 0x800, 0);
  return out;
}

}  // namespace svrfmt
