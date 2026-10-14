// SvR 2011's hair (gBaH) and body jiggle (gBaM) solvers - docs/HAIR_PHYSICS.md section 7, tools/hair_sim.py.
#include "hair_sim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>

namespace hair {

namespace {
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V& operator+=(V& a, V b) { return a = a + b; }
V& operator-=(V& a, V b) { return a = a - b; }
float Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Len(V a) { return std::sqrt(Dot(a, a)); }
V Norm(V a) {
  const float l = Len(a);
  return l > 0 ? a * (1.0f / l) : a;
}

std::vector<std::string> Tokens(const std::string& line) {
  std::vector<std::string> t;
  std::istringstream in(line);
  for (std::string s; in >> s;) t.push_back(s);
  return t;
}
// The game's line reader: lines split on CR / LF, `;` starts a comment line, tokens split on whitespace.
std::vector<std::vector<std::string>> Lines(const std::string& text) {
  std::vector<std::vector<std::string>> out;
  std::string line;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i < text.size() && text[i] != '\n' && text[i] != '\r') {
      line += text[i];
      continue;
    }
    const size_t first = line.find_first_not_of(" \t");
    if (first != std::string::npos && line[first] != ';') {
      auto t = Tokens(line);
      if (!t.empty()) out.push_back(std::move(t));
    }
    line.clear();
  }
  return out;
}
bool Num(const std::string& s, float& v) {
  char* end = nullptr;
  v = std::strtof(s.c_str(), &end);
  return end && end != s.c_str() && *end == 0;
}

// forces (8216B3B0) on positions P, velocities Vel
void Forces(const Chain& c, const std::vector<V>& P, const std::vector<V>& Vel, std::vector<V>& F) {
  const size_t n = P.size();
  F.assign(n, V{});
  for (size_t j = 0; j < n; ++j) F[j] = Vel[j] * -c.p.visc, F[j].y += c.p.grav;
  for (size_t j = 0; j + 1 < n; ++j) {
    V d = P[j] - P[j + 1];
    float l = Len(d);
    if (l < 1e-4f) d.y = 1e-4f, l = 1e-4f;
    const V dv = Vel[j] - Vel[j + 1];
    const float s = (c.p.spring * (l - c.L[j]) + c.p.damp * Dot(dv, d) / l) / l;
    F[j] -= d * s;
    F[j + 1] += d * s;
  }
}
}  // namespace

V Apply(const Rigid& m, V p) {
  return {m.r[0][0] * p.x + m.r[0][1] * p.y + m.r[0][2] * p.z + m.t.x, m.r[1][0] * p.x + m.r[1][1] * p.y + m.r[1][2] * p.z + m.t.y,
          m.r[2][0] * p.x + m.r[2][1] * p.y + m.r[2][2] * p.z + m.t.z};
}
V Rotate(const Rigid& m, V v) {
  return {m.r[0][0] * v.x + m.r[0][1] * v.y + m.r[0][2] * v.z, m.r[1][0] * v.x + m.r[1][1] * v.y + m.r[1][2] * v.z,
          m.r[2][0] * v.x + m.r[2][1] * v.y + m.r[2][2] * v.z};
}
V Unapply(const Rigid& m, V p) {
  const V d = p - m.t;
  return {m.r[0][0] * d.x + m.r[1][0] * d.y + m.r[2][0] * d.z, m.r[0][1] * d.x + m.r[1][1] * d.y + m.r[2][1] * d.z,
          m.r[0][2] * d.x + m.r[1][2] * d.y + m.r[2][2] * d.z};
}

std::vector<ChainParams> ParseChains(const std::string& text) {
  std::vector<ChainParams> out;
  for (const auto& t : Lines(text)) {
    if (out.size() >= 16 || t.size() < 10) continue;
    ChainParams c;
    c.root = t[0];
    float v[9];
    bool ok = true;
    for (int k = 0; k < 9; ++k) ok &= Num(t[size_t(k) + 1], v[k]);
    if (!ok) continue;
    c.radius = v[0], c.visc = v[1], c.grav = v[2], c.spring = v[3], c.damp = v[4], c.A = int(v[5]), c.lencorr = v[6];
    c.bend = v[7], c.movecorr = v[8];
    out.push_back(c);
  }
  return out;
}

