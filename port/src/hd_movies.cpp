// WWE SmackDown vs. Raw 2011 - HD entrance movies (see hd_movies.h).
//
// The game's movies are Bink (recompiled guest code, 0x82D4xxxx): BinkOpen
// (sub_82D49B80(name, flags)) reads the header, the movie's textures are made
// at its size (one 8-bit texture per Y / cR / cB plane, the colour in a
// shader; titantron movies are 320 x 320, the game's own go to 1024 x 1024),
// and Bink decodes each frame straight into one of two frame buffer sets. A
// movie object (sub_82592B90(movie) steps it: BinkDoFrame) has at +12 the
// Bink (+8 frames, +12 the frame number, from 1) and at +32 the frame buffer
// sets: +132 / +136 the Y plane's width / height, +140 / +144 the cR / cB
// planes', +148 the set just decoded, then per set (48 bytes from +152) the
// Y, cR, cB planes' {allocate, buffer, pitch} (sub_825930F8 copies that set
// to the textures' own at +36 for the screens).
//
// An HD movie is an .mp4: the game opens a stub .bik of its size and length
// (every frame "all skip": nearly free to decode) and after each step the
// decoder's frame for that time is written over the set Bink just decoded.
// Size: the mod's .bik fallback's (320 / 640 / 1024), else 1024 - the
// game's own movie layout at that size (the big screen the top size*220/320
// rows, shown at 16:9, the stage strip below): a square .mp4 is taken as
// laid out so, any other fills the big screen (stretched to it) over a black
// strip.
//
// Windows: Media Foundation's source reader (advanced video processing: it
// scales to the size and gives NV12) on a thread per open movie, a frame
// ahead. Where it can't decode (no H.264: a Windows "N" edition, Proton on
// the Steam Deck) RegisterHdMovie says no and the fallback .bik plays.
// Android: not yet (the fallback .bik).
//
// Test aid: SVR2011_TEST_HD_MOVIES=1 logs the decoders.

#include "hd_movies.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "user_movies.h"

extern "C" {
#include "bink_encode.h"
}

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace fs = std::filesystem;

namespace {

constexpr int kFps = 30;
constexpr const char* kHdFolder = ".hd";

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool LogOn() {
  static const bool on = [] {
    const char* e = std::getenv("SVR2011_TEST_HD_MOVIES");
    return e && *e == '1';
  }();
  return on;
}

struct HdMovie {
  fs::path mp4;
  int size = 1024;  // the stub's (square)
  bool square = false;  // the mp4 is laid out like the game's (else: the big screen only)
};
std::mutex g_mutex;
std::map<std::string, HdMovie> g_movies;  // by the stub's file name (lower case)

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

// A .bik's size from its header (0: not one).
int BinkSize(const fs::path& bik) {
  uint8_t h[28] = {};
  FILE* f = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&f, bik.wstring().c_str(), L"rb")) f = nullptr;
#else
  f = std::fopen(bik.c_str(), "rb");
#endif
  if (!f) return 0;
  const size_t n = std::fread(h, 1, sizeof h, f);
  std::fclose(f);
  if (n < sizeof h || std::memcmp(h, "BIK", 3)) return 0;
  const int w = h[20] | h[21] << 8, hh = h[24] | h[25] << 8;
  return w == hh && (w == 320 || w == 640 || w == 1024) ? w : 0;
}

// The stub: `frames` frames at size x size, black, then the same.
bool WriteStub(const fs::path& out, int size, int frames) {
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  const fs::path part = out.string() + ".part";
  FILE* f = nullptr;
#if defined(_WIN32)
  if (_wfopen_s(&f, part.wstring().c_str(), L"wb")) f = nullptr;
#else
  f = std::fopen(part.c_str(), "wb");
#endif
  if (!f) return false;
  BinkWriter* w = bink_writer_open_size(f, frames, size, size);
  bool ok = w != nullptr;
  if (ok) {
    std::vector<uint8_t> black(size_t(size) * size * 4, 0);
    for (size_t i = 3; i < black.size(); i += 4) black[i] = 255;
    ok = bink_writer_frame(w, black.data()) == 1;
    for (int k = 1; ok && k < frames; ++k) ok = bink_writer_repeat(w) == 1;
    if (ok) ok = bink_writer_close(w) != 0;
    else bink_writer_free(w);
  }
  std::fclose(f);
  if (ok) {
    fs::rename(part, out, ec);
    ok = !ec;
  }
  if (!ok) fs::remove(part, ec);
  return ok;
}

