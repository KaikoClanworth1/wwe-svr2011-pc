// Arena lighting options - see arena_lights.h.
//
// An arena's lights are its entry 0x7530: a YMKs bank (little-endian, as in
// the file) of light sets - "YMKs" header (16 bytes), event-size table T[256],
// u32 count at 0x110, count x 16-byte directory records {u8 y, u8 x, u16 set,
// u32 offset, u32 frames, u32 rt} from 0x114, then the key streams (offsets
// from the end of the directory). A set's channels (y): 0 / 1 ground / sky
// colour, 10-13 directional lights, 50-59 the crowd's moving lights (set 20).
// A key record: an optional event block (first byte with bit 7: that many
// bytes follow), then the 25-byte pose {u8 count, f32 x3 position, s16 x3
// rotation, u16 x3 colour / 4096}. In a match sets 10-14 light the wrestlers
// and the ring; on the entrance stage and ramp the light zones (0xC353) switch
// them to sets 200-202: set 10 plus a coloured key light (y 12). Arena models
// take their own set (0xC351 l(n); 100 without one) in both.
//
// The bank is changed in place as it is unpacked (sub_826AEF10, before the
// arena loader sub_82183B38 reads it): every colour key of the ground, sky and
// directional channels scaled (ARENA BRIGHTNESS x the arena's light_scale=),
// and, with match_light=, sets 10-14's directory records pointed at set 200's
// streams (same size: nothing moves).
#include "arena_lights.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(arena_brightness, 100, "GPU",
                     "Arena brightness, percent (50-200): the arenas' lights - the wrestlers, the ring and the hall");

namespace svr2011 {
namespace {

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
void PutLe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
void PutLe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24);
}

bool LightChannel(uint8_t y) { return y <= 1 || (y >= 10 && y <= 14) || (y >= 50 && y <= 59); }
bool LitChannel(uint8_t y) { return y <= 1 || (y >= 10 && y <= 13); }  // (ground, sky, directional: scaled)

// A key `value=` of a manifest text (empty: none)
std::string Key(const std::string& text, const char* key) {
  const std::string k = std::string(key) + "=";
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    if (text.compare(at, k.size(), k) == 0) {
      std::string v = text.substr(at + k.size(), end - at - k.size());
      while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\t')) v.pop_back();
      return v;
    }
    at = end + 1;
  }
  return {};
}

// Scales the colours of the stream at [p, end) -> keys changed
int ScaleStream(uint8_t* p, const uint8_t* end, float k) {
  int keys = 0;
  while (p < end) {
    const uint8_t c = *p;
    if (c < 0x60) {  // (frames skipped)
      ++p;
      continue;
    }
    size_t len;
    if (c == 0x60) {
      if (p + 3 > end) break;
      len = Le16(p + 1), p += 3;
    } else {
      len = c - 0x60, p += 1;
    }
    if (p + len > end) break;
    uint8_t* r = p;
    size_t rl = len;
    if (rl && (r[0] & 0x80)) {
      const size_t ev = r[0] & 0x7F;
      if (ev + 1 <= rl) r += 1 + ev, rl -= 1 + ev;
      else rl = 0;
    }
    if (rl >= 25) {
      for (int i = 0; i < 3; ++i) {
        const float v = std::round(Le16(r + 19 + 2 * i) * k);
        PutLe16(r + 19 + 2 * i, uint16_t(std::clamp(v, 0.0f, 65535.0f)));
      }
      ++keys;
    }
    p += len;
  }
  return keys;
}

}  // namespace

int ArenaBrightness() { return std::clamp(int(REXCVAR_GET(arena_brightness)), 50, 200); }

void SetArenaBrightness(int percent) {
  rex::cvar::SetFlagByName("arena_brightness", std::to_string(std::clamp(percent, 50, 200)));
}

void ArenaLightsDecoded(uint8_t* d, uint32_t size, const std::string& manifest) {
  if (size < 0x114 + 16 || std::memcmp(d, "YMKs", 4)) return;
  const uint32_t n = Le32(d + 0x110);
  const uint64_t data = 0x114 + 16ull * n;
  if (n < 8 || n > 4096 || data > size) return;
  // (a light bank: light channels only, and the sets the game needs - not a motion bank)
  bool has10 = false, has100 = false;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = d + 0x114 + 16 * i;
    if (!LightChannel(r[0])) return;
    has10 = has10 || Le16(r + 2) == 10;
    has100 = has100 || Le16(r + 2) == 100;
  }
  if (!has10 || !has100) return;
  const uint32_t dl = uint32_t(size - data);
  // match_light=: sets 10-14 take set 200's channels (its key light y 12 only with _spot)
  const std::string match = Key(manifest, "match_light");
  int pointed = 0;
  if (match == "entrance" || match == "entrance_spot") {
    const bool spot = match == "entrance_spot";
    for (uint32_t i = 0; i < n; ++i) {
      uint8_t* r = d + 0x114 + 16 * i;
      const uint16_t id = Le16(r + 2);
      if (id < 10 || id > 14 || !LitChannel(r[0]) || (r[0] == 12 && !spot)) continue;
      for (uint32_t j = 0; j < n; ++j) {
        const uint8_t* s = d + 0x114 + 16 * j;
        if (Le16(s + 2) != 200 || s[0] != r[0] || s[1] != r[1]) continue;
        std::memcpy(r + 4, s + 4, 12);  // (offset, frames, rt)
        ++pointed;
        break;
      }
    }
    if (!pointed) REXLOG_WARN("[svr2011] arena lights: match_light={} but the arena has no set 200", match);
  }
  // the scale: ARENA BRIGHTNESS x light_scale=
  float k = ArenaBrightness() / 100.0f;
  if (const std::string s = Key(manifest, "light_scale"); !s.empty()) {
    const float v = float(std::atof(s.c_str()));
    if (v > 0) k *= std::clamp(v, 0.25f, 4.0f);
  }
  int keys = 0;
  if (std::fabs(k - 1.0f) > 0.001f) {
    std::vector<uint32_t> offs;  // (every stream's start: one ends where the next begins)
    for (uint32_t i = 0; i < n; ++i) offs.push_back(Le32(d + 0x114 + 16 * i + 4));
    std::sort(offs.begin(), offs.end());
    offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
    std::vector<uint32_t> done;  // (streams shared by records: scaled once)
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* r = d + 0x114 + 16 * i;
      const uint32_t off = Le32(r + 4);
      if (!LitChannel(r[0]) || off >= dl || std::find(done.begin(), done.end(), off) != done.end()) continue;
      done.push_back(off);
      const auto nx = std::upper_bound(offs.begin(), offs.end(), off);
      const uint32_t end = nx == offs.end() || *nx > dl ? dl : *nx;
      keys += ScaleStream(d + data + off, d + data + end, k);
    }
  }
  if (pointed || keys)
    REXLOG_INFO("[svr2011] arena lights: x{:.2f} ({} keys){}", k, keys,
                pointed ? " - the match lit as the entrance (" + std::to_string(pointed) + " channels)" : std::string());
}

}  // namespace svr2011
