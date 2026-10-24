// The move inspector and "Will it work in-game?" (move_check.h, docs/MOVE_MODDING.md).
#include "move_check.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>

#include <imgui.h>

#include "app.h"
#include "char_preview.h"
#include "svrfmt/pac.h"

namespace mm::move_check {
namespace fs = std::filesystem;
using namespace svrfmt;

namespace {

std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.pop_back();
  return s;
}

// A row of the pack's per-key data (exh / evt / mbd: <group> <hex...>) is this move's when its record starts with
// the move id (u16, little-endian).
bool RowOf(const std::string& hex, int id) {
  if (hex.size() < 4) return false;
  const int v = int(std::strtoul(hex.substr(0, 2).c_str(), nullptr, 16)) | int(std::strtoul(hex.substr(2, 2).c_str(), nullptr, 16)) << 8;
  return v == id;
}

// The game's move records (misc.pac MOVS/WAZE: 160-byte records after a 272-byte head, id at +0x90, category bits
// +0..+15): id -> has bits. Read once.
const std::map<int, bool>& StockRecords() {
  static std::map<int, bool> rec;
  static std::wstring read_for;
  if (read_for == g_game) return rec;
  read_for = g_game, rec.clear();
  Bytes raw;
  if (!ReadPacEntry(fs::path(g_game) / L"pac" / L"misc.pac", "WAZE", raw, "MOVS")) return rec;
  const Bytes w = Unpack(raw);
  if (w.size() < 272) return rec;
  const uint32_t n = uint32_t(w[4]) | uint32_t(w[5]) << 8 | uint32_t(w[6]) << 16 | uint32_t(w[7]) << 24;
  for (uint32_t i = 0; i < n && 272 + size_t(i + 1) * 160 <= w.size(); ++i) {
    const uint8_t* r = &w[272 + size_t(i) * 160];
    const int mid = r[0x90] | r[0x91] << 8;
    bool bits = false;
    for (int k = 0; k < 16; ++k) bits |= r[k] != 0;
    rec[mid] = bits;
  }
  return rec;
}

// pack.txt frames against the file's own: a motion's stream (records and skips: one frame each).
int StreamFrames(const Bytes& d) {
  int frames = 0;
  for (size_t p = 0; p < d.size();) {
    const uint8_t c = d[p];
    if (c < 0x60) { frames += c, ++p; continue; }
    size_t sz;
    if (c == 0x60) {
      if (p + 3 > d.size()) break;
      sz = size_t(d[p + 1]) | size_t(d[p + 2]) << 8, p += 3;
    } else {
      sz = c - 0x60u, ++p;
    }
    p += sz, ++frames;
  }
  return frames;
}

}  // namespace

