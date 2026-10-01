// WWE SmackDown vs. Raw 2011 - user movies to and from MP4 (see
// movie_transcode.h).
//
// Bink -> MP4: the launcher's Bink reader (launcher/bink_decode.c) gives
// BGRA frames, converted here to NV12 (BT.601, what the H.264 encoder takes)
// and written by a Media Foundation sink writer. MP4 -> Bink: a source reader
// gives RGB32 frames, the launcher's Bink writer (launcher/bink_encode.c)
// stores them. Both keep the frame (320 x 320, 30 fps, the game's titantron
// layout) as it is.
#include "movie_transcode.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <system_error>
#include <vector>

#include <rex/logging.h>

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

extern "C" {
#include "bink_decode.h"
#include "bink_encode.h"
}

using Microsoft::WRL::ComPtr;
#endif

namespace svr2011 {

#if defined(_WIN32)

namespace {

// Media Foundation for this thread (the transfers run on threads of their own).
struct MfThread {
  bool ok = false;
  MfThread() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    co_ok = SUCCEEDED(co);
    ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  }
  ~MfThread() {
    if (ok) MFShutdown();
    if (co_ok) CoUninitialize();
  }
  bool co_ok = false;
};

// BGRA (w x h) -> NV12 (Y plane, then interleaved U V at half size), BT.601
// studio range.
void ToNv12(const uint8_t* bgra, int w, int h, uint8_t* out) {
  uint8_t* y_plane = out;
  uint8_t* uv = out + size_t(w) * h;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t* p = bgra + (size_t(y) * w + x) * 4;
      const int b = p[0], g = p[1], r = p[2];
      y_plane[size_t(y) * w + x] = uint8_t(std::clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 0, 255));
    }
  }
  for (int y = 0; y < h; y += 2) {
    for (int x = 0; x < w; x += 2) {
      int r = 0, g = 0, b = 0;
      for (int dy = 0; dy < 2; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          const uint8_t* p = bgra + (size_t(y + dy) * w + x + dx) * 4;
          b += p[0], g += p[1], r += p[2];
        }
      }
      r /= 4, g /= 4, b /= 4;
      uint8_t* o = uv + size_t(y / 2) * w + x;
      o[0] = uint8_t(std::clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 0, 255));
      o[1] = uint8_t(std::clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 0, 255));
    }
  }
}

// One encode at `bitrate` (bits per second).
bool EncodeMp4(BinkReader* reader, const std::filesystem::path& mp4, UINT32 bitrate) {
  const int w = bink_width(reader), h = bink_height(reader), frames = bink_frames(reader);
  const LONGLONG frame_time = bink_frame_time(reader) > 0 ? bink_frame_time(reader) : 333333;
  const UINT32 fps_num = 10000000, fps_den = UINT32(frame_time);
  bink_rewind(reader);
  ComPtr<IMFAttributes> attrs;
  MFCreateAttributes(&attrs, 1);
  attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
  ComPtr<IMFSinkWriter> writer;
  if (FAILED(MFCreateSinkWriterFromURL(mp4.wstring().c_str(), nullptr, attrs.Get(), &writer))) return false;
  ComPtr<IMFMediaType> out, in;
  MFCreateMediaType(&out);
  out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
  out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
  out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, UINT32(w), UINT32(h));
  MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
  MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  DWORD stream = 0;
  if (FAILED(writer->AddStream(out.Get(), &stream))) return false;
  MFCreateMediaType(&in);
  in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
  in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, UINT32(w), UINT32(h));
  MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps_num, fps_den);
  MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  if (FAILED(writer->SetInputMediaType(stream, in.Get(), nullptr)) || FAILED(writer->BeginWriting())) return false;
  std::vector<uint8_t> bgra(size_t(w) * h * 4);
  const DWORD nv12_size = DWORD(size_t(w) * h * 3 / 2);
  for (int i = 0; i < frames; ++i) {
    if (!bink_next(reader)) {
      REXLOG_WARN("movies: frame {} of {} is damaged", i, frames);
      return false;
    }
    bink_bgra(reader, bgra.data());
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    BYTE* data = nullptr;
    if (FAILED(MFCreateMemoryBuffer(nv12_size, &buffer)) || FAILED(buffer->Lock(&data, nullptr, nullptr))) {
      return false;
    }
    ToNv12(bgra.data(), w, h, data);
    buffer->Unlock();
    buffer->SetCurrentLength(nv12_size);
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(LONGLONG(i) * frame_time);
    sample->SetSampleDuration(frame_time);
    if (FAILED(writer->WriteSample(stream, sample.Get()))) return false;
  }
  return SUCCEEDED(writer->Finalize());
}

}  // namespace

bool CanTranscodeMovies() { return true; }

