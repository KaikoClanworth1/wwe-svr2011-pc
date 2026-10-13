"""The SvR 2011 superstar skeleton as data (tools/svr11_skeleton.json) for model importers
(the Mod Maker's FBX import, tools/svr11_build.py) and rig mapping (tools/rig_maps.json).

    py svr11_skeleton.py <2011 ch104.pac> [out.json]     -> regenerates the JSON from a stock pac

The node table is read with its true layout (name[16], t f32x3 +16, pad +28, r f32x3 +32, pad +44,
parent s32 +48; tools/jboy.py keeps the raw words for round trips). Everything else in the JSON
(roles, motion axes, mesh rules) is what the converters (svr08_char, w13_char, svr10_char) and the
budget check (model_budget.py) found; see the notes in the file.
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ch_tool  # noqa: E402
import svrfmt  # noqa: E402
import w13_char  # noqa: E402
import ymks  # noqa: E402

TWIST = ('_momo_x', '_ninoude_x', '_kote_x', '_kote_x2', 'kubi_x')
HELPER = ('_ninoude_m', '_kote_m', '_kata_m', '_sakotsu_m', '_mune_dummy', 'onaka')
FINGER_NAMES = ['thumb', 'index', 'middle', 'ring', 'pinky']

SPACE = {
    'units': '1 model unit = 10 cm (WWE \'12+ are in cm: x 0.1)',
    'up': '-Y (the head is at negative Y)',
    'front': '-Z (the face and toes point to -Z)',
    'left': '+X is the character\'s left (l_ bones)',
    'origin': 'the pelvis (root) at 0; the floor is the lowest vertex, about +9.4 on ch104 (no floor bone)',
    'rotation': 'local euler radians r = (x, y, z), matrix Rz * Ry * Rx (column vectors); world = parent world * [R | t]',
    'bone_axis': '+X of a bone\'s local frame points along the bone: a child\'s t is (length, ~0, ~0) in the parent frame',
    'bind_pose': 'A-pose: upper arms 15 deg below horizontal, elbows bent 20 deg forward (kote r.y = 0.349), '
                 'thighs 15 deg out, fingers slightly curled; meshes are stored in this pose (bind = rest)',
}

NODE_RECORD = {
    'size': 80,
    'fields': 'name char[16], t f32x3 +16, pad u32 +28, r f32x3 +32, pad u32 +44, parent s32 +48 (-1 / 0 = root), '
              'u32 x3 +52, bounding sphere f32x4 +64 (0 on stock bones)',
    'order': 'parents come before children; the game finds bones by name (motion, face animation, physics), '
             'so a table may drop or add bones (stock tables run 92-140 nodes)',
}

ROLES = {
    'root': 'pelvis: the motion root (translation + rotation)',
    'motion': 'the 21 bones the motion data (YMKs / YMBs) animates; axes = which local rotation axes the motion keys '
              '(xyz free; yz keeps the bone direction; xy and y are hinges)',
    'twist': 'in-line roll segments (*_momo_x, *_ninoude_x, *_kote_x, *_kote_x2, kubi_x): not in the motion data, '
             'the game spreads the limb roll over them; stock skins weight the forearm / upper arm / thigh roll to them',
    'helper': 'corrective bones (*_ninoude_m, *_kote_m, *_kata_m, *_sakotsu_m, *_mune_dummy, onaka): stock skins use '
              'them at the shoulders, elbows, chest and belly; a skin may leave them unweighted',
    'finger': '30 finger joints yubiFJ: F 0 thumb, 1 index, 2 middle, 3 ring, 4 pinky; J 0 base (parent te), 1, 2 '
              '(parent the previous joint); posed by hand shapes (tools/hand_tracks.py, finger_retarget.py)',
    'face': 'face animation bones under atama (brows mayu_*, miken, lids mabuta, eye, meziri, cheeks hoho*, nose hana, '
            'mouth *kuchi*); l_eye / r_eye carry the eyeballs (yReflect meshes); weight the head to atama only and '
            'the face simply does not animate',
    'end': 'end bones (tsumasaki = toes) are animated too (y hinge)',
}

OPTIONAL = {
    'l_boin / r_boin': 'breast sway bones under mune (divas: their tables drop the 8 *_m / *_mune_dummy helpers)',
    'kami_aNN[_JJ] / kami_bNN[_JJ]': 'sway physics chains (hair, ponytails, ties, coats) under atama / kubi / mune: '
                                     'chain NN, joints _00.._04; the game simulates them by name',
    'k*, kl_*, kr_*': 'contact points (ch192 only); not needed',
}

MESH_RULES = {
    'models': {'0x0': 'body (skin yBumpMapSS + eyes yReflect + a few yDefault)', '0x2710': 'hair / beard cards (yBumpMapSS)',
               '0x4e20': 'eyelashes (yDefault)', '0x7530 / 0x7531 / 0x7532': 'blood overlays (template copies; dropped when over the budget)'},
    'palette': 'up to 20 bones per mesh: node index + 1 (s32; unused = -1); split a mesh when it needs more',
    'influences': 'up to 4 per vertex; weight block count n = mesh +0x5C (1-4, the most any vertex of the mesh uses); '
                  'the weights are vertex-major: per vertex n entries {u8 palette slot (255 = none), u8 0 x3, f32 weight}, '
                  'summing to 1',
    'vertex': '28 bytes big-endian: position f32x3, normal f32x3, colour u32 ARGB (keep FFFFFFFF: alpha is drawn as opacity); '
              'uv f32x2 in their own array (v down, DirectX); indices u16 (so at most 65535 vertices per mesh)',
    'primitives': 'triangle strips (prim 6), degenerate-joined; several strips per mesh allowed',
    'vertex_format': 'mesh +0x8C, a per-material flag word (0-12 seen; not the block count): copy the template mesh\'s '
                     'for that kind (skin 0, eyes 3, hair 4, lashes 0)',
    'shaders': {'skin': 'yBumpMapSS: texDiffuse color, texNormal normal, texOcclusion Mk, texTilingMap noise',
                'eye': 'yReflect: texDiffuse color, texCubeRefction cubemap, texRefctionReg color, texOcclusion Mk',
                'hair': 'yBumpMapSS (the template hair mesh\'s parameters)', 'lash': 'yDefault: texDiffuse color'},
    'textures': {'0xc': 'colour bundle: "color" RGBA DXT5, alpha = specular level (and the cut-out on hair / lash cards)',
                 '0xa': 'normal (DXT5), noise (template), Mk (DXT5: R bruise mask, G/B ambient occlusion, A spec mask), '
                        'bloodhair* (template)',
                 'mips': 'full mip chains; a sheet without mips draws black in the game'},
    'budget': 'per wrestler 768 KB of vertex memory = 192 pages of 4 KB; per mesh ceil(verts x (32 if 1 weight block '
              'else 44) / 4096) + ceil(indices x 2 / 4096) pages, over every model of the attire (model_budget.py); '
              'over 192 the match crashes on load - drop the blood overlays first (the game also does at mod load)',
    'from_template': 'the node table (frames), hair / lash / eye parameter blocks, children 0xb, 0x28, 0x32, 0x50, '
                     '0x64, noise, cubemap, bloodhair textures, the blood overlays and the portrait 0xc8 / 0xc9 '
                     '(256 x 256 DXT5) unless one is given',
    'pac': 'EPK8 with one EMD group (entry names = attires); header +4 = index size 12 + 16 x entries, the group\'s '
           'count field = entries x 4 (wrong values crash the mount); children PACH, BPE-compressed',
    'pitfalls': ['vertex colour alpha 0 makes the skin invisible (force FF)',
                 'the Mod Maker preview ignores the EPK8 header and the vertex alpha: a model can look fine there and still fail',
                 'mesh +0x74 = the mesh\'s own index in the model'],
}


def frames(pac):
    _, groups = ch_tool.epk8_read(open(pac, 'rb').read())
    kids = {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(groups[0][1][0][1]))}
    return w13_char.read13_nodes(kids[0])


def role(name):
    if name == 'root':
        return 'root'
    if name in ymks.BONES:
        return 'motion'
    if name.endswith(TWIST) or name == 'kubi_x':
        return 'twist'
    if name.endswith(HELPER):
        return 'helper'
    if '_yubi' in name:
        return 'finger'
    return 'face'


def build(pac):
    N = frames(pac)
    W = w13_char.world(N)
    axes = {b: ymks.axes_of(ymks.TYPES_M1[k]) for k, b in enumerate(ymks.BONES)}
    bones = []
    for i, n in enumerate(N):
        p = n['parent'] if i else -1
        rec = {'name': n['name'], 'parent': N[p]['name'] if p >= 0 else None, 'parent_index': p,
               't': [round(float(x), 6) for x in n['t']], 'r': [round(float(x), 6) for x in n['r']],
               'world': [round(float(x), 5) for x in W[i][:3, 3]],
               'axis': [round(float(x), 4) for x in W[i][:3, 0]], 'role': role(n['name'])}
        if n['name'] in axes:
            rec['motion_axes'] = axes[n['name']]
        if rec['role'] == 'finger':
            f, j = int(n['name'][-2]), int(n['name'][-1])
            rec['finger'] = [FINGER_NAMES[f], j]
        bones.append(rec)
    feet = min(b['world'][1] for b in bones)
    return {
        'version': 1,
        'source': 'stock ch104 (male, the converters\' template); generated by tools/svr11_skeleton.py',
        'space': SPACE, 'node_record': NODE_RECORD, 'roles': ROLES, 'optional_bones': OPTIONAL,
        'motion_bones': list(ymks.BONES),
        'height_note': 'ch104: head top about -10.1, floor about +9.4 (19.5 units = 1.95 m); superstar mods scale with '
                       'height= (0.80-1.25) on top of the model',
        'bones': bones, 'mesh_rules': MESH_RULES, '_check_min_world_y': feet,
    }


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    out = argv[2] if len(argv) > 2 else os.path.join(HERE, 'svr11_skeleton.json')
    d = build(argv[1])
    d.pop('_check_min_world_y')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(d, f, indent=1)
    print(out, len(d['bones']), 'bones')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