#if defined(_WIN32)

// Media Foundation on this thread (COM, multithreaded) - once per thread.
bool MfOn() {
  thread_local int state = 0;  // 0 not tried, 1 on, 2 failed
  if (!state) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    state = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)) ? 1 : 2;
  }
  return state == 1;
}

// A source reader for `mp4` giving NV12 at w x h (scaled by the reader).
ComPtr<IMFSourceReader> OpenReader(const fs::path& mp4, uint32_t w, uint32_t h) {
  ComPtr<IMFAttributes> attrs;
  ComPtr<IMFSourceReader> reader;
  if (FAILED(MFCreateAttributes(&attrs, 1)) ||
      FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE)) ||
      FAILED(MFCreateSourceReaderFromURL(mp4.wstring().c_str(), attrs.Get(), &reader)))
    return nullptr;
  const DWORD video = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(video, TRUE);
  ComPtr<IMFMediaType> type;
  if (FAILED(MFCreateMediaType(&type)) || FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
      FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12)) ||
      FAILED(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h)) ||
      FAILED(reader->SetCurrentMediaType(video, nullptr, type.Get())))
    return nullptr;
  return reader;
}

// The mp4's length (s) and source size, and whether a frame decodes here.
bool Probe(const fs::path& mp4, double* seconds, uint32_t* sw, uint32_t* sh) {
  if (!MfOn()) return false;
  ComPtr<IMFSourceReader> reader;
  if (FAILED(MFCreateSourceReaderFromURL(mp4.wstring().c_str(), nullptr, &reader))) return false;
  PROPVARIANT v;
  PropVariantInit(&v);
  if (FAILED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &v))) return false;
  *seconds = double(v.uhVal.QuadPart) / 1e7;
  PropVariantClear(&v);
  ComPtr<IMFMediaType> native;
  if (FAILED(reader->GetNativeMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native)) ||
      FAILED(MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, sw, sh)) || !*sw || !*sh)
    return false;
  // (a frame: the decoder exists - not so without H.264)
  ComPtr<IMFSourceReader> test = OpenReader(mp4, 320, 320);
  if (!test) return false;
  DWORD flags = 0;
  LONGLONG t = 0;
  ComPtr<IMFSample> sample;
  for (int k = 0; k < 8 && !sample; ++k)
    if (FAILED(test->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &t, &sample)) ||
        (flags & MF_SOURCE_READERF_ENDOFSTREAM))
      break;
  return sample != nullptr;
}

