// WWE SmackDown vs. Raw 2011 - user movies to and from MP4 (see
// movie_transcode.h).
//
// Bink -> MP4: the launcher's Bink reader (launcher/bink_decode.c) gives
// BGRA frames, converted here to NV12 (BT.601, what H.264 encoders take) for
// the platform's encoder: Media Foundation's sink writer on Windows, the NDK's
// MediaCodec + MediaMuxer on Android. MP4 -> Bink: the platform's decoder
// gives frames (Windows: RGB32 from a source reader; Android: YUV images from
// an AImageReader, whatever the phone's own layout), the launcher's Bink
// writer (launcher/bink_encode.c) stores them. Both keep the frame (320 x 320,
// 30 fps, the game's titantron layout) as it is.
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
using Microsoft::WRL::ComPtr;
#elif defined(__ANDROID__)
#include <chrono>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaMuxer.h>
#endif

#if defined(_WIN32) || defined(__ANDROID__)
extern "C" {
#include "bink_decode.h"
#include "bink_encode.h"
}
#endif

namespace svr2011 {

#if defined(_WIN32) || defined(__ANDROID__)

namespace {

// BGRA (w x h) -> NV12: the Y plane (rows y_stride apart), then U V
// interleaved at half size (rows uv_stride apart). BT.601 studio range.
void ToNv12(const uint8_t* bgra, int w, int h, uint8_t* y_plane, int y_stride, uint8_t* uv, int uv_stride) {
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const uint8_t* p = bgra + (size_t(y) * w + x) * 4;
      const int b = p[0], g = p[1], r = p[2];
      y_plane[size_t(y) * y_stride + x] = uint8_t(std::clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16, 0, 255));
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
      uint8_t* o = uv + size_t(y / 2) * uv_stride + x;
      o[0] = uint8_t(std::clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128, 0, 255));
      o[1] = uint8_t(std::clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128, 0, 255));
    }
  }
}

LONGLONG FrameTime(BinkReader* reader) { return bink_frame_time(reader) > 0 ? bink_frame_time(reader) : 333333; }

// One encode at `bitrate` (bits per second); the platform's half below.
bool EncodeMp4(BinkReader* reader, const std::filesystem::path& mp4, uint32_t bitrate);

// Writes the frames of an MP4 to a Bink movie: next(frame) gives each frame
// (BINK_ENC_W x BINK_ENC_H BGRA), false at the end. `frames`: the movie's
// length (a stream a frame or two short: the last frame again).
template <typename Next>
bool WriteBink(const std::filesystem::path& bik, int frames, Next next) {
  const std::filesystem::path part = bik.string() + ".part";
#if defined(_WIN32)
  FILE* f = nullptr;
  if (_wfopen_s(&f, part.wstring().c_str(), L"wb")) f = nullptr;
#else
  FILE* f = std::fopen(part.c_str(), "wb");
#endif
  if (!f) return false;
  BinkWriter* writer = bink_writer_open(f, frames);
  std::vector<uint8_t> frame(size_t(BINK_ENC_W) * BINK_ENC_H * 4, 0);
  bool ok = writer != nullptr, more = true;
  for (int written = 0; ok && written < frames; ++written) {
    if (more) more = next(frame.data());
    ok = bink_writer_frame(writer, frame.data()) == 1;
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
  return ok;
}

}  // namespace

bool CanTranscodeMovies() { return true; }