Info Inspect(const fs::path& pack, int id) {
  Info info;
  info.id = id;
  Bytes t;
  if (!ReadFile(PathStr(pack / L"pack.txt"), t)) {
    info.findings.push_back({true, "The pack has no pack.txt.", "Open the folder that holds pack.txt and motions\\."});
    return info;
  }
  std::istringstream in(std::string(t.begin(), t.end()));
  std::string line, bits;
  int copy_of = 0;
  const std::string id_s = std::to_string(id);
  while (std::getline(in, line)) {
    line = Trim(line);
    std::istringstream w(line);
    std::string kind;
    w >> kind;
    if (kind == "#") {  // (notes: the ones naming this move)
      if ((" " + line + " ").find(" " + id_s) != std::string::npos) info.source.push_back(line.substr(2));
    } else if (kind == "motion") {
      Track tr;
      std::string pac, where;
      int mid = 0;
      w >> pac >> where >> mid >> tr.x >> tr.y >> tr.frames >> tr.file;
      if (mid != id) continue;
      tr.bank = pac + " " + where;
      std::error_code ec;
      tr.file_ok = fs::exists(pack / fs::u8path(tr.file), ec);
      info.tracks.push_back(tr);
    } else if (kind == "waze" || kind == "wazename" || kind == "wazecopy") {
      int mid = 0;
      w >> mid;
      if (mid != id) continue;
      std::string rest;
      std::getline(w, rest);
      while (!rest.empty() && rest.front() == ' ') rest.erase(0, 1);
      if (kind == "waze") bits = rest;
      if (kind == "wazename") info.name = rest;
      if (kind == "wazecopy") copy_of = std::atoi(rest.c_str());
    } else if (kind == "exh" || kind == "evt" || kind == "mbd") {
      std::string group, hex;
      w >> group >> hex;
      if (!RowOf(hex, id)) continue;
      std::string rest;
      std::getline(w, rest);
      (kind == "exh" ? info.exh : kind == "evt" ? info.evt : info.mbd).push_back(group + " " + hex + rest);
    }
  }
  // the porter's report beside the pack (moves_port.py convert: <out>/report.json)
  for (const fs::path& rj : {pack / L"report.json", pack.parent_path() / L"report.json"}) {
    Bytes r;
    if (ReadFile(PathStr(rj), r) && r.size() < 64 * 1024) {
      info.source.push_back("porter's report: " + std::string(r.begin(), r.end()));
      break;
    }
  }
  // the record
  const auto& stock = StockRecords();
  bool has_bits = false;
  for (char c : bits) has_bits |= c != '0' && c != ' ';
  if (!bits.empty()) info.record = "its own (waze " + bits + ")", info.usable = has_bits;
  else if (copy_of) {
    const auto it = stock.find(copy_of);
    info.record = "a copy of move " + std::to_string(copy_of) + "'s";
    info.usable = it != stock.end() && it->second;
  } else if (const auto it = stock.find(id); it != stock.end()) {
    info.record = "the game's", info.usable = it->second;
  } else {
    info.record = "none";
  }
  // the tracks as the preview (and the game) read them
  const auto bank = anims_page::PackBank(pack, id);
  std::set<int> xs;
  for (Track& tr : info.tracks) {
    if (tr.y > 1) continue;  // (cameras, props: not checked here)
    xs.insert(tr.x);
    Bytes d;
    if (tr.file_ok && ReadFile(PathStr(pack / fs::u8path(tr.file)), d)) {
      const int sf = StreamFrames(d);
      if (sf != tr.frames && std::abs(sf - tr.frames) > 1)
        info.findings.push_back({false, "Track x " + std::to_string(tr.x) + " y " + std::to_string(tr.y) + ": pack.txt says " +
                                            std::to_string(tr.frames) + " frames, the file has " + std::to_string(sf) + ".",
                                 "Port the move again (the porter writes both), or set the frames in pack.txt to the file's."});
    }
    if (!bank) continue;
    const auto f = char_preview::InspectTrack(*bank, id, tr.x, tr.y);
    if (!f.ok) {
      tr.decode_err = f.err;
      continue;
    }
    tr.keys = f.keys, tr.next = f.next, tr.attack = f.attack, tr.ops = f.ops;
  }
  info.phases.assign(xs.begin(), xs.end());
  {  // one row per track: a motion is copied into several banks (BM / RR / MOTP / MVMT) - their names joined
    std::vector<Track> merged;
    for (const Track& tr : info.tracks) {
      auto it = std::find_if(merged.begin(), merged.end(), [&](const Track& m) { return m.x == tr.x && m.y == tr.y; });
      if (it == merged.end()) {
        merged.push_back(tr);
        continue;
      }
      it->bank += ", " + tr.bank;
      if (!tr.file_ok && it->file_ok) it->file_ok = false, it->file = tr.file;
    }
    info.tracks = std::move(merged);
  }
  // the moveset slots that name it (superstar mods' moves.txt)
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(fs::path(g_game) / L"Mods" / L"Superstars", ec)) {
    Bytes m;
    if (!ReadFile(PathStr(e.path() / L"moves.txt"), m)) continue;
    std::istringstream ms(std::string(m.begin(), m.end()));
    while (std::getline(ms, line)) {
      const size_t eq = line.find('=');
      if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
      if (std::strtol(line.c_str() + eq + 1, nullptr, 0) != id) continue;
      const long off = std::strtol(line.c_str(), nullptr, 0);
      info.slots.push_back(Utf8(e.path().filename().wstring()) + ": " + star_page::SlotName(int(off / 2)));
    }
  }

  // ---- the check: what the game needs to play it
  auto err = [&](const std::string& what, const std::string& fix) { info.findings.push_back({true, what, fix}); };
  auto warn = [&](const std::string& what, const std::string& fix) { info.findings.push_back({false, what, fix}); };
  bool any_body = false;
  for (const Track& tr : info.tracks) any_body |= tr.y <= 1;
  if (!any_body) err("The move has no body motion (no y 0 / y 1 track) in the pack.", "Port it again; a move needs at least the attacker's track (y 0).");
  if (!info.usable)
    err(info.record == "none" ? "The move has no move record: a moveset can't name it and the CPU never picks it."
                              : "The move's record has no category bits: the game treats it as unusable.",
        "Port it with a template (the porter's --template: a 2011 move of the same kind gives the record), or add a "
        "wazecopy line from a stock move of the same kind.");
  for (const Track& tr : info.tracks)
    if (tr.y <= 1 && !tr.file_ok)
      err("Motion file " + tr.file + " is missing.", "The game counts the move as unusable without it: copy the pack's "
                                                       "motions\\ folder in full, or port the move again.");
  // the partner: once a phase pairs (y 1), every later phase under 100 needs the partner's track too
  bool paired = false;
  for (int x : info.phases) {
    if (x >= 100) continue;
    bool y1 = false;
    for (const Track& tr : info.tracks) y1 |= tr.x == x && tr.y == 1;
    if (paired && !y1)
      err("Phase x " + std::to_string(x) + " has no partner motion (y 1) after a paired phase.",
          "The partner would freeze in place: port the move again, or pick a template whose phases match.");
    paired |= y1;
  }
  // follow-ups: the motion each track goes on to must exist (the pack's, or the game's)
  for (const Track& tr : info.tracks) {
    if (tr.next <= 0) continue;
    if (!anims_page::MovesIndexed()) {
      warn("Follow-up " + std::to_string(tr.next) + " of x " + std::to_string(tr.x) + " y " + std::to_string(tr.y) +
               " not checked (m.pac is still being indexed).", "Check again in a moment.");
      continue;
    }
    if (!anims_page::HasMotion(tr.next))
      err("Track x " + std::to_string(tr.x) + " y " + std::to_string(tr.y) + " goes on to motion " + std::to_string(tr.next) +
              ", which the game doesn't have.",
          tr.y == 1 ? "The victim would stand up at once or freeze: port it again, or set its last event (op 49) to a down "
                      "motion (443-448, 473-478)."
                    : "Port it again, or set its last event (op 49) to 0 (back to the stance).");
  }
  // timing
  for (const Track& tr : info.tracks) {
    if (tr.y > 1) continue;
    if (tr.frames <= 0) err("Track x " + std::to_string(tr.x) + " y " + std::to_string(tr.y) + " is 0 frames long.", "Port it again.");
    else if (tr.frames > 1800)
      warn("Track x " + std::to_string(tr.x) + " y " + std::to_string(tr.y) + " is " + std::to_string(tr.frames) +
               " frames (over a minute in a match).", "A timing scale too high, or the wrong motion: check it in the preview.");
  }
  for (int x : info.phases) {
    int a = 0, b = 0;  // (the attacker's and the partner's frames in this phase)
    for (const Track& tr : info.tracks) {
      if (tr.x == x && tr.y == 0) a = std::max(a, tr.frames);
      if (tr.x == x && tr.y == 1) b = std::max(b, tr.frames);
    }
    if (a > 0 && b > 0 && (a > 3 * b || b > 3 * a))
      warn("Phase x " + std::to_string(x) + ": the attacker's track is " + std::to_string(a) + " frames, the partner's " +
               std::to_string(b) + ".", "One of them likely isn't the move's: watch it - the shorter one holds its last pose.");
  }
  // kinds the game (and the preview) can't read
  for (const Track& tr : info.tracks)
    if (tr.y <= 1 && !tr.decode_err.empty() && tr.decode_err.find("layout") != std::string::npos)
      err("Track x " + std::to_string(tr.x) + " y " + std::to_string(tr.y) + " is a kind this game doesn't read (" +
              tr.decode_err + ").", "Port it again with a template in the same bank kind (a match move: YMKs).");
  {  // (each finding once)
    std::vector<Finding> once;
    for (const Finding& f : info.findings)
      if (std::none_of(once.begin(), once.end(), [&](const Finding& o) { return o.what == f.what; })) once.push_back(f);
    info.findings = std::move(once);
  }
  return info;
}

