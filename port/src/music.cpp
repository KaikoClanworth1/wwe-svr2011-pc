// WWE SmackDown vs. Raw 2011 - user music for entrances (see music.h).

#include "music.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmreg.h>
#include <wrl/client.h>
#include <xaudio2.h>
#elif defined(__ANDROID__)
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <SDL3/SDL.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#endif

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/logging.h>
#include <fmt/format.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

namespace svr2011 {

namespace {

using rex::kernel::xam::apps::XmpApp;

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;
constexpr const wchar_t* kExtensions[] = {L".mp3", L".wma", L".m4a", L".aac", L".wav", L".flac"};
constexpr uint32_t kSlots = 6;   // buffers owned by the player
constexpr uint32_t kQueued = 4;  // at most this many queued on the voice
constexpr double kBufferSeconds = 0.1;
#else
// (Android's decoders: no WMA)
constexpr const wchar_t* kExtensions[] = {L".mp3", L".m4a", L".aac", L".wav", L".flac", L".ogg"};
#endif

std::filesystem::path g_folder;

bool IsSong(const std::filesystem::path& path) {
  std::wstring ext = path.extension().wstring();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
  return std::find(std::begin(kExtensions), std::end(kExtensions), ext) != std::end(kExtensions);
}

// The playlists: each subfolder (its first song, by name) and each song
// directly in the folder, sorted by name.
std::vector<std::pair<std::u16string, std::filesystem::path>> Playlists() {
  std::vector<std::pair<std::u16string, std::filesystem::path>> lists;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(g_folder, ec);
       !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    const std::filesystem::path& path = it->path();
    if (it->is_directory(ec)) {
      std::vector<std::filesystem::path> songs;
      std::error_code ec2;
      for (auto s = std::filesystem::recursive_directory_iterator(
               path, std::filesystem::directory_options::skip_permission_denied, ec2);
           !ec2 && s != std::filesystem::recursive_directory_iterator(); s.increment(ec2)) {
        if (s->is_regular_file(ec2) && IsSong(s->path())) songs.push_back(s->path());
      }
      if (!songs.empty()) {
        std::sort(songs.begin(), songs.end());
        lists.emplace_back(path.filename().u16string(), songs.front());
      }
    } else if (it->is_regular_file(ec) && IsSong(path)) {
      lists.emplace_back(path.stem().u16string(), path);
    }
  }
  std::sort(lists.begin(), lists.end());
  return lists;
}

#if defined(_WIN32)
// Plays one song on a loop on its own XAudio2 voice, on a thread of its own.
class Player {
 public:
  void Play(std::filesystem::path path) {
    std::lock_guard lock(mutex_);
    request_ = std::move(path);
    has_request_ = true;
    paused_ = false;
    wake_.notify_one();
  }
  void Stop() {
    std::lock_guard lock(mutex_);
    request_.clear();
    has_request_ = true;
    wake_.notify_one();
  }
  void Pause(bool paused) {
    std::lock_guard lock(mutex_);
    paused_ = paused;
    wake_.notify_one();
  }
  void SetVolume(float volume) {
    std::lock_guard lock(mutex_);
    game_volume_ = std::clamp(volume, 0.0f, 1.0f);
  }