bool BikToMp4(const std::filesystem::path& bik, const std::filesystem::path& mp4, uint64_t max_bytes) {
  WCHAR err[256] = {};
  BinkReader* reader = bink_open(bik.wstring().c_str(), err, 256);
  if (!reader) {
    REXLOG_WARN("movies: {} isn't a Bink movie", bik.filename().string());
    return false;
  }
  const int frames = bink_frames(reader);
  const double seconds = std::max(1.0, double(frames) * double(FrameTime(reader)) / 1e7);
  // ~90% of the limit, between 0.3 and 3 Mbit/s (the picture is 320 x 320)
  double bitrate = std::clamp(double(max_bytes) * 8 * 0.9 / seconds, 300000.0, 3000000.0);
  bool ok = false;
  std::error_code ec;
  for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
    std::filesystem::remove(mp4, ec);
    bink_rewind(reader);
    if (!EncodeMp4(reader, mp4, uint32_t(bitrate))) break;
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

#endif

#if defined(_WIN32)

namespace {

// Media Foundation for this thread (the transfers run on threads of their own).
struct MfThread {
  bool ok = false, co_ok = false;
  MfThread() {
    co_ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  }
  ~MfThread() {
    if (ok) MFShutdown();
    if (co_ok) CoUninitialize();
  }
};

bool EncodeMp4(BinkReader* reader, const std::filesystem::path& mp4, uint32_t bitrate) {
  MfThread mf;
  if (!mf.ok) return false;
  const int w = bink_width(reader), h = bink_height(reader), frames = bink_frames(reader);
  const LONGLONG frame_time = FrameTime(reader);
  const UINT32 fps_num = 10000000, fps_den = UINT32(frame_time);
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
    ToNv12(bgra.data(), w, h, data, w, data + size_t(w) * h, w);
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
  const bool ok = WriteBink(bik, frames, [&](uint8_t* frame) {
    for (;;) {
      DWORD index = 0, flags = 0;
      LONGLONG ts = 0;
      ComPtr<IMFSample> sample;
      if (FAILED(reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &index, &flags, &ts, &sample)) ||
          (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
        return false;
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
            std::memcpy(frame + size_t(y) * BINK_ENC_W * 4, row0 + LONG_PTR(stride) * y, BINK_ENC_W * 4);
          }
        }
        buffer->Unlock();
      }
      return true;
    }
  });
  REXLOG_INFO("movies: MP4 {} -> {} ({} frames): {}", mp4.filename().string(), bik.filename().string(), frames,
              ok ? "done" : "failed");
  return ok;
}

#elif defined(__ANDROID__)

namespace {

constexpr int64_t kWaitUs = 10000;
constexpr int32_t kColorNv12 = 21;  // COLOR_FormatYUV420SemiPlanar

// The encoder's output so far into the muxer (started at the format change);
// true at the end of the stream.
bool Drain(AMediaCodec* codec, AMediaMuxer* muxer, ssize_t* track, bool* started, bool eos) {
  for (int waits = 0; waits < 3000;) {  // (at the end: up to 30 s for the encoder to finish)
    AMediaCodecBufferInfo info;
    const ssize_t idx = AMediaCodec_dequeueOutputBuffer(codec, &info, eos ? kWaitUs : 0);
    if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat* format = AMediaCodec_getOutputFormat(codec);
      *track = AMediaMuxer_addTrack(muxer, format);
      AMediaFormat_delete(format);
      *started = AMediaMuxer_start(muxer) == AMEDIA_OK;
      continue;
    }
    if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
      if (!eos) return false;
      ++waits;
      continue;
    }
    if (idx < 0) continue;  // (output buffers changed)
    size_t size = 0;
    uint8_t* data = AMediaCodec_getOutputBuffer(codec, size_t(idx), &size);
    if (data && info.size > 0 && *started && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG)) {
      AMediaMuxer_writeSampleData(muxer, size_t(*track), data, &info);
    }
    AMediaCodec_releaseOutputBuffer(codec, size_t(idx), false);
    if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return true;
  }
  REXLOG_WARN("movies: the encoder didn't finish");
  return false;
}

