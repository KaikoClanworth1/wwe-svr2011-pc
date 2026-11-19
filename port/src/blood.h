// WWE SmackDown vs. Raw 2011 - blood on every superstar (report e4dec0).
//
// A wrestler bleeds through two models of its attire's PACH: 30000 (0x7530,
// blood_face) and 30001 (0x7531, blood_body). They are the skin itself, lifted
// 0.0023 / 0.002 along the normal, drawn with yDefault and the textures named
// bloodface / bloodbody - the game's blood object (sub_822032F8) finds the two
// children and swaps in the shared BloodFace<n>_<stage> / BloodBody<n>_<stage>
// (misc.pac BATS/INIT child 110). A model without them never shows blood.
// Their UVs are a front projection, the same on every stock superstar:
// u = 0.5 + 0.44 x, v = 0.44 (y - head top) - 0.11 (face); u = 0.53 + 0.19 x,
// v = 0.24 (y - head top) - 0.8 (body).
//
// Ported mods often have none (left out for the vertex budget, or never made)
// or a template's (another superstar's head: the blood floats or hides). With
// blood_all on, GiveBlood makes them from the attire's own skin.
//
// Whether anyone bleeds at all stays the game's own GAMEPLAY OPTIONS -> BLOOD
// (blood.cpp: with it OFF only rare near-head hits bleed; the log says so at a
// match load). Divas and non-wrestlers have no overlays on the disc - left so.
#pragma once

#include <filesystem>
#include <string>

#include "../modmaker/svrfmt/bytes.h"

namespace svr2011 {

// blood_all: every superstar can bleed (mods' blood overlays made where missing or not fitting).
bool BloodForAll();

// The attires (EMD kind 2 entries) of a character model pac: those without blood overlays, or with ones that
// don't fit their skin, get their own. `game` = the game folder (the stock template pac/ch/ch100.pac). -> the
// pac changed; `notes` gets what was done.
bool GiveBlood(svrfmt::Bytes& pac, const std::filesystem::path& game, std::string& notes);

}  // namespace svr2011