  void Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
      REXLOG_WARN("user music: Media Foundation unavailable");
      return;
    }
    if (FAILED(XAudio2Create(&xaudio2_, 0, XAUDIO2_DEFAULT_PROCESSOR)) ||
        FAILED(xaudio2_->CreateMasteringVoice(&master_))) {
      REXLOG_WARN("user music: no audio output");
      return;
    }
    for (;;) {
      std::filesystem::path request;
      bool has_request, paused;
      {
        std::unique_lock lock(mutex_);
        // Idle: sleep until asked; playing: top the queue up every 20 ms.
        wake_.wait_for(lock, voice_ ? std::chrono::milliseconds(20) : std::chrono::hours(1),
                       [&] { return has_request_ || (voice_ && paused_ != voice_paused_); });
        has_request = std::exchange(has_request_, false);
        request = request_;
        paused = paused_;
      }
      if (has_request) {
        StopVoice();
        song_ = request;
        if (!song_.empty() && !Open(song_)) song_.clear();
      }
      if (!voice_) continue;
      if (paused != voice_paused_) {
        paused ? voice_->Stop() : voice_->Start();
        voice_paused_ = paused;
      }
      UpdateVolume();
      if (voice_paused_) continue;
      Feed();
      if (ended_) {
        XAUDIO2_VOICE_STATE state = {};
        voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued == 0) {  // from the top again, until stopped
          StopVoice();
          if (Open(song_)) {
            voice_->Start();
            voice_paused_ = false;
          } else {
            song_.clear();
          }
        }
      }
    }
  }

 private:
  bool Open(const std::filesystem::path& path) {
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) {
      REXLOG_WARN("user music: cannot open {}", path.string());
      return false;
    }
    const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    reader->SetStreamSelection(stream, TRUE);
    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    ComPtr<IMFMediaType> got;
    WAVEFORMATEX* format = nullptr;
    UINT32 format_size = 0;
    if (FAILED(reader->SetCurrentMediaType(stream, nullptr, want.Get())) ||
        FAILED(reader->GetCurrentMediaType(stream, &got)) ||
        FAILED(MFCreateWaveFormatExFromMFMediaType(got.Get(), &format, &format_size))) {
      REXLOG_WARN("user music: cannot decode {}", path.string());
      return false;
    }
    IXAudio2SourceVoice* voice = nullptr;
    const HRESULT hr = xaudio2_->CreateSourceVoice(&voice, format);
    block_align_ = std::max<uint32_t>(format->nBlockAlign, 1);
    bytes_per_buffer_ = uint32_t(format->nAvgBytesPerSec * kBufferSeconds);
    bytes_per_buffer_ -= bytes_per_buffer_ % block_align_;
    CoTaskMemFree(format);
    if (FAILED(hr)) {
      REXLOG_WARN("user music: no voice for {}", path.string());
      return false;
    }
    reader_ = reader;
    voice_ = voice;
    ended_ = false;
    voice_paused_ = true;  // started by the loop (unless paused)
    volume_ = -1.0f;
    return true;
  }

  void StopVoice() {
    if (voice_) {
      voice_->DestroyVoice();  // stops it and drops its queue
      voice_ = nullptr;
    }
    reader_.Reset();
  }

  void UpdateVolume() {
    float v;
    {
      std::lock_guard lock(mutex_);
      v = game_volume_;
    }
    if (rex::cvar::GetFlagByName("audio_mute") == "true") v = 0.0f;
    if (v != volume_) {
      volume_ = v;
      voice_->SetVolume(v);
    }
  }

  // Tops the voice's queue up with decoded audio.
  void Feed() {
    XAUDIO2_VOICE_STATE state = {};
    voice_->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    uint32_t queued = state.BuffersQueued;
    while (!ended_ && queued < kQueued) {
      std::vector<uint8_t>& slot = slots_[next_slot_];
      slot.clear();
      while (slot.size() < bytes_per_buffer_) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader_->ReadSample(DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr,
                                       &flags, nullptr, &sample)) ||
            (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR))) {
          ended_ = true;
          break;
        }
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
          slot.insert(slot.end(), data, data + length);
          buffer->Unlock();
        }
      }
      slot.resize(slot.size() - slot.size() % block_align_);
      if (slot.empty()) break;
      XAUDIO2_BUFFER xb = {};
      xb.AudioBytes = uint32_t(slot.size());
      xb.pAudioData = slot.data();
      if (ended_) xb.Flags = XAUDIO2_END_OF_STREAM;
      if (FAILED(voice_->SubmitSourceBuffer(&xb))) {
        ended_ = true;
        break;
      }
      // Never more than kQueued < kSlots queued, so the slot written next is
      // one XAudio2 has finished with.
      next_slot_ = (next_slot_ + 1) % kSlots;
      ++queued;
    }
  }

  // Requests (any thread).
  std::mutex mutex_;
  std::condition_variable wake_;
  std::filesystem::path request_;
  bool has_request_ = false;
  bool paused_ = false;
  float game_volume_ = 1.0f;

  // The player thread's own.
  ComPtr<IXAudio2> xaudio2_;
  IXAudio2MasteringVoice* master_ = nullptr;
  IXAudio2SourceVoice* voice_ = nullptr;
  ComPtr<IMFSourceReader> reader_;
  std::filesystem::path song_;
  bool ended_ = false;
  bool voice_paused_ = true;
  float volume_ = -1.0f;
  uint32_t block_align_ = 1;
  uint32_t bytes_per_buffer_ = 0;
  std::array<std::vector<uint8_t>, kSlots> slots_;
  uint32_t next_slot_ = 0;
};

