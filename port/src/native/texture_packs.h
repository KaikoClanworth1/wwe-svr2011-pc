// WWE SmackDown vs. Raw 2011 - texture dumps and texture packs (as Dolphin
// has them), for the native renderer's texture cache (textures.cpp).
//
// A texture is known by its content hash: XXH3 of its top level's blocks,
// untiled, as the GPU reads them (textures.cpp ContentHash). For DXT and
// A8R8G8B8 that is the top level of the PC DDS in the game's pac files, so
// the same texture hashes the same on every run, version and machine.
//
// Dumps (native_dump_textures, a launcher switch for pack makers): every
// texture shown is written once to "Texture Dumps\<owner>\" as a PNG named
// <owner>_<name>_<hash16>.png - the owner (pac file: ch118_a1, bg17,
// menuHD...) and the game's own texture name from its pac texture bundle,
// found by an index of the pac files built on a thread the first time and
// cached (UserData\cache\texture_names.txt). Loose DDS without a name in the
// pac get <owner>_e<entry id>; textures not in the files (made at run time)
// tex_<w>x<h>_<format>_<hash16>. Written before the index is ready, a file
// is renamed once it is.
//
// Packs (native_texture_packs, the launchers' Texture packs lists): folders
// in "Texture Packs\"; any file (any subfolder) whose name ends with _<hash16>
// (or is <hash16>) .png or .dds replaces that texture, at any resolution. The
// first enabled pack that has a hash wins. Files are read and decoded on a
// thread when the texture is first shown (it shows the game's own until
// then). PNGs get a box-filtered mip chain; a DDS (DXT1/3/5 or 32-bit
// uncompressed) keeps its own mips.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace svr2011::native::texture_packs {

enum class Format : uint8_t { kRGBA8, kBC1, kBC2, kBC3 };

struct Replacement {
  uint32_t width = 0, height = 0;
  Format format = Format::kRGBA8;
  // Each level tightly packed: rows of pixels (RGBA8) or of 4x4 blocks (BC).
  std::vector<std::vector<uint8_t>> levels;
};

// Reads the settings, indexes the enabled packs and starts the threads
// (once; the texture cache calls it).
void Start();
// Content hashes needed: packs with files enabled, or dumping.
bool Active();
bool Dumping();

enum class Lookup { kNone, kPending, kReady };
// kNone: no enabled pack has it (or its file can't be read); kPending: being
// read (asked for now if not yet); kReady: *out holds it (handed over once;
// asked again later it is read again).
Lookup Find(uint64_t hash, std::shared_ptr<const Replacement>* out);

// A texture shown (once per hash; later calls are ignored): its top level as
// RGBA8 rows, as it looks (the fetch constant's swizzle applied). `format`:
// a short tag for unnamed files (dxt1, dxt5, rgba...).
bool WantDump(uint64_t hash);  // not dumped yet (call before converting)
void Dump(uint64_t hash, uint32_t width, uint32_t height, std::vector<uint8_t> rgba, const char* format);

// PNG (8-bit RGBA, deflate) - the dumps' writer.
std::vector<uint8_t> EncodePng(const uint8_t* rgba, uint32_t width, uint32_t height);

}  // namespace svr2011::native::texture_packs
