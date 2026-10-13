// Attires inside a character model pac (pac_attires.h).
#include "pac_attires.h"

#include <algorithm>
#include <cctype>

namespace mm {

using namespace svrfmt;

namespace {

bool IsEmd(const EpacGroup& g) { return g.type.rfind("EMD", 0) == 0; }
bool Named(const EpacEntry& e) { return e.name.size() >= 8 && std::isdigit(uint8_t(e.name[6])) && std::isdigit(uint8_t(e.name[7])); }
int AttireOf(const EpacEntry& e) { return Named(e) ? e.name[6] - '0' + 1 : 0; }

EpacGroup* Emd(Epac& pac) {
  for (auto& g : pac.groups)
    if (IsEmd(g)) return &g;
  return nullptr;
}
const EpacGroup* Emd(const Epac& pac) { return Emd(const_cast<Epac&>(pac)); }

// The character id part of the names ("000139"), from the first named entry.
std::string Prefix(const EpacGroup& g) {
  for (const auto& e : g.entries)
    if (Named(e)) return e.name.substr(0, 6);
  return {};
}

void Clear(EpacEntry& e) {  // (an empty row: the table keeps its size)
  e.name.assign(e.name.size(), '\0');
  e.data.clear();
}

// Entries renamed: attire digit d (0-based) -> d2, for every row of that attire.
void Rename(EpacGroup& g, int from, int to) {
  for (auto& e : g.entries)
    if (AttireOf(e) == from) e.name[6] = char('0' + to - 1);
}

}  // namespace

std::vector<int> PacAttires(const Epac& pac) {
  std::vector<int> v;
  if (const EpacGroup* g = Emd(pac))
    for (const auto& e : g->entries)
      if (Named(e) && e.name[7] == '2' && !e.data.empty()) v.push_back(AttireOf(e));
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

std::vector<int> PacAttireKinds(const Epac& pac, int attire) {
  std::vector<int> v;
  if (const EpacGroup* g = Emd(pac))
    for (const auto& e : g->entries)
      if (AttireOf(e) == attire && !e.data.empty()) v.push_back(e.name[7] - '0');
  std::sort(v.begin(), v.end());
  return v;
}

bool PacCopyAttire(Epac& dst, int to, const Epac& src, int from, std::string* error) {
  auto fail = [&](const char* why) { if (error) *error = why; return false; };
  EpacGroup* dg = Emd(dst);
  const EpacGroup* sg = Emd(src);
  if (!dg || !sg) return fail("not a character model pac (no EMD group)");
  if (to < 1 || to > 9 || from < 1 || from > 9) return fail("attires are numbered 1 to 9");
  std::vector<const EpacEntry*> in;
  for (const auto& e : sg->entries)
    if (AttireOf(e) == from && !e.data.empty()) in.push_back(&e);
  if (in.empty()) return fail("that pac has no such attire");
  std::string prefix = Prefix(*dg);
  if (prefix.empty()) prefix = Prefix(*sg);
  for (auto& e : dg->entries)
    if (AttireOf(e) == to) Clear(e);
  for (const EpacEntry* s : in) {
    EpacEntry e = *s;
    e.name = prefix + char('0' + to - 1) + s->name[7];
    if (dst.wide && e.name.size() < 8) e.name.resize(8, '\0');
    auto row = std::find_if(dg->entries.begin(), dg->entries.end(), [](const EpacEntry& x) { return !Named(x) && x.data.empty(); });
    if (row != dg->entries.end()) *row = std::move(e);
    else dg->entries.push_back(std::move(e));
  }
  // the named rows in name order (as the game's pacs have them), the empty ones after
  std::stable_sort(dg->entries.begin(), dg->entries.end(), [](const EpacEntry& a, const EpacEntry& b) {
    const bool na = Named(a), nb = Named(b);
    return na != nb ? na : na && a.name < b.name;
  });
  return true;
}

bool PacRemoveAttire(Epac& pac, int a, std::string* error) {
  auto fail = [&](const char* why) { if (error) *error = why; return false; };
  EpacGroup* g = Emd(pac);
  if (!g) return fail("not a character model pac (no EMD group)");
  bool any = false;
  for (auto& e : g->entries)
    if (AttireOf(e) == a) Clear(e), any = true;
  if (!any) return fail("the pac has no such attire");
  for (int k = a + 1; k <= 9; ++k) Rename(*g, k, k - 1);
  std::stable_sort(g->entries.begin(), g->entries.end(), [](const EpacEntry& x, const EpacEntry& y) {
    const bool nx = Named(x), ny = Named(y);
    return nx != ny ? nx : nx && x.name < y.name;
  });
  return true;
}

bool PacOnlyAttire(Epac& pac, int a, std::string* error) {
  auto fail = [&](const char* why) { if (error) *error = why; return false; };
  EpacGroup* g = Emd(pac);
  if (!g) return fail("not a character model pac (no EMD group)");
  if (PacAttireKinds(pac, a).empty()) return fail("the pac has no such attire");
  for (auto& e : g->entries)
    if (Named(e) && AttireOf(e) != a) Clear(e);
  Rename(*g, a, 1);
  std::stable_sort(g->entries.begin(), g->entries.end(), [](const EpacEntry& x, const EpacEntry& y) {
    const bool nx = Named(x), ny = Named(y);
    return nx != ny ? nx : nx && x.name < y.name;
  });
  return true;
}

}  // namespace mm
