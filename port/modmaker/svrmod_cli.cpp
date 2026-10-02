// svrmod: command-line front end of the Mod Maker's format library.
//   svrmod roundtrip <file.pac>...       EPAC/PACH/BPE/textures/JBOY self-test
//   svrmod export <bgNN.pac> <out dir>   arena -> arena.fbx + textures/*.png
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

#include "svrfmt/arena.h"
#include "svrfmt/arena_import.h"
#include "svrfmt/jboy.h"
#include "svrfmt/png.h"
#include "svrfmt/ring_kit.h"
#include "svrfmt/pac.h"
#include "svrfmt/texture.h"
#include "svrfmt/zip_write.h"

using namespace svrfmt;

namespace {

struct Counts {
  int files = 0, files_same = 0, jboy = 0, jboy_same = 0, bundles = 0, bundles_same = 0, dds = 0, dds_ok = 0;
};

void WalkPach(const Bytes& blob, Counts& c) {
  std::vector<PachEntry> ents;
  if (!PachRead(blob, ents)) return;
  for (const auto& e : ents) {
    const Bytes raw = Unpack(e.data);
    if (IsPach(raw)) { WalkPach(raw, c); continue; }
    if (IsJboy(raw)) {
      ++c.jboy;
      Model m, m2;
      std::string err;
      if (!JboyRead(raw, m, &err)) { std::printf("  JBOY %x: %s\n", e.id, err.c_str()); continue; }
      const Bytes w = JboyWrite(m);
      // compare by content: re-read and re-write must be stable, and
      // geometry must match the original read
      if (JboyRead(w, m2) && JboyWrite(m2) == w && m2.meshes.size() == m.meshes.size()) {
        bool same = true;
        for (size_t i = 0; i < m.meshes.size() && same; ++i) {
          const auto &a = m.meshes[i], &b = m2.meshes[i];
          same = a.verts.size() == b.verts.size() && a.strips.size() == b.strips.size() &&
                 !std::memcmp(a.verts.data(), b.verts.data(), a.verts.size() * sizeof(Vertex));
          for (size_t k = 0; same && k < a.strips.size(); ++k) same = a.strips[k].indices == b.strips[k].indices;
        }
        if (same) ++c.jboy_same;
        else std::printf("  JBOY %x: content differs\n", e.id);
      } else {
        std::printf("  JBOY %x: rewrite unstable\n", e.id);
      }
    } else if (IsTextureBundle(raw)) {
      ++c.bundles;
      std::vector<BundleTexture> texs;
      if (BundleRead(raw, texs)) {
        Bytes w = BundleWrite(texs);
        Bytes w2 = w;
        // originals sometimes stop without the final 16-byte pad
        if (w == raw || (w.size() > raw.size() && std::equal(raw.begin(), raw.end(), w.begin()))) ++c.bundles_same;
        for (const auto& t : texs) {
          ++c.dds;
          Image img;
          if (DdsDecode(t.data, img)) ++c.dds_ok;
        }
      }
    }
  }
}

int Roundtrip(int argc, char** argv) {
  Counts c;
  for (int i = 0; i < argc; ++i) {
    Bytes d;
    if (!ReadFile(argv[i], d)) { std::printf("%s: cannot read\n", argv[i]); continue; }
    Epac e;
    if (!EpacRead(d, e)) { std::printf("%s: not EPAC\n", argv[i]); continue; }
    ++c.files;
    if (EpacWrite(e) == d) ++c.files_same;
    else std::printf("%s: EPAC repack differs\n", argv[i]);
    for (const auto& g : e.groups)
      for (const auto& en : g.entries)
        if (IsPach(en.data)) WalkPach(en.data, c);
  }
  std::printf("EPAC %d/%d identical; JBOY %d/%d stable; bundles %d/%d identical; DDS decoded %d/%d\n",
              c.files_same, c.files, c.jboy_same, c.jboy, c.bundles_same, c.bundles, c.dds_ok, c.dds);
  return c.files_same == c.files && c.jboy_same == c.jboy && c.bundles_same == c.bundles && c.dds_ok == c.dds ? 0 : 1;
}

}  // namespace

int Export(const char* pac, const char* out) {
  Arena a;
  std::string err;
  if (!a.Load(pac, &err)) { std::printf("%s: %s\n", pac, err.c_str()); return 1; }
  ExportReport rep;
  const bool ok = ExportArena(a, out, pac, rep);
  std::printf("%s: %d models, %d meshes, %d triangles, %d textures (%d failed)\n", ok ? "exported" : "FAILED",
              rep.models, rep.meshes, rep.triangles, rep.textures, rep.textures_failed);
  for (const auto& w : rep.warnings) std::printf("  %s\n", w.c_str());
  return ok ? 0 : 1;
}