#elif defined(__ANDROID__)

// Android: the phone's own decoders (NDK AMediaExtractor + AMediaCodec: mp3,
// m4a / aac, flac, wav...) into an SDL audio stream, on a thread of its own.
class Player {
 public:
  void Play(std::filesystem::path path) {
    std::lock_guard lock(mutex_);
    request_ = std::move(path);
    has_request_ = true;
    paused_ = false;
    wake_.notify_one();
  }
  void Stop() {
    std::lock_guard lock(mutex_);
    request_.clear();
    has_request_ = true;
    wake_.notify_one();
  }
  void Pause(bool paused) {
    std::lock_guard lock(mutex_);
    paused_ = paused;
    wake_.notify_one();
  }
  void SetVolume(float volume) {
    std::lock_guard lock(mutex_);
    game_volume_ = std::clamp(volume, 0.0f, 1.0f);
  }

  void Run() {
    for (;;) {
      std::filesystem::path request;
      bool has_request, paused;
      {
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, codec_ ? std::chrono::milliseconds(20) : std::chrono::hours(1),
                       [&] { return has_request_ || (codec_ && paused_ != stream_paused_); });
        has_request = std::exchange(has_request_, false);
        request = request_;
        paused = paused_;
      }
      if (has_request) {
        Close();
        song_ = request;
        if (!song_.empty() && !Open(song_)) song_.clear();
      }
      if (!codec_) continue;
      if (paused != stream_paused_) {
        paused ? SDL_PauseAudioStreamDevice(stream_) : SDL_ResumeAudioStreamDevice(stream_);
        stream_paused_ = paused;
      }
      UpdateVolume();
      if (stream_paused_) continue;
      Feed();
      if (ended_ && SDL_GetAudioStreamQueued(stream_) == 0) {  // from the top again, until stopped
        Close();
        if (!Open(song_)) song_.clear();
      }
    }
  }

 private:
  bool Open(const std::filesystem::path& path) {
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
      REXLOG_WARN("user music: cannot open {}", path.string());
      return false;
    }
    const off64_t size = lseek64(fd_, 0, SEEK_END);
    extractor_ = AMediaExtractor_new();
    if (AMediaExtractor_setDataSourceFd(extractor_, fd_, 0, size) != AMEDIA_OK) {
      REXLOG_WARN("user music: cannot read {}", path.string());
      Close();
      return false;
    }
    for (size_t i = 0; i < AMediaExtractor_getTrackCount(extractor_) && !codec_; ++i) {
      AMediaFormat* format = AMediaExtractor_getTrackFormat(extractor_, i);
      const char* mime = nullptr;
      if (AMediaFormat_getString(format, AMEDIAFORMAT_KEY_MIME, &mime) && mime &&
          std::strncmp(mime, "audio/", 6) == 0) {
        AMediaExtractor_selectTrack(extractor_, i);
        codec_ = AMediaCodec_createDecoderByType(mime);
        if (codec_ && (AMediaCodec_configure(codec_, format, nullptr, nullptr, 0) != AMEDIA_OK ||
                       AMediaCodec_start(codec_) != AMEDIA_OK)) {
          AMediaCodec_delete(codec_);
          codec_ = nullptr;
        }
        int32_t rate = 0, channels = 0;
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
        SetFormat(rate, channels);
      }
      AMediaFormat_delete(format);
    }
    if (!codec_ || !stream_) {
      REXLOG_WARN("user music: cannot decode {}", path.string());
      Close();
      return false;
    }
    ended_ = input_done_ = false;
    stream_paused_ = true;  // started by the loop (unless paused)
    volume_ = -1.0f;
    return true;
  }

  // The decoder's output (16-bit PCM): the stream's input format.
  void SetFormat(int32_t rate, int32_t channels) {
    if (rate <= 0 || channels <= 0) return;
    SDL_AudioSpec spec = {SDL_AUDIO_S16, channels, rate};
    if (!stream_) {
      stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
      if (!stream_) REXLOG_WARN("user music: no audio output ({})", SDL_GetError());
    } else {
      SDL_SetAudioStreamFormat(stream_, &spec, nullptr);
    }
    bytes_per_second_ = rate * channels * 2;
  }

  void Close() {
    if (codec_) {
      AMediaCodec_stop(codec_);
      AMediaCodec_delete(codec_);
      codec_ = nullptr;
    }
    if (extractor_) {
      AMediaExtractor_delete(extractor_);
      extractor_ = nullptr;
    }
    if (fd_ >= 0) {
      close(fd_);
      fd_ = -1;
    }
    if (stream_) SDL_ClearAudioStream(stream_);
  }

  void UpdateVolume() {
    float v;
    {
      std::lock_guard lock(mutex_);
      v = game_volume_;
    }
    if (rex::cvar::GetFlagByName("audio_mute") == "true") v = 0.0f;
    if (v != volume_) {
      volume_ = v;
      SDL_SetAudioStreamGain(stream_, v);
    }
  }

  // Keeps ~0.3 s decoded audio queued on the stream.
  void Feed() {
    while (!ended_ && SDL_GetAudioStreamQueued(stream_) < int(bytes_per_second_ * 0.3)) {
      if (!input_done_) {
        const ssize_t in = AMediaCodec_dequeueInputBuffer(codec_, 2000);
        if (in >= 0) {
          size_t capacity = 0;
          uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, size_t(in), &capacity);
          const ssize_t n = AMediaExtractor_readSampleData(extractor_, buffer, capacity);
          if (n < 0) {
            AMediaCodec_queueInputBuffer(codec_, size_t(in), 0, 0, 0,
                                         AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
            input_done_ = true;
          } else {
            AMediaCodec_queueInputBuffer(codec_, size_t(in), 0, size_t(n),
                                         uint64_t(AMediaExtractor_getSampleTime(extractor_)), 0);
            AMediaExtractor_advance(extractor_);
          }
        }
      }
      AMediaCodecBufferInfo info = {};
      const ssize_t out = AMediaCodec_dequeueOutputBuffer(codec_, &info, 2000);
      if (out >= 0) {
        size_t capacity = 0;
        const uint8_t* data = AMediaCodec_getOutputBuffer(codec_, size_t(out), &capacity);
        if (data && info.size > 0) SDL_PutAudioStreamData(stream_, data + info.offset, info.size);
        AMediaCodec_releaseOutputBuffer(codec_, size_t(out), false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) ended_ = true;
      } else if (out == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
        int32_t rate = 0, channels = 0;
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_SAMPLE_RATE, &rate);
        AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_CHANNEL_COUNT, &channels);
        AMediaFormat_delete(format);
        SetFormat(rate, channels);
      } else if (out == AMEDIACODEC_INFO_TRY_AGAIN_LATER && input_done_) {
        break;  // (draining: wait for the next round)
      }
    }
  }

  // Requests (any thread).
  std::mutex mutex_;
  std::condition_variable wake_;
  std::filesystem::path request_;
  bool has_request_ = false;
  bool paused_ = false;
  float game_volume_ = 1.0f;

  // The player thread's own.
  std::filesystem::path song_;
  int fd_ = -1;
  AMediaExtractor* extractor_ = nullptr;
  AMediaCodec* codec_ = nullptr;
  SDL_AudioStream* stream_ = nullptr;
  int bytes_per_second_ = 1;
  bool ended_ = false, input_done_ = false;
  bool stream_paused_ = true;
  float volume_ = -1.0f;
};