std::vector<CapsuleParams> ParseCapsules(const std::string& text) {
  std::vector<CapsuleParams> out;
  for (const auto& t : Lines(text)) {
    if (out.size() >= 14 || t.size() < 9) continue;
    float v[8];
    bool ok = true;
    for (int k = 0; k < 8; ++k) ok &= Num(t[size_t(k) + 1], v[k]);
    if (!ok) continue;
    CapsuleParams c;
    c.bone = t[0];
    c.p0 = {v[0], v[1], v[2]}, c.r0 = v[3], c.p1 = {v[4], v[5], v[6]}, c.r1 = v[7];
    out.push_back(c);
  }
  return out;
}

std::vector<MuscleRow> ParseMuscles(const std::string& text) {
  std::vector<MuscleRow> out;
  for (const auto& t : Lines(text)) {
    if (out.size() >= 9 || t.size() < 8) continue;
    float v[8];
    bool ok = true;
    for (int k = 0; k < 8; ++k) ok &= Num(t[size_t(k)], v[k]);
    if (!ok) continue;
    MuscleRow r;
    r.visc = v[0], r.grav = v[1], r.spring = v[2], r.damp = v[3], r.time = v[4], r.lim[0] = v[5], r.lim[1] = v[6], r.lim[2] = v[7];
    out.push_back(r);
  }
  return out;
}

// RK4 with h = 1 (8216B5E8): the trial state accumulates from stage to stage (copied exactly).
void Rk4(Chain& c) {
  const size_t n = c.n.size();
  std::vector<V> P(n), Vel(n), F, AV, AF, TV(n), TP(n), F2;
  for (size_t j = 0; j < n; ++j) P[j] = c.n[j].pos, Vel[j] = c.n[j].vel;
  Forces(c, P, Vel, F);
  AV = Vel, AF = F;
  for (size_t j = 0; j < n; ++j) {
    TV[j] = Vel[j] + F[j] * 0.5f;
    TP[j] = c.n[j].pinned ? P[j] : P[j] + Vel[j] * 0.5f;
  }
  const float wa[3] = {2, 2, 1}, wt[3] = {0.5f, 1.0f, 0};
  for (int k = 0; k < 3; ++k) {
    Forces(c, TP, TV, F2);
    const std::vector<V> V2 = TV;
    for (size_t j = 0; j < n; ++j) AF[j] += F2[j] * wa[k], AV[j] += V2[j] * wa[k];
    if (k < 2)
      for (size_t j = 0; j < n; ++j) {
        TV[j] += F2[j] * wt[k];
        if (!c.n[j].pinned) TP[j] += V2[j] * wt[k];
      }
  }
  for (size_t j = 0; j < n; ++j) {
    c.n[j].vel += AF[j] * (1.0f / 6);
    if (!c.n[j].pinned) c.n[j].pos += AV[j] * (1.0f / 6);
  }
}

void StepA(Chain& c, V root_pos, V node1_pos, const Rigid* root_w) {
  if (c.n.size() < 2) return;
  Node& n0 = c.n[0];
  n0.pos = root_pos;
  V d = n0.pos - n0.prev;
  if (Len(d) <= 1.0f) {  // (further: a teleport, carried whole)
    float f = c.p.movecorr;  // x G60 = 1 at 60 Hz
    if (root_w) {
      const V loc = Norm(Unapply(*root_w, n0.prev));
      if (loc.y < 0) f = (loc.y + 1) * c.p.movecorr;
    }
    d = d * std::max(f, 0.5f);  // (lowfps: 0.75 - never at the port's 60 Hz)
  }
  for (size_t j = 1; j < c.n.size(); ++j) c.n[j].pos += d, c.n[j].prev += d;
  c.n[1].pos = node1_pos, c.n[1].pinned = true;
  Rk4(c);
}

