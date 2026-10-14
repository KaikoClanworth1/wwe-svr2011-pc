// SvR 2011's secondary motion for the Mod Maker's 3D views: the superstar hair solver (gBaH) and the body
// jiggle (gBaM), ported from docs/HAIR_PHYSICS.md (section 7; the Python reference tools/hair_sim.py). Pure
// math - char_preview.cpp feeds it the posed bones and writes the results back.
//
// Space: the game's (+Y down, about 0.11 m a unit). A rigid transform maps a point as p' = R p + t (R's
// columns are the bone's axes: the doc's row-vector (v, w) * World is Apply / Rotate here).
#pragma once
#include <string>
#include <vector>

namespace hair {

struct V {
  float x = 0, y = 0, z = 0;
};
struct Rigid {
  float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  V t;
};
V Apply(const Rigid& m, V p);      // R p + t
V Rotate(const Rigid& m, V v);     // R v
V Unapply(const Rigid& m, V p);    // the rigid inverse: R^T (p - t)

// ---- entry 60 / 61 (text children of the attire's EMD PACH)
struct ChainParams {
  std::string root;  // kami_bNN
  float radius = 0.15f, visc = 0.1f, grav = 0.034f, spring = 0, damp = 0, lencorr = 0, bend = 0, movecorr = 0;
  int A = 0;
};
struct CapsuleParams {
  std::string bone;
  V p0, p1;
  float r0 = 0, r1 = 0;
};
// One line each: `kami_bNN radius visc grav spring damp A lencorr bend movecorr` (at most 16) /
// `bone x0 y0 z0 r0 x1 y1 z1 r1 k` (at most 14); `;` starts a comment line, lines that don't parse are skipped.
std::vector<ChainParams> ParseChains(const std::string& text);
std::vector<CapsuleParams> ParseCapsules(const std::string& text);
// Entry 50: up to 9 rows of 8 floats (visc grav spring damp time limitX limitY limitZ).
struct MuscleRow {
  float visc = 0, grav = 0, spring = 0, damp = 0, time = 0, lim[3] = {0, 0, 0};
};
std::vector<MuscleRow> ParseMuscles(const std::string& text);

// ---- the hair solver
struct Node {
  V pos, vel, prev;
  bool pinned = false;
  int bone = -1;
};
struct Chain {
  std::vector<Node> n;   // node 0 the root (kami_bNN), 1 its first child (pinned), 2.. simulated, last a target
  std::vector<float> L;  // rest length of each segment: all the last node's local x (the game's)
  int parent = -1;       // the root's parent bone (atama)
  ChainParams p;
  V prev_dir;            // the parent's -Z last frame (the head-turn kick)
};
struct Sphere {
  V c;
  float r = 0;
};

// Step A (8216D618) for one chain: the root follows its bone, the rest is carried by half of its move (the
// game's movement correction at 60 Hz), node 1 pinned to its animated place, then RK4 #1.
// root_w: the root bone's world transform (for the correction's local test); null: the stock value 0.5.
void StepA(Chain& c, V root_pos, V node1_pos, const Rigid* root_w);
// Step B (8216C6F0): bend stiffness, the head-turn kick (parent_negz: the parent's -Z now; root_negz: the root
// bone's -Z), then RK4 #2.
void StepB(Chain& c, V parent_negz, V root_negz);
// Step C (8216A9E8): the length passes, then (unless skip_collision) the spheres and the floor (y <= floor_y -
// radius; `ring`: only inside the 64 x 64 square), prev = pos.
void StepC(Chain& c, const std::vector<Sphere>& spheres, float floor_y, bool skip_collision, bool ring);
void Rk4(Chain& c);
// The capsules as the game's five spheres each (world: the capsule bone's transform, scale the character's).
void CapsuleSpheres(const CapsuleParams& cap, const Rigid& bone_world, float scale, std::vector<Sphere>& out);
// Reset (8216A830): every node hung from the parent's origin straight down (+Y), at rest.
void Reset(Chain& c, V parent_pos);

// ---- the body jiggle (gBaM): a point mass per bone on a damped spring; the output moves the bone's skin only
struct Muscle {
  int bone = -1, row = 0;
  V x, v, mprev;
  float mv_prev = 0;
};
void MuscleStep(std::vector<Muscle>& ms, const std::vector<MuscleRow>& rows, const std::vector<V>& bone_pos);
// The skin offset (world) for muscle m on a bone with world transform w (limits per axis are gains).
V MuscleOffset(const Muscle& m, const MuscleRow& row, const Rigid& w);
void MuscleReset(Muscle& m, V bone_pos);

// ---- check against the game (tools/data/hair_ref_edge.txt, src/hair_dump.cpp's dump): every step replayed
// from the game's state before it, compared with its state after it, as tools/hair_sim.py does. -> a report.
std::string ReplayDump(const std::string& dump_text, float floor_y);

}  // namespace hair