std::string Report(const Info& i) {
  std::ostringstream o;
  o << "Move " << i.id << (i.name.empty() ? "" : " (" + i.name + ")") << "\n";
  for (const auto& s : i.source) o << "  source: " << s << "\n";
  o << "  record: " << i.record << (i.usable ? " - usable" : " - NOT usable") << "\n";
  o << "  phases:";
  for (int x : i.phases) o << " " << x;
  o << "\n";
  for (const Track& t : i.tracks) {
    o << "  track x " << t.x << " y " << t.y << ": " << t.frames << " frames, " << t.file << (t.file_ok ? "" : " (MISSING)") << ", "
      << t.bank;
    if (t.keys) o << ", " << t.keys << " keys";
    if (t.next >= 0) o << ", then " << t.next;
    if (!t.attack.empty()) {
      o << ", hits";
      for (const auto& [k, m] : t.attack) o << " " << m << "@" << k;
    }
    if (!t.ops.empty()) {
      o << ", events";
      for (const auto& [op, n] : t.ops) o << " " << op << "x" << n;
    }
    if (!t.decode_err.empty()) o << " (not read: " << t.decode_err << ")";
    o << "\n";
  }
  for (const auto& r : i.exh) o << "  exh " << r << "\n";
  for (const auto& r : i.evt) o << "  evt " << r << "\n";
  for (const auto& r : i.mbd) o << "  mbd " << r << "\n";
  for (const auto& s : i.slots) o << "  in moveset: " << s << "\n";
  int errors = 0;
  for (const auto& f : i.findings) errors += f.error;
  o << "Will it work in-game? " << (errors ? "No" : i.findings.empty() ? "Yes" : "Yes, with warnings") << "\n";
  for (const auto& f : i.findings) o << "  " << (f.error ? "ERROR " : "warning ") << f.what << " Fix: " << f.fix << "\n";
  return o.str();
}

