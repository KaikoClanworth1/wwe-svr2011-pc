// WWE SmackDown vs. Raw 2011 - XAudio2 audio output (see xaudio2_audio.h).

#include "xaudio2_audio.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <xaudio2.h>

#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/logging.h>

namespace svr2011 {

namespace {

// From ksmedia.h, which would need its GUIDs linked in.
constexpr DWORD kSpeakerStereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
constexpr DWORD kSpeaker5Point1 = kSpeakerStereo | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                                  SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
constexpr GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

bool AudioMuted() {
  return rex::cvar::GetFlagByName("audio_mute") == "true";
}

}  // namespace

class XAudio2AudioDriver::VoiceCallback : public IXAudio2VoiceCallback {
 public:
  explicit VoiceCallback(XAudio2AudioDriver* driver) : driver_(driver) {}

  void STDMETHODCALLTYPE OnBufferEnd(void*) noexcept override { driver_->OnBufferEnd(); }
  void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) noexcept override {}
  void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
  void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
  void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
  void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
  void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) noexcept override {
    REXAPU_ERROR("XAudio2 voice error {:08X}", static_cast<uint32_t>(error));
  }

 private:
  XAudio2AudioDriver* driver_;
};

XAudio2AudioDriver::XAudio2AudioDriver(rex::memory::Memory* memory,
                                       rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

XAudio2AudioDriver::~XAudio2AudioDriver() = default;

bool XAudio2AudioDriver::Initialize() {
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  com_initialized_ = SUCCEEDED(hr);

  hr = XAudio2Create(&xaudio2_, 0, XAUDIO2_DEFAULT_PROCESSOR);
  if (FAILED(hr)) {
    REXAPU_ERROR("XAudio2Create failed: {:08X}", static_cast<uint32_t>(hr));
    return false;
  }

  hr = xaudio2_->CreateMasteringVoice(&mastering_voice_, XAUDIO2_DEFAULT_CHANNELS, kFrequency);
  if (FAILED(hr)) {
    REXAPU_ERROR("XAudio2 CreateMasteringVoice failed: {:08X}", static_cast<uint32_t>(hr));
    return false;
  }

  // Pass 5.1 (or wider) speaker setups the six guest channels as they are;
  // fold everything else down to stereo.
  XAUDIO2_VOICE_DETAILS details = {};
  mastering_voice_->GetVoiceDetails(&details);
  output_channels_ = details.InputChannels >= 6 ? 6 : 2;

  WAVEFORMATEXTENSIBLE format = {};
  format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  format.Format.nChannels = static_cast<WORD>(output_channels_);
  format.Format.nSamplesPerSec = kFrequency;
  format.Format.wBitsPerSample = 32;
  format.Format.nBlockAlign = static_cast<WORD>(output_channels_ * sizeof(float));
  format.Format.nAvgBytesPerSec = kFrequency * format.Format.nBlockAlign;
  format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  format.Samples.wValidBitsPerSample = 32;
  format.dwChannelMask = output_channels_ == 6 ? kSpeaker5Point1 : kSpeakerStereo;
  format.SubFormat = kSubtypeIeeeFloat;

  callback_ = std::make_unique<VoiceCallback>(this);
  hr = xaudio2_->CreateSourceVoice(&source_voice_, &format.Format, 0,
                                   XAUDIO2_DEFAULT_FREQ_RATIO, callback_.get());
  if (FAILED(hr)) {
    REXAPU_ERROR("XAudio2 CreateSourceVoice failed: {:08X}", static_cast<uint32_t>(hr));
    return false;
  }
  hr = source_voice_->Start();
  if (FAILED(hr)) {
    REXAPU_ERROR("XAudio2 source voice Start failed: {:08X}", static_cast<uint32_t>(hr));
    return false;
  }

  REXAPU_INFO("XAudio2 output: endpoint {} ch @ {} Hz, submitting {} ch", details.InputChannels,
              details.InputSampleRate, output_channels_);
  return true;
}

void XAudio2AudioDriver::SubmitFrame(uint32_t frame_ptr) {
  const auto* input = memory_->TranslateVirtual<const float*>(frame_ptr);
  float* output = frames_[next_frame_].data();
  next_frame_ = (next_frame_ + 1) % kFrameCount;

  // Tests: SVR2011_AUDIO_LEVEL=1 logs the game's loudness (front left/right,
  // read before muting) every ~5 s.
  {
    double sum = 0;
    for (uint32_t i = 0; i < 2 * kChannelSamples; ++i) {
      const uint32_t raw = _byteswap_ulong(reinterpret_cast<const uint32_t*>(input)[i]);
      float f;
      std::memcpy(&f, &raw, 4);
      sum += double(f) * f;
    }
    const double mean_square = sum / (2 * kChannelSamples);
    static const bool log_level = [] {
      char* v = nullptr;
      size_t n = 0;
      const bool on = _dupenv_s(&v, &n, "SVR2011_AUDIO_LEVEL") == 0 && v;
      free(v);
      return on;
    }();
    if (log_level) {
      log_sum_ += mean_square;
      if (++log_frames_ == 940) {  // ~5 s
        REXAPU_INFO("audio level: rms {:.5f}", std::sqrt(log_sum_ / 940.0));
        log_sum_ = 0;
        log_frames_ = 0;
      }
    }
  }

  if (AudioMuted()) {
    std::memset(output, 0, output_channels_ * kChannelSamples * sizeof(float));
  } else if (output_channels_ == 6) {
    rex::audio::conversion::sequential_6_BE_to_interleaved_6_LE(
        output, input, kChannelSamples, rex::audio::GetSurroundMix(),
        rex::audio::GetOutputGain());
  } else {
    rex::audio::conversion::sequential_6_BE_to_interleaved_2_LE(
        output, input, kChannelSamples, rex::audio::GetStereoFold(),
        rex::audio::GetOutputGain());
  }

  XAUDIO2_BUFFER buffer = {};
  buffer.AudioBytes = output_channels_ * kChannelSamples * sizeof(float);
  buffer.pAudioData = reinterpret_cast<const BYTE*>(output);
  HRESULT hr = source_voice_->SubmitSourceBuffer(&buffer);
  if (FAILED(hr)) {
    // Keep the guest's audio thread moving even if the device went away.
    REXAPU_ERROR("XAudio2 SubmitSourceBuffer failed: {:08X}", static_cast<uint32_t>(hr));
    semaphore_->Release(1, nullptr);
  }
}

void XAudio2AudioDriver::OnBufferEnd() {
  semaphore_->Release(1, nullptr);
}

void XAudio2AudioDriver::Shutdown() {
  if (source_voice_) {
    source_voice_->Stop();
    source_voice_->DestroyVoice();
    source_voice_ = nullptr;
  }
  if (mastering_voice_) {
    mastering_voice_->DestroyVoice();
    mastering_voice_ = nullptr;
  }
  if (xaudio2_) {
    xaudio2_->Release();
    xaudio2_ = nullptr;
  }
  callback_.reset();
  if (com_initialized_) {
    CoUninitialize();
    com_initialized_ = false;
  }
}

XAudio2AudioSystem::XAudio2AudioSystem(rex::runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

XAudio2AudioSystem::~XAudio2AudioSystem() = default;

bool XAudio2AudioSystem::IsAvailable() {
  const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  IXAudio2* xaudio2 = nullptr;
  IXAudio2MasteringVoice* voice = nullptr;
  bool ok = SUCCEEDED(XAudio2Create(&xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR)) &&
            SUCCEEDED(xaudio2->CreateMasteringVoice(&voice));
  if (voice) voice->DestroyVoice();
  if (xaudio2) xaudio2->Release();
  if (com) CoUninitialize();
  return ok;
}

std::unique_ptr<rex::audio::AudioSystem> XAudio2AudioSystem::Create(
    rex::runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<XAudio2AudioSystem>(function_dispatcher);
}

X_STATUS XAudio2AudioSystem::CreateDriver(size_t, rex::thread::Semaphore* semaphore,
                                          rex::audio::AudioDriver** out_driver) {
  auto* driver = new XAudio2AudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    driver->Shutdown();
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void XAudio2AudioSystem::DestroyDriver(rex::audio::AudioDriver* driver) {
  auto* xdriver = static_cast<XAudio2AudioDriver*>(driver);
  xdriver->Shutdown();
  delete xdriver;
}

}  // namespace svr2011
