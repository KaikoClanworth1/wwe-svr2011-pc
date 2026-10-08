/* WWE SmackDown vs. Raw 2011 PC launcher - custom main menu backgrounds.
 *
 * A picture (anything Windows can read: PNG, JPG, BMP, WebP with its codec)
 * becomes a Background mod: <game>/Mods/Backgrounds/<id>/ with picture.dds
 * (1024 x 512 DXT5, the size and format of the game's own menu pictures) and
 * manifest.txt (type=Background, id, name, logo=0|1). The Mods tab lists it
 * (tick on / off, - removes it); the game shows it after its own 12
 * (src/backgrounds_page.cpp). logo=1 keeps the game's "SmackDown vs RAW 2011"
 * logo over it (fan wallpapers usually have their own).
 */
#pragma once

#include <windows.h>

/* Makes the mod from `image` (name: shown in the game; NULL or "": the file's
 * name). 1 on success; err says why not. out_dir (MAX_PATH, may be NULL)
 * gets the mod's folder. */
int bg_make(const WCHAR *game_dir, const WCHAR *image, const WCHAR *name, int logo, WCHAR *out_dir, WCHAR *err,
            size_t errn);