// One open HD movie: a thread decoding a frame ahead of the game.
class Decoder {
 public:
  Decoder(HdMovie movie) : movie_(std::move(movie)) {
    w_ = uint32_t(movie_.size);
    h_ = movie_.square ? w_ : w_ * 220 / 320;
    h_ &= ~1u;
    thread_ = std::thread([this] { Run(); });
  }
  ~Decoder() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
  }

  // Copies the frame for `seconds` (the newest at or before it; none yet:
  // nothing) into Bink's planes: Y (pitch ypitch, the top h_ rows), cR / cB
  // (half size). The strip below stays the stub's black.
  void Write(double seconds, uint8_t* y, uint32_t ypitch, uint8_t* cr, uint8_t* cb, uint32_t cpitch) {
    want_ = seconds;
    std::lock_guard lock(mutex_);
    const Frame* best = nullptr;
    for (const Frame& f : frames_)
      if (f.ready && f.time <= seconds + 0.5 / kFps && (!best || f.time > best->time)) best = &f;
    if (!best) return;
    for (uint32_t r = 0; r < h_; ++r) std::memcpy(y + size_t(r) * ypitch, best->nv12.data() + size_t(r) * w_, w_);
    const uint8_t* uv = best->nv12.data() + size_t(w_) * h_;
    for (uint32_t r = 0; r < h_ / 2; ++r) {
      uint8_t* u = cb + size_t(r) * cpitch;
      uint8_t* v = cr + size_t(r) * cpitch;
      const uint8_t* s = uv + size_t(r) * w_;
      for (uint32_t x = 0; x < w_ / 2; ++x) u[x] = s[2 * x], v[x] = s[2 * x + 1];
    }
  }

 private:
  struct Frame {
    std::vector<uint8_t> nv12;
    double time = -1;
    bool ready = false;
  };

  void Run() {
    if (!MfOn()) return;
    ComPtr<IMFSourceReader> reader = OpenReader(movie_.mp4, w_, h_);
    if (!reader) {
      REXLOG_WARN("[svr2011] hd movies: can't decode {}", movie_.mp4.string());
      return;
    }
    if (LogOn()) REXLOG_INFO("[svr2011] hd movies: decoding {} at {}x{}", movie_.mp4.string(), w_, h_);
    double last = -1;
    bool ended = false;
    while (!stop_) {
      // (a frame ahead of the game at most; the game looped: from the start)
      const double want = want_.load();
      if (last >= 0 && want + 1.0 < last) {
        PROPVARIANT p;
        PropVariantInit(&p);
        p.vt = VT_I8;
        p.hVal.QuadPart = 0;
        reader->SetCurrentPosition(GUID_NULL, p);
        last = -1;
        ended = false;
        std::lock_guard lock(mutex_);
        for (Frame& f : frames_) f.ready = false;
      }
      if (ended || (last >= 0 && last > want + 2.0 / kFps)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        continue;
      }
      DWORD flags = 0;
      LONGLONG t = 0;
      ComPtr<IMFSample> sample;
      if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &t, &sample)) ||
          (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
        ended = true;  // (the end: the last frame stays until the game loops or closes)
        continue;
      }
      if (!sample) continue;
      ComPtr<IMFMediaBuffer> buffer;
      if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
      Frame f;
      f.nv12.resize(size_t(w_) * h_ * 3 / 2);
      ComPtr<IMF2DBuffer> b2;
      BYTE* data = nullptr;
      LONG pitch = 0;
      bool copied = false;
      if (SUCCEEDED(buffer.As(&b2)) && SUCCEEDED(b2->Lock2D(&data, &pitch))) {
        for (uint32_t r = 0; r < h_; ++r) std::memcpy(f.nv12.data() + size_t(r) * w_, data + LONG_PTR(pitch) * r, w_);
        const BYTE* uv = data + LONG_PTR(pitch) * h_;
        for (uint32_t r = 0; r < h_ / 2; ++r)
          std::memcpy(f.nv12.data() + size_t(w_) * h_ + size_t(r) * w_, uv + LONG_PTR(pitch) * r, w_);
        b2->Unlock2D();
        copied = true;
      } else {
        DWORD len = 0;
        if (SUCCEEDED(buffer->Lock(&data, nullptr, &len))) {
          std::memcpy(f.nv12.data(), data, std::min<size_t>(len, f.nv12.size()));
          buffer->Unlock();
          copied = true;
        }
      }
      if (!copied) continue;
      f.time = double(t) / 1e7;
      f.ready = true;
      last = f.time;
      std::lock_guard lock(mutex_);
      // (the oldest of three goes)
      auto oldest = std::min_element(frames_, frames_ + 3, [](const Frame& a, const Frame& b) {
        return !a.ready ? true : !b.ready ? false : a.time < b.time;
      });
      *oldest = std::move(f);
    }
  }

  HdMovie movie_;
  uint32_t w_ = 0, h_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<double> want_{0.0};
  std::mutex mutex_;
  Frame frames_[3];
};

#else

bool Probe(const fs::path&, double*, uint32_t*, uint32_t*) { return false; }
class Decoder {
 public:
  explicit Decoder(HdMovie) {}
  void Write(double, uint8_t*, uint32_t, uint8_t*, uint8_t*, uint32_t) {}
};

#endif

std::mutex g_open_mutex;
std::map<uint32_t, std::unique_ptr<Decoder>> g_open;  // by the guest Bink

}  // namespace

