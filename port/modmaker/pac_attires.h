// Attires inside a character model pac (EPK8): its EMD entries are named
// "%06d%02d" - the character id, then attire * 10 + kind (kind 2 the
// wrestler model, 4 an entrance outfit, ...). The game uses a superstar mod's
// in-pac attires contiguous from 1, at most 4 (Superstar Porting). Edits keep
// every other entry and the table's row count (removed rows become empty).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "svrfmt/pac.h"

namespace mm {

// The attires (1-based) with a kind-2 entry, sorted.
std::vector<int> PacAttires(const svrfmt::Epac& pac);
// The kinds an attire has ("2", "4", ...), for showing.
std::vector<int> PacAttireKinds(const svrfmt::Epac& pac, int attire);
// Attire `to` of `dst` becomes attire `from` of `src`: dst's entries of that
// attire are dropped, src's are copied in renamed to dst's character id.
bool PacCopyAttire(svrfmt::Epac& dst, int to, const svrfmt::Epac& src, int from, std::string* error);
// Attire `a` removed; the later ones move down one (the game counts them contiguous).
bool PacRemoveAttire(svrfmt::Epac& pac, int a, std::string* error);
// Only attire `a`, as attire 1 (an attire mod's attire.pac from any attire of a pac).
bool PacOnlyAttire(svrfmt::Epac& pac, int a, std::string* error);

// Entrance gear: attire N's kind-4 entry "<id><N-1>4" - the whole wrestler wearing the entrance outfit (coat, hat,
// shirt), worn for the entrance instead of kind 2 when it exists (HCTP Entrance Porting, docs/ENTRANCE_ATTIRES.md 4).
std::vector<int> PacGearAttires(const svrfmt::Epac& pac);  // the attires with gear, sorted
bool PacRemoveGear(svrfmt::Epac& pac, int attire, std::string* error);
// Attire `to` of dst gets attire `from`'s gear of src (dst's own gear for `to` replaced).
bool PacCopyGear(svrfmt::Epac& dst, int to, const svrfmt::Epac& src, int from, std::string* error);
// The gear choices per attire ("off" leaves it out, "from<k>" gives it attire k's gear of the same pac), applied.
bool PacApplyGear(svrfmt::Epac& pac, const std::map<int, std::string>& ops, std::string* error);

}  // namespace mm