bool BikToMp4(const std::filesystem::path& bik, const std::filesystem::path& mp4, uint64_t max_bytes) {
  MfThread mf;
  if (!mf.ok) return false;
  WCHAR err[256] = L"";
  BinkReader* reader = bink_open(bik.wstring().c_str(), err, 256);
  if (!reader) {
    REXLOG_WARN("movies: {} isn't a Bink movie", bik.filename().string());
    return false;
  }
  const int frames = bink_frames(reader);
  const LONGLONG frame_time = bink_frame_time(reader) > 0 ? bink_frame_time(reader) : 333333;
  const double seconds = std::max(1.0, double(frames) * double(frame_time) / 1e7);
  // ~90% of the limit, between 0.3 and 3 Mbit/s (the picture is 320 x 320)
  double bitrate = std::clamp(double(max_bytes) * 8 * 0.9 / seconds, 300000.0, 3000000.0);
  bool ok = false;
  std::error_code ec;
  for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
    std::filesystem::remove(mp4, ec);
    if (!EncodeMp4(reader, mp4, UINT32(bitrate))) break;
    const uint64_t size = std::filesystem::file_size(mp4, ec);
    if (!ec && size <= max_bytes) {
      ok = true;
      REXLOG_INFO("movies: {} ({} frames) -> MP4 {} KB at {} kbit/s", bik.filename().string(), frames, size / 1024,
                  int(bitrate / 1000));
    } else {
      bitrate *= 0.7;  // (the encoder went over: again, smaller)
    }
  }
  bink_close(reader);
  if (!ok) std::filesystem::remove(mp4, ec);
  return ok;
}

bool Mp4ToBik(const std::filesystem::path& mp4, const std::filesystem::path& bik) {
  MfThread mf;
  if (!mf.ok) return false;
  ComPtr<IMFAttributes> attrs;
  MFCreateAttributes(&attrs, 1);
  attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  ComPtr<IMFSourceReader> reader;
  if (FAILED(MFCreateSourceReaderFromURL(mp4.wstring().c_str(), attrs.Get(), &reader))) return false;
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
  ComPtr<IMFMediaType> type;
  MFCreateMediaType(&type);
  type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  if (FAILED(reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get()))) {
    return false;
  }
  ComPtr<IMFMediaType> actual;
  UINT32 w = 0, h = 0;
  if (FAILED(reader->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &actual)) ||
      FAILED(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &w, &h)) || w != BINK_ENC_W || h < BINK_ENC_H) {
    REXLOG_WARN("movies: {} is {}x{}, not a user movie", mp4.filename().string(), w, h);
    return false;
  }
  LONG stride = LONG(w) * 4;
  UINT32 s = 0;
  if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) stride = LONG(s);
  PROPVARIANT pv;
  PropVariantInit(&pv);
  LONGLONG duration = 0;
  if (SUCCEEDED(reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &pv))) {
    duration = LONGLONG(pv.uhVal.QuadPart);
  }
  PropVariantClear(&pv);
  const int frames = std::max(1, int((duration * BINK_ENC_FPS + 5000000) / 10000000));
  const std::filesystem::path part = bik.string() + ".part";
  FILE* f = nullptr;
  if (_wfopen_s(&f, part.wstring().c_str(), L"wb") || !f) return false;
  BinkWriter* writer = bink_writer_open(f, frames);
  std::vector<uint8_t> frame(size_t(BINK_ENC_W) * BINK_ENC_H * 4, 0);
  bool ok = writer != nullptr;
  int written = 0;
  while (ok && written < frames) {
    DWORD index = 0, flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> sample;
    if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &index, &flags, &ts, &sample)) ||
        (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
      break;
    }
    if (!sample) continue;
    ComPtr<IMFMediaBuffer> buffer;
    BYTE* data = nullptr;
    DWORD length = 0;
    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) && SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
      const LONG pitch = stride < 0 ? -stride : stride;
      if (size_t(pitch) * h <= length) {
        const BYTE* row0 = stride < 0 ? data + size_t(pitch) * (h - 1) : data;
        for (int y = 0; y < BINK_ENC_H; ++y) {
          std::memcpy(frame.data() + size_t(y) * BINK_ENC_W * 4, row0 + LONG_PTR(stride) * y, BINK_ENC_W * 4);
        }
      }
      buffer->Unlock();
    }
    ok = bink_writer_frame(writer, frame.data()) == 1;
    ++written;
  }
  // (a stream a frame or two short: the last frame again)
  while (ok && written < frames) {
    ok = bink_writer_frame(writer, frame.data()) == 1;
    ++written;
  }
  if (writer) {
    if (ok) {
      ok = bink_writer_close(writer) != 0;
    } else {
      bink_writer_free(writer);
    }
  }
  std::fclose(f);
  std::error_code ec;
  if (ok) {
    std::filesystem::rename(part, bik, ec);
    ok = !ec;
  }
  if (!ok) std::filesystem::remove(part, ec);
  REXLOG_INFO("movies: MP4 {} -> {} ({} frames): {}", mp4.filename().string(), bik.filename().string(), frames,
              ok ? "done" : "failed");
  return ok;
}

#else

bool CanTranscodeMovies() { return false; }
bool BikToMp4(const std::filesystem::path&, const std::filesystem::path&, uint64_t) { return false; }
bool Mp4ToBik(const std::filesystem::path&, const std::filesystem::path&) { return false; }

#endif

}  // namespace svr2011