void StepB(Chain& c, V parent_negz, V root_negz) {
  const float turn = std::min(Len(parent_negz - c.prev_dir) * 0.1f, 0.02f);
  c.prev_dir = parent_negz;
  const size_t n = c.n.size();
  const float L0 = c.L.empty() ? 1.0f : c.L[0];
  for (size_t j = 2; j < n; ++j) {
    const V e = c.n[j - 1].pos * 2 - c.n[j - 2].pos - c.n[j].pos;
    if (Len(e) / L0 > 0.1f) c.n[j].vel += e * (c.p.bend * (j >= 4 ? 0.5f : j == 3 ? 0.75f : 1.0f));
  }
  if (turn > 0.001f)
    for (size_t j = 2; j < n; ++j) c.n[j].vel += root_negz * turn;
  Rk4(c);
}

void StepC(Chain& c, const std::vector<Sphere>& spheres, float floor_y, bool skip_collision, bool ring) {
  const size_t n = c.n.size();
  if (n >= 3) {
    std::vector<std::pair<V, V>> pair(n);
    for (int it = 0; it < 4; ++it) {  // Jacobi, stretch only (segment 0 excluded)
      for (size_t s = 1; s + 1 < n; ++s) {
        const V a = c.n[s].pos, b = c.n[s + 1].pos;
        if (Len(a - b) > c.L[s]) {
          const V m = (a + b) * 0.5f, u = Norm(b - m);
          pair[s] = {m - u * (c.L[s] / 2), m + u * (c.L[s] / 2)};
        } else {
          pair[s] = {a, b};
        }
      }
      for (size_t j = 2; j + 1 < n; ++j) c.n[j].pos = (pair[j - 1].second + pair[j].first) * 0.5f;
      c.n[n - 1].pos = pair[n - 2].second;
    }
    for (size_t s = 1; s + 1 < n; ++s) {  // a strict pass, top down
      const V d = c.n[s + 1].pos - c.n[s].pos;
      if (Len(d) > c.L[s]) c.n[s + 1].pos = c.n[s].pos + Norm(d) * c.L[s];
    }
  }
  if (!skip_collision) {
    for (size_t j = 2; j < n; ++j)
      for (const Sphere& sp : spheres) {
        const float R = sp.r + c.p.radius;
        const V dd = c.n[j].pos - sp.c;
        const float l = Len(dd);
        if (l > 0 && l < R) c.n[j].pos = sp.c + dd * (R / l), c.n[j].vel = V{};
      }
    const float lim = floor_y - c.p.radius;
    for (size_t j = 2; j < n; ++j)
      if (c.n[j].pos.y > lim && (!ring || (std::fabs(c.n[j].pos.x) < 32 && std::fabs(c.n[j].pos.z) < 32))) c.n[j].pos.y = lim;
  }
  for (Node& nd : c.n) nd.prev = nd.pos;
}

void CapsuleSpheres(const CapsuleParams& cap, const Rigid& w, float s, std::vector<Sphere>& out) {
  const V P0 = Apply(w, cap.p0 * s), P1 = Apply(w, cap.p1 * s);
  const float R0 = cap.r0 * s, R1 = cap.r1 * s;
  const V M = (P0 + P1) * 0.5f, S3 = (P1 + M) * 0.5f, S4 = (P0 + S3) * 0.5f;  // (S4 from S3: the game's quirk)
  const float RM = (R0 + R1) / 2;
  out.push_back({P0, R0}), out.push_back({P1, R1}), out.push_back({M, RM}), out.push_back({S3, (RM + R1) / 2});
  out.push_back({S4, (RM + R0) / 2});
}

void Reset(Chain& c, V parent_pos) {
  float y = 0;
  for (size_t j = 0; j < c.n.size(); ++j) {
    Node& nd = c.n[j];
    nd.pos = parent_pos + V{0, y, 0}, nd.prev = nd.pos, nd.vel = V{};
    nd.pinned = j == 0;
    if (j < c.L.size()) y += c.L[j];
  }
  c.prev_dir = V{};
}

