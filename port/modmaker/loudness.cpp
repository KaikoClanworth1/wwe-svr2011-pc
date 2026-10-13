// A song's integrated loudness (ITU-R BS.1770-4, as LUFS meters show it), for
// a superstar mod's theme_gain= (checklist D7). The song is decoded with Media
// Foundation (mp3, m4a, wav, wma, flac on Windows 10+), K-weighted (a high
// shelf, then a high pass), cut in 400 ms blocks every 100 ms, gated at -70
// LUFS and then 10 LU under the mean of the blocks left.
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <cmath>
#include <string>
#include <vector>

namespace mm {

namespace {

struct Biquad {
  double b0, b1, b2, a1, a2;
  double z1 = 0, z2 = 0;
  double Run(double x) {
    const double y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

// The K-weighting at this sample rate (the coefficients libebur128 derives).
void KWeighting(double fs, Biquad& shelf, Biquad& pass) {
  const double pi = 3.14159265358979323846;
  {
    const double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    const double k = std::tan(pi * f0 / fs), vh = std::pow(10.0, g / 20.0), vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    shelf = {(vh + vb * k / q + k * k) / a0, 2.0 * (k * k - vh) / a0, (vh - vb * k / q + k * k) / a0,
             2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
  }
  {
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double k = std::tan(pi * f0 / fs);
    const double a0 = 1.0 + k / q + k * k;
    pass = {1.0, -2.0, 1.0, 2.0 * (k * k - 1.0) / a0, (1.0 - k / q + k * k) / a0};
  }
}

}  // namespace

// The song's integrated loudness; false (with why) when it doesn't decode.
bool MeasureLufs(const std::wstring& file, double* lufs, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    return false;
  };
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // (a worker thread)
  // (mfplat / mfreadwrite are delay-loaded: Windows "N" without the Media Feature Pack has neither)
  static const bool started = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) &&
                              LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) &&
                              SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  if (!started) return fail("Media Foundation didn't start");
  IMFSourceReader* rd = nullptr;
  if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &rd))) return fail("the song didn't open (format not supported?)");
  rd->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  rd->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
  IMFMediaType* want = nullptr;
  MFCreateMediaType(&want);
  want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
  const HRESULT set = rd->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want);
  want->Release();
  IMFMediaType* got = nullptr;
  if (FAILED(set) || FAILED(rd->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got))) {
    rd->Release();
    return fail("the song has no audio Media Foundation can decode");
  }
  UINT32 channels = 0, rate = 0;
  got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
  got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
  got->Release();
  if (!channels || !rate) {
    rd->Release();
    return fail("the song's audio format is unknown");
  }
  std::vector<Biquad> shelf(channels), pass(channels);
  std::vector<double> weight(channels, 1.0);  // L R C 1, LFE left out, surrounds 1.41 (5.1 order)
  for (UINT32 c = 3; c < channels && channels > 3; ++c) weight[c] = c == 3 ? 0.0 : 1.41;
  for (UINT32 c = 0; c < channels; ++c) KWeighting(double(rate), shelf[c], pass[c]);
  // per 100 ms step: the K-weighted energy of each channel (weighted: surrounds 1.41)
  const size_t step = rate / 10;
  std::vector<double> steps;  // energy per step, summed over channels
  double acc = 0;
  size_t in_step = 0;
  for (;;) {
    DWORD flags = 0;
    IMFSample* sample = nullptr;
    if (FAILED(rd->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample))) break;
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
      if (sample) sample->Release();
      break;
    }
    if (!sample) continue;
    IMFMediaBuffer* buf = nullptr;
    if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
      BYTE* p = nullptr;
      DWORD len = 0;
      if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
        const float* f = reinterpret_cast<const float*>(p);
        const size_t frames = len / sizeof(float) / channels;
        for (size_t i = 0; i < frames; ++i) {
          for (UINT32 c = 0; c < channels; ++c) {
            const double y = pass[c].Run(shelf[c].Run(double(f[i * channels + c])));
            acc += y * y * weight[c];
          }
          if (++in_step == step) steps.push_back(acc), acc = 0, in_step = 0;
        }
        buf->Unlock();
      }
      buf->Release();
    }
    sample->Release();
  }
  rd->Release();
  if (steps.size() < 4) return fail("the song is too short to measure (under half a second)");
  // 400 ms blocks every 100 ms: mean square over 4 steps
  std::vector<double> blocks;
  for (size_t i = 0; i + 4 <= steps.size(); ++i) blocks.push_back((steps[i] + steps[i + 1] + steps[i + 2] + steps[i + 3]) / double(4 * step));
  auto loud = [](double z) { return -0.691 + 10.0 * std::log10(z > 0 ? z : 1e-20); };
  double sum = 0;
  size_t n = 0;
  for (double z : blocks)
    if (loud(z) > -70.0) sum += z, ++n;
  if (!n) return fail("the song is silent");
  const double relative = loud(sum / double(n)) - 10.0;
  sum = 0, n = 0;
  for (double z : blocks)
    if (loud(z) > -70.0 && loud(z) > relative) sum += z, ++n;
  if (!n) return fail("the song is silent");
  *lufs = loud(sum / double(n));
  return true;
}

}  // namespace mm