#endif

Player* g_player = nullptr;  // lives for the whole run

}  // namespace

std::filesystem::path UserMusicFolder() { return g_folder; }

std::filesystem::path UserMusicSong(const std::string& name) {
  const std::u16string wanted = std::filesystem::path(std::u8string(name.begin(), name.end())).u16string();
  for (auto& [list, song] : Playlists())
    if (list == wanted) return song;
  return {};
}

void InstallUserMusic(const std::filesystem::path& folder) {
  g_folder = folder;
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);  // so players can find it
  g_player = new Player();
  std::thread([] {
#if defined(_WIN32)
    SetThreadDescription(GetCurrentThread(), L"User music");
#endif
    g_player->Run();
  }).detach();

  XmpApp::HostMusic music;
  music.list_playlists = [] {
    std::vector<std::u16string> names;
    for (auto& [name, song] : Playlists()) names.push_back(name);
    REXLOG_INFO("user music: {} playlist(s) in {}", names.size(), g_folder.string());
    return names;
  };
  music.play = [](const std::u16string& name) {
    for (auto& [list, song] : Playlists()) {
      if (list == name) {
        REXLOG_INFO("user music: playing {}", song.string());
        g_player->Play(song);
        return true;
      }
    }
    return false;
  };
  music.stop = [] {
    REXLOG_INFO("user music: stop");
    g_player->Stop();
  };
  music.pause = [] {
    REXLOG_INFO("user music: pause");
    g_player->Pause(true);
  };
  music.resume = [] {
    REXLOG_INFO("user music: resume");
    g_player->Pause(false);
  };
  music.set_volume = [](float volume) { g_player->SetVolume(volume); };
  XmpApp::SetHostMusic(std::move(music));
}

}  // namespace svr2011