bool EncodeMp4(BinkReader* reader, const std::filesystem::path& mp4, uint32_t bitrate) {
  const int w = bink_width(reader), h = bink_height(reader), frames = bink_frames(reader);
  const int64_t frame_us = FrameTime(reader) / 10;
  AMediaCodec* codec = AMediaCodec_createEncoderByType("video/avc");
  if (!codec) return false;
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, w);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, h);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, int32_t(bitrate));
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, int32_t(1000000 / std::max<int64_t>(1, frame_us)));
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 1);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorNv12);
  bool ok = AMediaCodec_configure(codec, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK &&
            AMediaCodec_start(codec) == AMEDIA_OK;
  AMediaFormat_delete(format);
  // the encoder's input layout (its rows may be wider than the picture)
  int32_t stride = w, slice = h;
  if (ok) {
    if (AMediaFormat* in = AMediaCodec_getInputFormat(codec)) {
      AMediaFormat_getInt32(in, AMEDIAFORMAT_KEY_STRIDE, &stride);
      AMediaFormat_getInt32(in, AMEDIAFORMAT_KEY_SLICE_HEIGHT, &slice);
      AMediaFormat_delete(in);
    }
    stride = std::max(stride, w), slice = std::max(slice, h);
  }
  const int fd = ok ? open(mp4.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644) : -1;
  AMediaMuxer* muxer = fd >= 0 ? AMediaMuxer_new(fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4) : nullptr;
  ok = ok && muxer;
  ssize_t track = -1;
  bool started = false;
  std::vector<uint8_t> bgra(size_t(w) * h * 4);
  for (int i = 0; ok && i <= frames; ++i) {
    const bool last = i == frames;  // (then the end of the stream)
    if (!last && !bink_next(reader)) {
      REXLOG_WARN("movies: frame {} of {} is damaged", i, frames);
      ok = false;
      break;
    }
    ssize_t idx;
    while ((idx = AMediaCodec_dequeueInputBuffer(codec, kWaitUs)) < 0) Drain(codec, muxer, &track, &started, false);
    size_t size = 0;
    uint8_t* data = AMediaCodec_getInputBuffer(codec, size_t(idx), &size);
    const size_t need = size_t(stride) * slice + size_t(stride) * (h / 2);
    if (last || !data || size < need) {
      if (!last) REXLOG_WARN("movies: the encoder's buffer is {} bytes, {} needed", size, need);
      AMediaCodec_queueInputBuffer(codec, size_t(idx), 0, 0, uint64_t(i) * frame_us,
                                   AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
      ok = ok && last;
      break;
    }
    bink_bgra(reader, bgra.data());
    ToNv12(bgra.data(), w, h, data, stride, data + size_t(stride) * slice, stride);
    AMediaCodec_queueInputBuffer(codec, size_t(idx), 0, need, uint64_t(i) * frame_us, 0);
    Drain(codec, muxer, &track, &started, false);
  }
  if (muxer) {
    ok = Drain(codec, muxer, &track, &started, true) && ok && started;
    if (started) AMediaMuxer_stop(muxer);
    AMediaMuxer_delete(muxer);
  }
  if (fd >= 0) close(fd);
  AMediaCodec_stop(codec);
  AMediaCodec_delete(codec);
  return ok;
}

// YUV 4:2:0 (any plane layout) -> BGRA, BT.601 studio range.
void ImageToBgra(AImage* image, int w, int h, uint8_t* bgra) {
  uint8_t* plane[3] = {};
  int len[3] = {}, row[3] = {}, pixel[3] = {1, 1, 1};
  for (int p = 0; p < 3; ++p) {
    AImage_getPlaneData(image, p, &plane[p], &len[p]);
    AImage_getPlaneRowStride(image, p, &row[p]);
    AImage_getPlanePixelStride(image, p, &pixel[p]);
  }
  if (!plane[0] || !plane[1] || !plane[2]) return;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int yy = plane[0][y * row[0] + x * pixel[0]] - 16;
      const int u = plane[1][(y / 2) * row[1] + (x / 2) * pixel[1]] - 128;
      const int v = plane[2][(y / 2) * row[2] + (x / 2) * pixel[2]] - 128;
      uint8_t* o = bgra + (size_t(y) * w + x) * 4;
      o[0] = uint8_t(std::clamp((298 * yy + 516 * u + 128) >> 8, 0, 255));
      o[1] = uint8_t(std::clamp((298 * yy - 100 * u - 208 * v + 128) >> 8, 0, 255));
      o[2] = uint8_t(std::clamp((298 * yy + 409 * v + 128) >> 8, 0, 255));
      o[3] = 255;
    }
  }
}

}  // namespace

