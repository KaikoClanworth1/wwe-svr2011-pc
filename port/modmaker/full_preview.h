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
  float height = 0;           // a mod's height= (a factor of the superstar's size; 0 = none)
  int start_part = 0;         // the part it starts playing at (0 the first; 4 the walk to the ring)
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
// Test aid --full-preview-movie <file>: the titantron movie the test opens with.
void TestMovie(const std::wstring& file);
// Test aid --full-preview-dump <file>: once loaded, every key from 0 (one a frame): "K key S slot L local-key lift y
// V <view 16> P <proj 0, 5> B <bone x y z>..." (game coordinates, as drawn), then the file is closed.
void TestDump(const std::wstring& file);
// Test aid --full-preview-cameras <A|B>: that camera track set kept (else one at random, as the game).
void TestCameras(const std::wstring& ab);
// Test aid --full-preview-view <16 floats>,<fov degrees>: the view drawn with that camera (a game camera dump's view
// matrix, row vectors) instead of the entrance's.
void TestView(const std::wstring& spec);

}  // namespace full_preview
}  // namespace mm