// Re-compress every BPE entry with our encoder: must decode back exactly;
// reports sizes against Yuke's. With an output path, writes the re-packed file.
int BpeTest(const char* pac, const char* out) {
  Bytes d;
  Epac e;
  if (!ReadFile(pac, d) || !EpacRead(d, e)) { std::printf("%s: cannot read\n", pac); return 1; }
  size_t theirs = 0, ours = 0, bad = 0;
  for (auto& g : e.groups)
    for (auto& en : g.entries) {
      std::vector<PachEntry> ents;
      if (!PachRead(en.data, ents)) continue;
      for (auto& pe : ents) {
        if (!IsBpe(pe.data)) continue;
        const Bytes raw = Unpack(pe.data);
        const Bytes enc = BpeEncode(raw);
        Bytes back;
        if (!BpeDecode(enc, back) || back != raw) ++bad;
        theirs += pe.data.size();
        ours += enc.size();
        pe.data = enc;
      }
      en.data = PachWrite(ents);
    }
  std::printf("%s: Yuke's %zu, ours %zu (%.1f%%), bad %zu\n", pac, theirs, ours, 100.0 * ours / theirs, bad);
  if (out) WriteFile(out, EpacWrite(e));
  return bad ? 1 : 0;
}

// Compress a raw file to a BPE entry (used by the Python lab tools).
int Bpe(const char* in, const char* out) {
  Bytes raw;
  if (!ReadFile(in, raw)) return 1;
  const Bytes enc = BpeEncode(raw);
  if (enc.size() >= raw.size()) std::printf("warning: packed %zu >= unpacked %zu\n", enc.size(), raw.size());
  return WriteFile(out, enc) ? 0 : 1;
}

int Import(const char* pac, const char* fbx, const char* out) {
  Arena a;
  std::string err;
  if (!a.Load(pac, &err)) { std::printf("%s: %s\n", pac, err.c_str()); return 1; }
  ImportOptions opt;
  ImportReport rep;
  const bool ok = ImportFbx(a, fbx, opt, rep);
  std::printf("%s: %d models changed, %d objects added (%d meshes), %d hidden, textures %d replaced %d added\n",
              ok ? "imported" : "FAILED", rep.models_changed, rep.objects_added, rep.meshes_added, rep.models_hidden,
              rep.textures_replaced, rep.textures_added);
  for (const auto& w : rep.warnings) std::printf("  warning: %s\n", w.c_str());
  for (const auto& e : rep.errors) std::printf("  error: %s\n", e.c_str());
  if (!ok) return 1;
  std::string serr;
  const Bytes data = a.Save(&serr);
  if (!serr.empty()) { std::printf("  error: %s\n", serr.c_str()); return 1; }
  if (!WriteFile(out, data)) return 1;
  std::printf("wrote %s (%zu bytes)\n", out, data.size());
  return 0;
}

// Ring Kit on its own: ring <bgNN.pac> <ring spec file> <out.pac>
// The spec file holds manifest lines (ring.ropes=1 0 1, ring.tints=..., ring.rope<N>_texture=<picture>).
bool LoadRingSpec(const char* path, RingSpec& spec) {
  Bytes b;
  if (!ReadFile(path, b)) { std::printf("%s: cannot read\n", path); return false; }
  const std::string text(b.begin(), b.end());
  spec.FromManifest(text);
  for (int r = 0; r < 3; ++r) {
    const std::string key = "ring.rope" + std::to_string(r) + "_texture=";
    const size_t at = text.find(key);
    if (at == std::string::npos) continue;
    size_t end = text.find_first_of("\r\n", at);
    spec.ropes[r].texture = text.substr(at + key.size(), end == std::string::npos ? end : end - at - key.size());
  }
  return true;
}

bool ApplyRingFile(Arena& a, const char* spec_path) {
  RingSpec spec;
  if (!LoadRingSpec(spec_path, spec)) return false;
  RingReport rep;
  const bool ok = ApplyRing(a, spec, rep);
  std::printf("ring: %d models added, %d changed, %d textures added\n", rep.models_added, rep.models_changed,
              rep.textures_added);
  for (const auto& w : rep.warnings) std::printf("  warning: %s\n", w.c_str());
  const auto reduced = a.FitFile({});
  if (!reduced.empty()) std::printf("  %zu textures halved to fit\n", reduced.size());
  return ok;
}

