// video_mf.h: a Source Reader with the video processor on, RGB32 out (B G R x in memory: BGRA with x set to 255).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "video_mf.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mm {
namespace video_mf {

using Microsoft::WRL::ComPtr;

struct Reader::Impl {
  ComPtr<IMFSourceReader> reader;
  LONGLONG next = 0;      // the time of the next sample read (100 ns)
  LONGLONG frame = 333333;  // a frame's duration
  LONGLONG shown = -1;    // the time of the frame in bgra
  bool started = false;
};

Reader::Reader() = default;

Reader::~Reader() {
  if (p_ && p_->started) {
    p_->reader.Reset();
    MFShutdown();
  }
}

bool Reader::Open(const std::wstring& file, std::string& err) {
  if (!LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) ||
      !LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
    return err = "Media Foundation isn't installed (Windows \"N\": the Media Feature Pack)", false;
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return err = "Media Foundation didn't start", false;
  p_ = std::make_unique<Impl>();
  p_->started = true;
  ComPtr<IMFAttributes> attrs;
  if (FAILED(MFCreateAttributes(&attrs, 2)) || FAILED(attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE)) ||
      FAILED(MFCreateSourceReaderFromURL(file.c_str(), attrs.Get(), &p_->reader)))
    return err = "the video didn't open", false;
  const DWORD video = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  p_->reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  p_->reader->SetStreamSelection(video, TRUE);
  ComPtr<IMFMediaType> type;
  if (FAILED(MFCreateMediaType(&type)) || FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
      FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) || FAILED(p_->reader->SetCurrentMediaType(video, nullptr, type.Get())))
    return err = "the video can't be decoded to pictures", false;
  ComPtr<IMFMediaType> out;
  UINT32 w = 0, h = 0, num = 0, den = 0;
  if (FAILED(p_->reader->GetCurrentMediaType(video, &out)) || FAILED(MFGetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h)
    return err = "the video has no picture size", false;
  if (SUCCEEDED(MFGetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, &num, &den)) && num && den) p_->frame = LONGLONG(10000000.0 * den / num);
  w_ = int(w), h_ = int(h);
  PROPVARIANT v;
  PropVariantInit(&v);
  if (SUCCEEDED(p_->reader->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &v)) && v.vt == VT_UI8)
    seconds_ = double(v.uhVal.QuadPart) / 1e7;
  PropVariantClear(&v);
  return true;
}

bool Reader::FrameAt(double t, std::vector<uint8_t>& bgra) {
  if (!p_ || !p_->reader || w_ <= 0) return false;
  if (seconds_ > 0) t = std::fmod(t, seconds_);
  const LONGLONG want = LONGLONG(t * 1e7);
  if (p_->shown >= 0 && want >= p_->shown && want < p_->shown + p_->frame) return false;  // (the same frame)
  if (want < p_->shown || want > p_->next + 10000000) {  // (back, or more than a second ahead: seek)
    PROPVARIANT p;
    InitPropVariantFromInt64(want, &p);
    p_->reader->SetCurrentPosition(GUID_NULL, p);
    PropVariantClear(&p);
    p_->next = want;
  }
  ComPtr<IMFSample> last;
  LONGLONG last_t = -1;
  for (int guard = 0; guard < 600; ++guard) {  // (read up to the frame shown at `want`)
    DWORD flags = 0;
    LONGLONG ts = 0;
    ComPtr<IMFSample> s;
    if (FAILED(p_->reader->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, nullptr, &flags, &ts, &s))) break;
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {  // (the end: start over next time)
      PROPVARIANT p;
      InitPropVariantFromInt64(0, &p);
      p_->reader->SetCurrentPosition(GUID_NULL, p);
      PropVariantClear(&p);
      p_->next = 0;
      break;
    }
    if (!s) continue;
    p_->next = ts + p_->frame;
    last = s, last_t = ts;
    if (ts + p_->frame > want) break;
  }
  if (!last) return false;
  ComPtr<IMFMediaBuffer> buf;
  if (FAILED(last->ConvertToContiguousBuffer(&buf))) return false;
  BYTE* data = nullptr;
  DWORD len = 0;
  if (FAILED(buf->Lock(&data, nullptr, &len))) return false;
  const size_t need = size_t(w_) * size_t(h_) * 4;
  bgra.resize(need);
  if (len >= need) {
    std::memcpy(bgra.data(), data, need);
    for (size_t i = 3; i < need; i += 4) bgra[i] = 255;  // (RGB32: x, not alpha)
  }
  buf->Unlock();
  p_->shown = last_t;
  return len >= need;
}

}  // namespace video_mf
}  // namespace mm
