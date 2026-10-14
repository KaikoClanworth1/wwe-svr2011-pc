// A song played from any point (the Full preview's theme): decoded whole with
// Media Foundation (any file it reads: mp3, m4a, wav, wma, ...), played with XAudio2.
// One song at a time; every call is from the UI thread except the decode's own worker.
#pragma once

#include <string>

namespace audio_out {

// Starts decoding `file` in the background (the one before is dropped). "" = none.
void Load(const std::wstring& file);
// 0 decoding / nothing, 1 ready, -1 failed (Error says why).
int State();
std::string Error();
double Length();  // seconds (0 until ready)
// Plays from `at` seconds (also while playing: a jump); past the end: silence.
void Play(double at);
void Pause();
bool Playing();
void Unload();  // stops and frees it
// No sound out at all (hidden test runs): Play does nothing.
void SetMuted(bool muted);

}  // namespace audio_out
