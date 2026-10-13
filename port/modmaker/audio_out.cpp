// A song played from any point (audio_out.h): Media Foundation decodes it to 16-bit PCM in memory,
// XAudio2 plays the buffer from a sample (PlayBegin) - a jump is a stop, a flush and a new start.
// (mfplat / mfreadwrite are delay-loaded: Windows "N" without the Media Feature Pack has neither.)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio_out.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <xaudio2.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace audio_out {
namespace {

struct Song {
  std::vector<uint8_t> pcm;
  WAVEFORMATEX fmt = {};
};

std::mutex g_mu;
std::shared_ptr<Song> g_song;      // ready to play
std::atomic<int> g_state{0};       // 0 none / decoding, 1 ready, -1 failed
std::atomic<int> g_generation{0};  // (a newer Load wins over an older decode still running)
std::string g_error;
bool g_muted = false;

IXAudio2* g_xa = nullptr;
IXAudio2MasteringVoice* g_master = nullptr;
IXAudio2SourceVoice* g_voice = nullptr;
std::shared_ptr<Song> g_voice_song;  // (the buffer the voice reads stays alive)
bool g_playing = false;

bool Decode(const std::wstring& file, Song& out, std::string& err) {
  if (!LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) ||
      !LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
    return err = "Windows can't play songs here (no Media Foundation: on Windows N, the Media Feature Pack)", false;
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return err = "Media Foundation didn't start", false;
  bool ok = false;
  IMFSourceReader* rd = nullptr;
  IMFMediaType* want = nullptr;
  IMFMediaType* got = nullptr;
  WAVEFORMATEX* wf = nullptr;
  UINT32 wf_size = 0;
  if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &rd))) {
    err = "the song didn't open (a format Windows can't read?)";
    goto done;
  }
  rd->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
  rd->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
  MFCreateMediaType(&want);
  want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
  want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
  want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
  if (FAILED(rd->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want)) ||
      FAILED(rd->GetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got)) ||
      FAILED(MFCreateWaveFormatExFromMFMediaType(got, &wf, &wf_size))) {
    err = "the song has no audio Windows can decode";
    goto done;
  }
  out.fmt = *wf;
  out.fmt.cbSize = 0;
  out.fmt.wFormatTag = WAVE_FORMAT_PCM;
  for (;;) {
    DWORD flags = 0;
    IMFSample* sample = nullptr;
    if (FAILED(rd->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample))) break;
    if (sample) {
      IMFMediaBuffer* buf = nullptr;
      if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
        BYTE* p = nullptr;
        DWORD n = 0;
        if (SUCCEEDED(buf->Lock(&p, nullptr, &n))) {
          out.pcm.insert(out.pcm.end(), p, p + n);
          buf->Unlock();
        }
        buf->Release();
      }
      sample->Release();
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
  }
  ok = !out.pcm.empty() && out.fmt.nBlockAlign;
  if (!ok) err = "the song decoded to nothing";
done:
  if (wf) CoTaskMemFree(wf);
  if (got) got->Release();
  if (want) want->Release();
  if (rd) rd->Release();
  return ok;
}

bool EnsureVoice(const Song& s) {
  if (!g_xa) {
    if (FAILED(XAudio2Create(&g_xa, 0, XAUDIO2_DEFAULT_PROCESSOR))) return g_xa = nullptr, false;
    if (FAILED(g_xa->CreateMasteringVoice(&g_master))) return g_master = nullptr, false;
  }
  if (g_voice) {
    XAUDIO2_VOICE_DETAILS d;
    g_voice->GetVoiceDetails(&d);
    if (d.InputChannels == s.fmt.nChannels && d.InputSampleRate == s.fmt.nSamplesPerSec) return true;
    g_voice->DestroyVoice(), g_voice = nullptr;
  }
  return SUCCEEDED(g_xa->CreateSourceVoice(&g_voice, &s.fmt));
}

}  // namespace

void Load(const std::wstring& file) {
  Unload();
  const int gen = ++g_generation;
  if (file.empty()) return;
  g_state = 0;
  std::thread([file, gen] {
    auto s = std::make_shared<Song>();
    std::string err;
    const bool ok = Decode(file, *s, err);
    std::lock_guard lock(g_mu);
    if (gen != g_generation) return;  // (another song was asked for meanwhile)
    if (ok) g_song = std::move(s), g_state = 1;
    else g_error = err, g_state = -1;
  }).detach();
}

int State() { return g_state; }
std::string Error() {
  std::lock_guard lock(g_mu);
  return g_error;
}

double Length() {
  std::lock_guard lock(g_mu);
  return g_song && g_song->fmt.nAvgBytesPerSec ? double(g_song->pcm.size()) / g_song->fmt.nAvgBytesPerSec : 0;
}

void Play(double at) {
  std::shared_ptr<Song> s;
  {
    std::lock_guard lock(g_mu);
    s = g_song;
  }
  if (!s || g_muted || !EnsureVoice(*s)) return;
  g_voice->Stop();
  g_voice->FlushSourceBuffers();
  const UINT32 frames = UINT32(s->pcm.size() / s->fmt.nBlockAlign);
  const double begin = at * s->fmt.nSamplesPerSec;
  g_playing = false;
  if (begin < 0 || begin >= frames) return;
  g_voice_song = s;
  XAUDIO2_BUFFER b = {};
  b.Flags = XAUDIO2_END_OF_STREAM;
  b.AudioBytes = UINT32(s->pcm.size());
  b.pAudioData = s->pcm.data();
  b.PlayBegin = UINT32(begin);
  if (SUCCEEDED(g_voice->SubmitSourceBuffer(&b)) && SUCCEEDED(g_voice->Start())) g_playing = true;
}

void Pause() {
  if (g_voice) g_voice->Stop();
  g_playing = false;
}

bool Playing() { return g_playing; }

void Unload() {
  if (g_voice) g_voice->Stop(), g_voice->FlushSourceBuffers(), g_voice->DestroyVoice(), g_voice = nullptr;
  g_voice_song.reset();
  g_playing = false;
  std::lock_guard lock(g_mu);
  g_song.reset();
  g_error.clear();
  g_state = 0;
}

void SetMuted(bool muted) {
  g_muted = muted;
  if (muted) Pause();
}

}  // namespace audio_out
