// WWE SmackDown vs. Raw 2011 - XAudio2 audio output.
//
// The guest's audio driver hands over one frame at a time (256 samples x 6
// channels, big-endian float, 48 kHz). Each frame is converted to the output
// layout and queued on an XAudio2 source voice; when XAudio2 finishes playing
// it, the driver semaphore is released, which paces the game's audio thread.
// 5.1 endpoints get the channels as they are, anything else the stereo fold.

#pragma once

#include <array>
#include <atomic>
#include <memory>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/thread.h>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

namespace svr2011 {

using rex::X_STATUS;

class XAudio2AudioDriver : public rex::audio::AudioDriver {
 public:
  XAudio2AudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~XAudio2AudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void Shutdown();

  static constexpr uint32_t kFrequency = 48000;
  static constexpr uint32_t kGuestChannels = 6;
  static constexpr uint32_t kChannelSamples = 256;
  // Frames that may be queued on the voice at once; the guest never has more
  // in flight than its semaphore allows, which is well under this.
  static constexpr uint32_t kFrameCount = 64;

 private:
  class VoiceCallback;
  void OnBufferEnd();

  rex::thread::Semaphore* semaphore_ = nullptr;
  IXAudio2* xaudio2_ = nullptr;
  IXAudio2MasteringVoice* mastering_voice_ = nullptr;
  IXAudio2SourceVoice* source_voice_ = nullptr;
  std::unique_ptr<VoiceCallback> callback_;
  bool com_initialized_ = false;
  uint32_t output_channels_ = 2;
  uint32_t next_frame_ = 0;
  double log_sum_ = 0;     // SVR2011_AUDIO_LEVEL
  uint32_t log_frames_ = 0;
  std::array<std::array<float, kGuestChannels * kChannelSamples>, kFrameCount> frames_{};
};

class XAudio2AudioSystem : public rex::audio::AudioSystem {
 public:
  explicit XAudio2AudioSystem(rex::runtime::FunctionDispatcher* function_dispatcher);
  ~XAudio2AudioSystem() override;

  // True when XAudio2 can open an output device on this machine.
  static bool IsAvailable();

  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* function_dispatcher);

 protected:
  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        rex::audio::AudioDriver** out_driver) override;
  void DestroyDriver(rex::audio::AudioDriver* driver) override;
};

}  // namespace svr2011
