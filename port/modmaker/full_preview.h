// The Full preview (checklist G6): a window inside the Mod Maker that plays a whole entrance - its five
// segments one after another - with the superstar in the practice arena (pac\bg\bg29.pac), seen through
// the entrance's own camera track, its fingers (the entrant's y2 / y3 tracks), the theme music and the
// titantron movie. Play, pause, scrub, restart. Any entrance a project can hold: its own move pack's,
// an installed pack's, the game's (pac\evt\Nyujyo*.pac).
#pragma once

#include <string>

namespace mm {
namespace full_preview {

struct Request {
  int entrance = -1;          // the entrance number
  int star = -1;              // the superstar it is for (the style's: its profile's theme / titantron when the mod has none)
  std::wstring model;         // the superstar's ch.pac
  int attire = 1;             // the attire shown (1-4)
  bool gear = false;          // in its entrance gear (EMD kind 4: char_preview::SetGear)
  std::wstring song, movie;   // the mod's own theme / titantron ("": the superstar's profile's)
  std::wstring pack;          // the project's own move pack folder ("" = none): looked in first
  std::string title;          // the window's subtitle (the superstar's name)
};

void Open(const Request& r);
bool IsOpen();
void Close();
// Every frame, inside the Mod Maker's window (draws the window while open).
void Draw();
// Test aid --full-preview <entrance>,<star>[,<key>[,<camera 0/1>]]: opens with that superstar's model;
// with a key, paused there (for --shot).
void TestOpen(int entrance, int star, float key, bool camera);
// Test aid --full-preview-song <file>: the theme the test opens with (else the profile's).
void TestSong(const std::wstring& file);

}  // namespace full_preview
}  // namespace mm
