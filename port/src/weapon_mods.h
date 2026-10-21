// WWE SmackDown vs. Raw 2011 - weapon mods: new weapons for matches and backstage areas.
//
// A weapon mod is Mods/Weapons/<folder>/ with
//   weapon.pac    the weapon as the game keeps one: a PACH (model JBOY 0x0, texture set 0xA, ...) like a
//                 bgEtc.pac STG/WPON entry - made by the converters (another game's weapon, a PS2 model) or
//                 the Mod Maker
//   manifest.txt  name=<shown name>, like=<a stock weapon's id> (its settings - grip type, sounds, weight,
//                 hits to break, damage, hit spheres - are copied), and pool=extreme / backstage (where it
//                 may be chosen; both when not given)
//   disabled      (optional) the mod is off
// Each mod gets a weapon id of its own (160-255: one byte, as the placed lists keep them), kept in
// Mods/Weapons/ids.txt so it stays the same across starts. At start the game's bgEtc.pac is read and its
// weapon tables - STG/WPON, STG/WPBS and STG3/WPON - are made again in memory with the mods' packs added
// (and a record each in the weapon settings table, 0x9C41); the game's look-ups in its own tables are
// pointed at those (weapon_mods.cpp). The game's file is never written, nor a copy of it.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

struct WeaponMod {
  std::string folder, name;
  int id = 0;    // its weapon id (160-255)
  int like = 0;  // the stock weapon whose settings it has
  int kb = 0;    // its pack's size (KB): what loading it takes, roughly
  bool extreme = true, backstage = true;  // pool=
};

// Reads Mods/Weapons and makes the tables with the mods (at start).
void SetWeaponModsMemory(rex::memory::Memory* memory);
void LoadWeaponMods();
// The mods that loaded (by id).
const std::vector<WeaponMod>& WeaponMods();
// A mod's id by its folder name ("" / unknown: 0) - backstage load_weapons= / weapons= may name one.
int WeaponModId(const std::string& folder);

}  // namespace svr2011