void MuscleReset(Muscle& m, V p) {
  m.x = m.mprev = p + V{0, 0.1f, 0};
  m.v = V{}, m.mv_prev = 0;
}

void MuscleStep(std::vector<Muscle>& ms, const std::vector<MuscleRow>& rows, const std::vector<V>& bone_pos) {
  std::vector<V> F(ms.size());
  for (size_t i = 0; i < ms.size(); ++i) {
    Muscle& m = ms[i];
    const MuscleRow& r = rows[size_t(m.row)];
    const V p = bone_pos[i];
    const float mv = Len(p - m.mprev);
    if (std::fabs(mv - m.mv_prev) > 0.7f || mv > 10.0f) m.x += p - m.mprev;  // (a jump: the mass dragged along)
    m.mv_prev = mv, m.mprev = p;
    F[i] = m.v * -r.visc, F[i].y += r.grav;
  }
  for (size_t i = 0; i < ms.size(); ++i) {
    Muscle& m = ms[i];
    const MuscleRow& r = rows[size_t(m.row)];
    V d = bone_pos[i] - m.x;
    float l = Len(d);
    if (l == 0) d.y = 0.001f, l = Len(d);
    F[i] += d * ((r.spring * (l - 0.1f) + r.damp * Dot(m.v, d) / l) / l);
  }
  for (size_t i = 0; i < ms.size(); ++i) {
    Muscle& m = ms[i];
    m.v += F[i] * rows[size_t(m.row)].time;
    m.x += m.v;
  }
}

V MuscleOffset(const Muscle& m, const MuscleRow& r, const Rigid& w) {
  const V off = m.x - w.t - V{0, 0.1f, 0};
  // bone-local (the axes: R's columns), gains per axis, back to world
  V loc{w.r[0][0] * off.x + w.r[1][0] * off.y + w.r[2][0] * off.z, w.r[0][1] * off.x + w.r[1][1] * off.y + w.r[2][1] * off.z,
        w.r[0][2] * off.x + w.r[1][2] * off.y + w.r[2][2] * off.z};
  loc.x *= r.lim[0], loc.y *= r.lim[1], loc.z *= r.lim[2];
  return Rotate(w, loc);
}

