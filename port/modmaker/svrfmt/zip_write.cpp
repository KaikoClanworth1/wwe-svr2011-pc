#include "zip_write.h"

namespace svrfmt {

namespace {

uint32_t Crc32(const Bytes& d) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (uint8_t b : d) c = table[(c ^ b) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void Le16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }

}  // namespace

Bytes ZipWrite(const std::vector<ZipEntry>& entries) {
  Bytes out, central;
  for (const auto& e : entries) {
    const uint32_t crc = Crc32(e.data), size = uint32_t(e.data.size()), offset = uint32_t(out.size());
    // local file header
    AppLe32(out, 0x04034B50);
    Le16(out, 20); Le16(out, 0x0800); Le16(out, 0);  // version, flags (UTF-8 names), stored
    Le16(out, 0); Le16(out, 0x21);                    // time, date (1980-01-01)
    AppLe32(out, crc); AppLe32(out, size); AppLe32(out, size);
    Le16(out, uint16_t(e.name.size())); Le16(out, 0);
    App(out, reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size());
    App(out, e.data);
    // central directory record
    AppLe32(central, 0x02014B50);
    Le16(central, 20); Le16(central, 20); Le16(central, 0x0800); Le16(central, 0);
    Le16(central, 0); Le16(central, 0x21);
    AppLe32(central, crc); AppLe32(central, size); AppLe32(central, size);
    Le16(central, uint16_t(e.name.size())); Le16(central, 0); Le16(central, 0);
    Le16(central, 0); Le16(central, 0); AppLe32(central, 0);
    AppLe32(central, offset);
    App(central, reinterpret_cast<const uint8_t*>(e.name.data()), e.name.size());
  }
  const uint32_t cd_offset = uint32_t(out.size()), cd_size = uint32_t(central.size());
  App(out, central);
  AppLe32(out, 0x06054B50);
  Le16(out, 0); Le16(out, 0);
  Le16(out, uint16_t(entries.size())); Le16(out, uint16_t(entries.size()));
  AppLe32(out, cd_size); AppLe32(out, cd_offset);
  Le16(out, 0);
  return out;
}

}  // namespace svrfmt