// Saving an entrance whose music is USER PLAYLIST (CREATE AN ENTRANCE ->
// FINALIZE -> SAVE -> YES) waits in sub_8287DDB0 until the music manager is
// done (sub_8286FE30): the user song stopped and each channel of the game's
// music engine stopped - sub_8286F928 accepts channel flags 0, 0x01 or 0x20.
// The entrance-music channel the game paused (0x08) for the user song while
// the saved entrance already had one is never resumed, so the save waited
// forever on a frozen frame. A channel that is only paused counts as stopped
// here (the manager has already asked the channels to stop).
namespace {
std::atomic<uint32_t> g_channel_flags[3] = {0, 0, 0};  // last read, per channel
}

// Channel status: sub_825F31E8(engine, channel, &flags).
REX_EXTERN(__imp__sub_825F31E8);
REX_HOOK_RAW(sub_825F31E8) {
  const uint32_t channel = ctx.r4.u32, out = ctx.r5.u32;
  __imp__sub_825F31E8(ctx, base);
  if (channel < 3 && out) {
    g_channel_flags[channel] = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + out));
  }
}

// "Channels stopped?" of the music manager's player (sub_8286F928).
REX_EXTERN(__imp__sub_8286F928);
REX_HOOK_RAW(sub_8286F928) {
  for (auto& f : g_channel_flags) f = 0;
  __imp__sub_8286F928(ctx, base);
  if (ctx.r3.u32) return;
  bool paused = false;
  for (auto& f : g_channel_flags) {
    const uint32_t flags = f;
    if (flags == 0x08) {
      paused = true;
    } else if (flags && !(flags & 0x21)) {
      return;  // really still playing
    }
  }
  if (paused) ctx.r3.u64 = 1;
}