void DrawPanel(const fs::path& pack, int id) {
  static fs::path for_pack;
  static int for_id = -1;
  static Info info;
  if (!ImGui::CollapsingHeader(("Inspect and check move " + std::to_string(id) + "##mc").c_str())) return;
  if (for_pack != pack || for_id != id || ImGui::SmallButton("Check again")) info = Inspect(pack, id), for_pack = pack, for_id = id;
  int errors = 0;
  for (const auto& f : info.findings) errors += f.error;
  ImGui::Text("Will it work in-game? %s", errors ? "No" : info.findings.empty() ? "Yes" : "Yes, with warnings");
  for (const auto& f : info.findings) {
    ImGui::TextColored(f.error ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(1, 0.8f, 0.3f, 1), "%s %s", f.error ? "Error:" : "Warning:", f.what.c_str());
    ImGui::TextWrapped("    Fix: %s", f.fix.c_str());
  }
  if (ImGui::TreeNode("What the move is")) {
    ImGui::TextUnformatted(Report(info).c_str());
    ImGui::TreePop();
  }
  if (ImGui::SmallButton("Watch it")) {
    std::string err;
    const auto bank = anims_page::PackBank(pack, id);
    if (!bank || !anims_page::PlayMove(bank, id, -1, &err)) Status("That move can't be shown: " + (bank ? err : "its motions didn't read") + ".");
    else Status("Playing move " + std::to_string(id) + ": " + char_preview::MoveSteps() + ".");
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Export report...")) {
    static const COMDLG_FILTERSPEC spec[] = {{L"Text", L"*.txt"}};
    const std::wstring f = PickFile(true, L"Save the move's report", spec, 1, L"txt", (L"move_" + std::to_wstring(id) + L".txt").c_str());
    const std::string r = Report(info);
    if (!f.empty()) Status(WriteFile(Utf8(f), Bytes(r.begin(), r.end())) ? "Saved " + Utf8(f) + "." : "The report could not be written.");
  }
}

// Test aid --move-check <pack>,<id>,<out.txt>: the report written once m.pac is indexed, then the Mod Maker quits.
namespace {
std::wstring g_test;
}
void Test(const std::wstring& spec) { g_test = spec; }
void Tick() {
  if (g_test.empty() || !anims_page::MovesIndexed()) return;
  const size_t a = g_test.find(L','), b = g_test.find(L',', a + 1);
  const fs::path pack = g_test.substr(0, a);
  const int id = _wtoi(g_test.substr(a + 1, b - a - 1).c_str());
  const std::string r = Report(Inspect(pack, id));
  WriteFile(Utf8(g_test.substr(b + 1)), Bytes(r.begin(), r.end()));
  Log("move check: " + std::to_string(id) + " -> " + Utf8(g_test.substr(b + 1)));
  g_test.clear();
  PostMessageW(g_wnd, WM_CLOSE, 0, 0);
}

}  // namespace mm::move_check