bool Mp4ToBik(const std::filesystem::path& mp4, const std::filesystem::path& bik) {
  const int fd = open(mp4.c_str(), O_RDONLY);
  if (fd < 0) return false;
  std::error_code ec;
  const auto file_size = std::filesystem::file_size(mp4, ec);
  AMediaExtractor* extractor = AMediaExtractor_new();
  AMediaFormat* format = nullptr;
  const char* mime = nullptr;
  int32_t w = 0, h = 0;
  int64_t duration = 0;
  if (AMediaExtractor_setDataSourceFd(extractor, fd, 0, off64_t(file_size)) == AMEDIA_OK) {
    for (size_t t = 0; t < AMediaExtractor_getTrackCount(extractor) && !format; ++t) {
      AMediaFormat* f = AMediaExtractor_getTrackFormat(extractor, t);
      const char* m = nullptr;
      if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m && std::strncmp(m, "video/", 6) == 0) {
        format = f, mime = m;
        AMediaExtractor_selectTrack(extractor, t);
      } else {
        AMediaFormat_delete(f);
      }
    }
  }
  if (format) {
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &w);
    AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &h);
    AMediaFormat_getInt64(format, AMEDIAFORMAT_KEY_DURATION, &duration);
  }
  bool ok = format && w == BINK_ENC_W && h >= BINK_ENC_H;
  if (format && !ok) REXLOG_WARN("movies: {} is {}x{}, not a user movie", mp4.filename().string(), w, h);
  // the decoder draws into an image reader: planes with their strides, whatever the phone's layout
  AImageReader* images = nullptr;
  ANativeWindow* window = nullptr;
  AMediaCodec* codec = nullptr;
  if (ok) {
    ok = AImageReader_new(w, h, AIMAGE_FORMAT_YUV_420_888, 4, &images) == AMEDIA_OK &&
         AImageReader_getWindow(images, &window) == AMEDIA_OK;
  }
  if (ok) {
    codec = AMediaCodec_createDecoderByType(mime);
    ok = codec && AMediaCodec_configure(codec, format, window, nullptr, 0) == AMEDIA_OK &&
         AMediaCodec_start(codec) == AMEDIA_OK;
  }
  const int frames = std::max(1, int((duration * BINK_ENC_FPS + 500000) / 1000000));
  if (ok) {
    bool input_done = false, output_done = false;
    int idle = 0;  // (a decoder that stops giving pictures: given up after ~10 s)
    ok = WriteBink(bik, frames, [&](uint8_t* frame) {
      while (!output_done) {
        if (!input_done) {
          const ssize_t in = AMediaCodec_dequeueInputBuffer(codec, kWaitUs);
          if (in >= 0) {
            size_t size = 0;
            uint8_t* data = AMediaCodec_getInputBuffer(codec, size_t(in), &size);
            const ssize_t n = data ? AMediaExtractor_readSampleData(extractor, data, size) : -1;
            if (n < 0) {
              AMediaCodec_queueInputBuffer(codec, size_t(in), 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
              input_done = true;
            } else {
              AMediaCodec_queueInputBuffer(codec, size_t(in), 0, size_t(n),
                                           uint64_t(AMediaExtractor_getSampleTime(extractor)), 0);
              AMediaExtractor_advance(extractor);
            }
          }
        }
        AMediaCodecBufferInfo info;
        const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec, &info, kWaitUs);
        if (out < 0) {
          if (++idle > 1000) return false;
          continue;
        }
        idle = 0;
        const bool eos = (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) != 0;
        const bool picture = info.size > 0;
        AMediaCodec_releaseOutputBuffer(codec, size_t(out), picture);  // (to the image reader)
        if (eos) output_done = true;
        if (!picture) continue;
        AImage* image = nullptr;
        for (int t = 0; t < 200; ++t) {  // (the picture arrives a moment later)
          if (AImageReader_acquireNextImage(images, &image) == AMEDIA_OK) break;
          image = nullptr;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!image) continue;
        ImageToBgra(image, BINK_ENC_W, BINK_ENC_H, frame);
        AImage_delete(image);
        return true;
      }
      return false;
    });
  }
  if (codec) {
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
  }
  if (images) AImageReader_delete(images);
  if (format) AMediaFormat_delete(format);
  AMediaExtractor_delete(extractor);
  close(fd);
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
