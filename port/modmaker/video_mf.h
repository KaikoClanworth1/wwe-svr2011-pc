// A video file's frames through Media Foundation (the Full preview's titantron when it isn't a Bink 1 movie: a
// mod's movie.mp4). Frames come out as BGRA; FrameAt decodes forward and seeks when going back or far ahead.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mm {
namespace video_mf {

class Reader {
 public:
  Reader();
  ~Reader();
  // false (why in err) when the file can't be read, or Media Foundation isn't there (Windows "N")
  bool Open(const std::wstring& file, std::string& err);
  int Width() const { return w_; }
  int Height() const { return h_; }
  double Seconds() const { return seconds_; }
  // the frame shown at `t` seconds (looped over the length) -> bgra (w * h * 4); false: nothing new / failed
  bool FrameAt(double t, std::vector<uint8_t>& bgra);

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
  int w_ = 0, h_ = 0;
  double seconds_ = 0;
};

}  // namespace video_mf
}  // namespace mm