// ---- the replay (tools/hair_sim.py main)
std::string ReplayDump(const std::string& text, float floor_y) {
  struct St {
    std::vector<V> P, Vel, prev;
    std::vector<int> pin;
  };
  std::map<int, Chain> chains;
  std::map<int, std::map<std::pair<std::string, int>, St>> steps;
  std::map<int, std::vector<Sphere>> S;
  std::map<int, std::map<int, V>> D;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    auto t = Tokens(line);
    if (t.empty()) continue;
    auto f = [&](size_t k) { return k < t.size() ? float(std::atof(t[k].c_str())) : 0.0f; };
    if (t[0] == "P" && t.size() >= 28) {
      Chain c;
      const int cnt = std::atoi(t[2].c_str());
      for (int k = 0; k < 16; ++k) c.L.push_back(f(3 + size_t(k)));
      c.p.lencorr = f(19), c.p.visc = f(20), c.p.grav = f(21), c.p.spring = f(22), c.p.damp = f(23), c.p.radius = f(24);
      c.p.bend = f(25), c.p.movecorr = f(26), c.p.A = std::atoi(t[27].c_str());
      c.n.resize(size_t(cnt));
      chains[std::atoi(t[1].c_str())] = c;
    } else if (t[0] == "S" && t.size() >= 3) {
      const int fr = std::atoi(t[1].c_str()), n = std::atoi(t[2].c_str());
      if (t.size() < 3 + size_t(4 * n)) continue;
      for (int k = 0; k < n; ++k) S[fr].push_back({{f(3 + 4 * size_t(k)), f(4 + 4 * size_t(k)), f(5 + 4 * size_t(k))}, f(6 + 4 * size_t(k))});
    } else if (t[0] == "D" && t.size() >= 6) {
      D[std::atoi(t[1].c_str())][std::atoi(t[2].c_str())] = {f(3), f(4), f(5)};
    } else if (t.size() >= 4 && std::isdigit(uint8_t(t[0][0]))) {
      const int fr = std::atoi(t[0].c_str()), i = std::atoi(t[2].c_str()), cnt = std::atoi(t[3].c_str());
      if (t.size() != 4 + 10 * size_t(cnt)) continue;  // (the last line, cut when the game stopped)
      St st;
      for (int k = 0; k < cnt; ++k) {
        const size_t b = 4 + 10 * size_t(k);
        st.P.push_back({f(b), f(b + 1), f(b + 2)}), st.Vel.push_back({f(b + 3), f(b + 4), f(b + 5)});
        st.prev.push_back({f(b + 6), f(b + 7), f(b + 8)}), st.pin.push_back(int(f(b + 9)));
      }
      steps[fr][{t[1], i}] = std::move(st);
    }
  }
  auto load = [](Chain c, const St& st) {
    for (size_t j = 0; j < c.n.size() && j < st.P.size(); ++j)
      c.n[j].pos = st.P[j], c.n[j].vel = st.Vel[j], c.n[j].prev = st.prev[j], c.n[j].pinned = st.pin[j] != 0;
    return c;
  };
  auto err = [](const Chain& c, const St& st, bool vel) {
    float e = 0;
    for (size_t j = 0; j < c.n.size() && j < st.P.size(); ++j) {
      const V a = vel ? c.n[j].vel : c.n[j].pos, b = vel ? st.Vel[j] : st.P[j];
      e = std::max({e, std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)});
    }
    return e;
  };
  const char* names[5] = {"A pos", "A vel", "B pos", "B vel", "C pos"};
  float worst[5] = {}, sum[5] = {};
  int count[5] = {}, kicks = 0;
  for (const auto& [fr, st] : steps)
    for (const auto& [i, c0] : chains) {
      auto a0 = st.find({"A0", i}), a1 = st.find({"A1", i}), b1 = st.find({"B1", i}), c1 = st.find({"C1", i});
      if (a0 == st.end() || a1 == st.end() || b1 == st.end() || c1 == st.end()) continue;
      Chain a = load(c0, a0->second);
      for (size_t j = 0; j < a.n.size(); ++j) a.n[j].pinned = a1->second.pin[j] != 0;
      StepA(a, a1->second.P[0], a1->second.P[1], nullptr);
      Chain b = load(c0, a1->second);
      // the kick: the head's -Z from the dump stands in for the root's (as hair_sim.py)
      V now, before;
      const bool has = D.count(fr + 1) && D[fr + 1].count(i) && D.count(fr) && D[fr].count(i);
      if (has) now = D[fr + 1][i], before = D[fr][i], ++kicks;
      b.prev_dir = has ? before : V{};
      StepB(b, has ? now : V{}, now);
      Chain cc = load(c0, b1->second);
      const auto b10 = st.find({"B1", 0});
      const bool skip = (b10 != st.end() && b10->second.P[0].y > 5.0f) || (S.count(fr) && !S[fr].empty() && S[fr][0].c.y > 0);
      StepC(cc, S.count(fr) ? S[fr] : std::vector<Sphere>(), floor_y, skip, true);
      const float e[5] = {err(a, a1->second, false), err(a, a1->second, true), err(b, b1->second, false), err(b, b1->second, true),
                          err(cc, c1->second, false)};
      for (int k = 0; k < 5; ++k) worst[k] = std::max(worst[k], e[k]), sum[k] += e[k], ++count[k];
    }
  char b[160];
  std::string out = "frames " + std::to_string(steps.size()) + " chains " + std::to_string(chains.size()) + "\n";
  for (int k = 0; k < 5; ++k) {
    std::snprintf(b, sizeof b, "%-6s mean err %.5f  worst %.5f  (%d chain-frames)\n", names[k], count[k] ? sum[k] / count[k] : 0.0f,
                  worst[k], count[k]);
    out += b;
  }
  return out;
}

}  // namespace hair
