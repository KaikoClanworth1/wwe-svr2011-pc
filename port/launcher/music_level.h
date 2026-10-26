/* WWE SmackDown vs. Raw 2011 PC launcher - music volume levelling (music_level.c): a mod's theme / song measured
   (ITU-R BS.1770, as the Mod Maker's theme volume meter: modmaker/loudness.cpp) and its gain written so it lands
   at the Mod Maker's target, -24 LUFS. The audio is never re-encoded: the game applies the gain (theme_gain=). */
#pragma once

#include <windows.h>

#define LEVEL_TARGET_LUFS (-24.0)   /* (modmaker/app.h kThemeTargetLufs) */

/* A song's integrated loudness (Media Foundation decodes it). 1 on success; why: why not. */
int level_measure(const WCHAR *file, double *lufs, WCHAR *why, size_t why_n);
/* The gain (dB, -20..+6, one decimal) that puts a song of that loudness at the target. */
double level_gain_for(double lufs);

/* One mod folder (Mods\Superstars\<id>: song= -> theme_gain=; Mods\Media\<id>: theme.<n>= -> theme_gain.<n>=).
   Only a gain the author left out, or one this wrote (theme_gain_auto=<size>:<time> of the song it measured;
   theme_gain_auto.<n>= for media) when the song changed since, is measured: an author's own theme_gain= stays.
   Appends "<song>: <dB>" lines to report. -> songs levelled, -1 when the manifest couldn't be written. */
int level_mod(const WCHAR *folder, WCHAR *report, size_t report_n);
/* The Music folder's own songs (the jukebox MY MUSIC / CAE USER PLAYLIST): their gains into <music>\.levels.txt
   ("<file name>\t<dB>", the game's music.cpp reads it), each measured once (kept by size and time). Mods' theme
   copies there are left to their mods. -> songs levelled. */
int level_music_folder(const WCHAR *music, WCHAR *report, size_t report_n);