namespace svr2011 {

bool RegisterHdMovie(const std::string& file, const fs::path& mp4) {
  const fs::path movies = UserMoviesFolder();
  if (movies.empty()) return false;
  double seconds = 0;
  uint32_t sw = 0, sh = 0;
  if (!Probe(mp4, &seconds, &sw, &sh) || seconds <= 0) {
    REXLOG_INFO("[svr2011] hd movies: {} doesn't play here (no decoder) - its .bik does", mp4.filename().string());
    return false;
  }
  HdMovie m;
  m.mp4 = mp4;
  const int fallback = BinkSize(movies / file);
  m.size = fallback ? fallback : 1024;
  m.square = sw * 10 / sh >= 9 && sw * 10 / sh <= 11;
  const int frames = std::max(1, int(seconds * kFps + 0.5));
  const fs::path stub = movies / kHdFolder / file;
  // (made again when the mp4 or the size changed: the stub's own size and
  // length say; same: kept)
  std::error_code ec;
  if (BinkSize(stub) != m.size || fs::last_write_time(stub, ec) < fs::last_write_time(mp4, ec)) {
    if (!WriteStub(stub, m.size, frames)) {
      REXLOG_WARN("[svr2011] hd movies: can't write {}", stub.string());
      return false;
    }
  }
  if (!fs::exists(movies / file, ec)) fs::copy_file(stub, movies / file, ec);  // (no fallback: the stub is the user movie)
  REXLOG_INFO("[svr2011] hd movies: {} plays {} ({}x{}, {:.1f} s, {})", file, mp4.filename().string(), m.size, m.size,
              seconds, m.square ? "laid out" : "big screen");
  std::lock_guard lock(g_mutex);
  g_movies[Lower(file)] = std::move(m);
  return true;
}

std::string HdMovieFile(const std::string& file) {
  std::lock_guard lock(g_mutex);
  return g_movies.count(Lower(file)) ? std::string(kHdFolder) + "\\" + file : file;
}

}  // namespace svr2011

// BinkOpen(name, flags) -> the Bink: an HD stub's gets its decoder.
REX_EXTERN(__imp__sub_82D49B80);
REX_HOOK_RAW(sub_82D49B80) {
  const uint32_t name = ctx.r3.u32;
  std::string n;
  if (name)
    for (uint32_t k = 0; k < 260 && base[name + k]; ++k) n.push_back(char(base[name + k]));
  __imp__sub_82D49B80(ctx, base);
  const uint32_t bink = ctx.r3.u32;
  const std::string hd = std::string("\\") + kHdFolder + "\\";
  const size_t at = n.find(hd);
  if (!bink || at == std::string::npos) return;
  HdMovie m;
  {
    std::lock_guard lock(g_mutex);
    const auto it = g_movies.find(Lower(n.substr(at + hd.size())));
    if (it == g_movies.end()) return;
    m = it->second;
  }
  std::lock_guard lock(g_open_mutex);
  g_open[bink] = std::make_unique<Decoder>(std::move(m));
}

// BinkClose(bink).
REX_EXTERN(__imp__sub_82D4BEF8);
REX_HOOK_RAW(sub_82D4BEF8) {
  const uint32_t bink = ctx.r3.u32;
  {
    std::unique_ptr<Decoder> d;
    std::lock_guard lock(g_open_mutex);
    if (auto it = g_open.find(bink); it != g_open.end()) {
      d = std::move(it->second);
      g_open.erase(it);
    }
  }
  __imp__sub_82D4BEF8(ctx, base);
}

// A movie's step (sub_82592B90(movie): BinkDoFrame): an HD movie's frame
// over the set just decoded.
REX_EXTERN(__imp__sub_82592B90);
REX_HOOK_RAW(sub_82592B90) {
  const uint32_t movie = ctx.r3.u32;
  __imp__sub_82592B90(ctx, base);
  if (!movie) return;
  const uint32_t bink = Rd32(base + movie + 12), sets = Rd32(base + movie + 32);
  if (!bink || !sets) return;
  std::lock_guard lock(g_open_mutex);
  const auto it = g_open.find(bink);
  if (it == g_open.end()) return;
  const uint32_t frame = Rd32(base + bink + 12), i = Rd32(base + sets + 148);
  if (!frame || i > 1) return;
  const uint32_t set = sets + 152 + i * 48;
  const uint32_t y = Rd32(base + set + 4), yp = Rd32(base + set + 8);
  const uint32_t cr = Rd32(base + set + 16), cp = Rd32(base + set + 20), cb = Rd32(base + set + 28);
  if (!y || !cr || !cb || !yp || !cp) return;
  it->second->Write(double(frame - 1) / kFps, base + y, yp, base + cr, base + cb, cp);
}