int Ring(const char* pac, const char* spec_path, const char* out) {
  Arena a;
  std::string err;
  if (!a.Load(pac, &err)) { std::printf("%s: %s\n", pac, err.c_str()); return 1; }
  if (!ApplyRingFile(a, spec_path)) return 1;
  const Bytes data = a.Save(&err);
  if (!err.empty()) { std::printf("error: %s\n", err.c_str()); return 1; }
  if (!WriteFile(out, data)) return 1;
  std::printf("wrote %s (%zu bytes, shipped %zu)\n", out, data.size(), a.original_file);
  return 0;
}

// The Mod Maker's "Save as mod" without its window (tests):
// makemod <bgNN.pac> <edited.fbx | -> <banner picture> <name> <out.svrmod> [ring spec file]
int MakeMod(const char* pac, const char* fbx, const char* banner, const char* name, const char* out,
            const char* ring = nullptr) {
  Arena a;
  std::string err;
  if (!a.Load(pac, &err)) { std::printf("%s: %s\n", pac, err.c_str()); return 1; }
  if (std::strcmp(fbx, "-")) {
    ImportOptions opt;
    ImportReport rep;
    if (!ImportFbx(a, fbx, opt, rep)) {
      for (const auto& e : rep.errors) std::printf("error: %s\n", e.c_str());
      return 1;
    }
  }
  RingSpec spec;
  if (ring) {
    if (!ApplyRingFile(a, ring)) return 1;
    LoadRingSpec(ring, spec);
  }
  const Bytes data = a.Save(&err);
  if (!err.empty()) { std::printf("error: %s\n", err.c_str()); return 1; }
  Image img;
  if (!LoadImageFile(banner, img)) { std::printf("%s: cannot read\n", banner); return 1; }
  std::string id;
  for (const char* c = name; *c; ++c)
    if (std::isalnum(static_cast<unsigned char>(*c))) id.push_back(char(std::tolower(static_cast<unsigned char>(*c))));
    else if (!id.empty() && id.back() != '_') id.push_back('_');
  const std::string manifest =
      std::string("type=arena\nid=") + id + "\nname=" + name + "\nauthor=test\nversion=1.0\n" +
      (ring ? spec.ManifestLines() : std::string());
  std::vector<ZipEntry> files = {{"manifest.txt", Bytes(manifest.begin(), manifest.end())},
                                 {"arena.pac", data},
                                 {"banner.dds", DdsEncode(Resize(img, 256, 128), DxtFormat::kDxt5, false)}};
  if (!WriteFile(out, ZipWrite(files))) return 1;
  std::printf("wrote %s (id %s)\n", out, id.c_str());
  return 0;
}

int main(int argc, char** argv) {
  if ((argc == 7 || argc == 8) && !std::strcmp(argv[1], "makemod"))
    return MakeMod(argv[2], argv[3], argv[4], argv[5], argv[6], argc == 8 ? argv[7] : nullptr);
  if (argc == 5 && !std::strcmp(argv[1], "ring")) return Ring(argv[2], argv[3], argv[4]);
  if (argc == 5 && !std::strcmp(argv[1], "import")) return Import(argv[2], argv[3], argv[4]);
  if (argc == 4 && !std::strcmp(argv[1], "bpe")) return Bpe(argv[2], argv[3]);
  if (argc >= 3 && !std::strcmp(argv[1], "bpetest")) return BpeTest(argv[2], argc >= 4 ? argv[3] : nullptr);
  if (argc >= 3 && !std::strcmp(argv[1], "roundtrip")) return Roundtrip(argc - 2, argv + 2);
  if (argc == 4 && !std::strcmp(argv[1], "export")) return Export(argv[2], argv[3]);
  if (argc == 4 && !std::strcmp(argv[1], "banner")) {
    // any picture -> the arena select banner: 256 x 128 DXT5, one level
    Image img;
    if (!LoadImageFile(argv[2], img)) { std::printf("%s: cannot read\n", argv[2]); return 1; }
    return WriteFile(argv[3], DdsEncode(Resize(img, 256, 128), DxtFormat::kDxt5, false)) ? 0 : 1;
  }
  if (argc == 6 && !std::strcmp(argv[1], "dds")) {
    // dds <picture> <w> <h> <out.dds>: DXT5, one level (VS screen pictures)
    Image img;
    if (!LoadImageFile(argv[2], img)) { std::printf("%s: cannot read\n", argv[2]); return 1; }
    return WriteFile(argv[5], DdsEncode(Resize(img, std::atoi(argv[3]), std::atoi(argv[4])), DxtFormat::kDxt5, false))
               ? 0 : 1;
  }
  if (argc == 5 && !std::strcmp(argv[1], "retexture")) {
    // retexture <bgNN.pac> <map.txt> <out.pac>: lines "<arena texture>=<picture>"
    // (a picture at its own size, power of two, in the texture's format and
    // mip layout); then FitFile and a size report
    Arena a;
    std::string err;
    if (!a.Load(argv[2], &err)) { std::printf("%s: %s\n", argv[2], err.c_str()); return 1; }
    Bytes mapb;
    if (!ReadFile(argv[3], mapb)) { std::printf("%s: cannot read\n", argv[3]); return 1; }
    std::string map(mapb.begin(), mapb.end()), line;
    size_t pos = 0, done = 0, missing = 0, before = 0, after = 0;
    std::vector<std::string> keep;
    while (pos < map.size()) {
      size_t nl = map.find('\n', pos);
      line = map.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
      pos = nl == std::string::npos ? map.size() : nl + 1;
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
      const size_t eq = line.find('=');
      if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
      const std::string name = line.substr(0, eq), pic = line.substr(eq + 1);
      bool found = false;
      for (auto& b : a.bundles)
        for (auto& t : b.textures) {
          if (t.name != name) continue;
          found = true;
          DdsInfo info;
          Image img;
          if (!DdsInfoOf(t.data, info) || !LoadImageFile(pic, img)) { std::printf("  %s: cannot use %s\n", name.c_str(), pic.c_str()); continue; }
          auto p2 = [](int v) { int p = 4; while (p < v && p < 2048) p *= 2; return p; };
          img = Resize(img, p2(img.w), p2(img.h));
          const DxtFormat f = info.format == DxtFormat::kArgb ? DxtFormat::kDxt5 : info.format;
          before += t.data.size();
          t.data = DdsEncode(img, f, info.mips > 1);
          after += t.data.size();
          b.changed = true;
          keep.push_back(name);
          ++done;
        }
      if (!found) { std::printf("  no texture %s in the arena\n", name.c_str()); ++missing; }
    }
    std::printf("retextured %zu (%zu not found): %.2f MB -> %.2f MB\n", done, missing, before / 1048576.0, after / 1048576.0);
    const auto halved = a.FitFile(keep);  // (the arena's own textures give way first)
    if (!halved.empty()) {
      std::printf("  over the room: %zu textures halved to fit:", halved.size());
      for (const auto& h : halved) std::printf(" %s", h.c_str());
      std::printf("\n");
    }
    const Bytes data = a.Save(&err);
    if (!err.empty()) { std::printf("error: %s\n", err.c_str()); return 1; }
    WriteFile(argv[4], data);
    std::printf("wrote %s: %.2f MB (shipped %.2f MB)\n", argv[4], data.size() / 1048576.0, a.original_file / 1048576.0);
    return 0;
  }
  if (argc == 5 && !std::strcmp(argv[1], "grow")) {
    // test: grow <bgNN.pac> <out.pac> <KB>: an unused texture of about that
    // many KB unpacked (a soft pattern, so it packs well) in the first set
    Arena a;
    std::string err;
    if (!a.Load(argv[2], &err) || a.bundles.empty()) { std::printf("%s: %s\n", argv[2], err.c_str()); return 1; }
    const int kb = std::atoi(argv[4]);
    int side = 64;
    while (side * side * 4 / 3 < kb * 1024 && side < 4096) side *= 2;  // DXT5: 1 byte per pixel (+ mips)
    Image img;
    img.w = img.h = side;
    img.rgba.resize(size_t(side) * side * 4);
    for (int y = 0; y < side; ++y)
      for (int x = 0; x < side; ++x) {
        uint8_t* p = &img.rgba[(size_t(y) * side + x) * 4];
        p[0] = uint8_t(x * 255 / side), p[1] = uint8_t(y * 255 / side), p[2] = uint8_t((x ^ y) & 0x40), p[3] = 255;
      }
    a.bundles[0].textures.push_back({"zz_grow", "dds", DdsEncode(img, DxtFormat::kDxt5, true)});
    a.bundles[0].changed = true;
    const Bytes data = a.Save(&err);
    if (!err.empty()) { std::printf("error: %s\n", err.c_str()); return 1; }
    WriteFile(argv[3], data);
    std::printf("wrote %s: %zu bytes (shipped %zu), texture %dx%d\n", argv[3], data.size(), a.original_file, side, side);
    return 0;
  }
  std::printf("usage: svrmod roundtrip <file.pac>... | export <bgNN.pac> <out dir> | import <bgNN.pac> <fbx> <out.pac>\n"
              "       | banner <picture> <out.dds> | bpe <raw> <out> | bpetest <file.pac> [out.pac]\n");
  return 2;
}
