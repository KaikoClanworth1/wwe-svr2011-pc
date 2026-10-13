"""Bone maps from well-known rigs to the SvR 2011 skeleton (tools/svr11_skeleton.json), and the
reference matcher the Mod Maker's FBX import follows.

    py rig_maps.py                         -> regenerates tools/rig_maps.json
    py rig_maps.py match <names.json>      -> JSON {rig, map {source bone: 2011 bone}, folded, unmapped}
    py rig_maps.py selftest [dir]          -> synthetic Mixamo / UE5 / Rigify / Biped / WWE / generic rigs must map
                                              every core bone; dir: writes <rig>.json {bones, expect} for other matchers
                                              names.json: [[name, parent name or null], ...]

Rigs: Unity Humanoid / VRChat (incl. the Blender .L / .R and CATS spellings), Mixamo, Blender Rigify
(DEF- bones), 3ds Max Biped (Bip01 / Bip001), Unreal Mannequin (UE4 / UE5), and the Biped names of
WWE '12 - 2K17 (tools/w13_char.name13to11: 'Bip L Thigh', 'L_Thigh_x', ...).

Matching (match()):
 1. names are compared after stripping a namespace ('mixamorig:', 'Armature|', 'rig/') and case;
 2. the rig with the most exact hits among its core bones wins (at least 8 hits), its table maps;
 3. bones it doesn't name go through the generic aliases (side + normalised base name);
 4. spine: the hips -> root; spine bones from the hips up to the one the clavicles / neck hang off: the
    first -> koshi, the last -> mune, any between fold into koshi (2011 has two spine bones);
 5. twist / roll bones ('twist', 'roll', Rigify '.001' segments) -> the 2011 twist bone of the segment
    they sit in (upper arm -> ninoude_x, forearm -> kote_x, thigh -> momo_x, neck -> kubi_x);
 6. anything else folds into its nearest mapped ancestor (weights added to that bone): hair, skirt and
    coat chains, props, metacarpals, finger ends, extra toes. Breast bones -> l_boin / r_boin when the
    template has them, else mune.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# 2011 targets by a side-neutral key ('{s}' = l / r)
LIMB = {
    'clavicle': '{s}_sakotsu', 'upperarm': '{s}_ninoude', 'forearm': '{s}_kote', 'hand': '{s}_te',
    'thigh': '{s}_momo', 'calf': '{s}_sune', 'foot': '{s}_ashi', 'toe': '{s}_tsumasaki',
    'upperarm_twist': '{s}_ninoude_x', 'forearm_twist': '{s}_kote_x', 'forearm_twist2': '{s}_kote_x2',
    'thigh_twist': '{s}_momo_x', 'eye': '{s}_eye', 'breast': '{s}_boin',
}
for _f, _n in enumerate(('thumb', 'index', 'middle', 'ring', 'pinky')):
    for _j in range(3):
        LIMB['%s%d' % (_n, _j + 1)] = '{s}_yubi%d%d' % (_f, _j)
CENTRE = {'hips': 'root', 'spine': 'koshi', 'chest': 'mune', 'neck': 'kubi', 'neck_twist': 'kubi_x', 'head': 'atama',
          'jaw': 'd_kuchi'}

FINGERS = ('thumb', 'index', 'middle', 'ring', 'pinky')


def _rig(centre, limb, sides):
    """centre: {source: key}; limb: {source with {S}: key}; sides: (left token, right token)"""
    out = {}
    for src, key in centre.items():
        out[src] = CENTRE[key]
    for src, key in limb.items():
        for tok, s in zip(sides, 'lr'):
            out[src.replace('{S}', tok)] = LIMB[key].format(s=s)
    return out


def _fingers(fmt, names, joints=(1, 2, 3)):
    """fmt like '{S}Hand{F}{J}' -> {name: key}"""
    out = {}
    for f, nm in zip(FINGERS, names):
        for k, j in enumerate(joints):
            out[fmt.replace('{F}', nm).replace('{J}', str(j))] = '%s%d' % (f, k + 1)
    return out


RIGS = {}

# Unity Humanoid (Avatar bone names, also what VRChat's humanoid uses)
RIGS['unity_humanoid'] = {
    'label': 'Unity Humanoid / VRChat',
    'map': _rig({'Hips': 'hips', 'Spine': 'spine', 'Chest': 'chest', 'UpperChest': 'chest', 'Neck': 'neck', 'Head': 'head',
                 'Jaw': 'jaw'},
                dict({'{S}Shoulder': 'clavicle', '{S}UpperArm': 'upperarm', '{S}LowerArm': 'forearm', '{S}Hand': 'hand',
                      '{S}UpperLeg': 'thigh', '{S}LowerLeg': 'calf', '{S}Foot': 'foot', '{S}Toes': 'toe', '{S}Eye': 'eye'},
                     **_fingers('{S}{F}{J}', ('Thumb', 'Index', 'Middle', 'Ring', 'Little'), ('Proximal', 'Intermediate', 'Distal'))),
                ('Left', 'Right')),
}
# (Unity's own spelling with spaces: 'Left Upper Arm', 'Left Thumb Proximal')
RIGS['unity_humanoid']['map'].update(_rig({'Upper Chest': 'chest'},
    dict({'{S} Shoulder': 'clavicle', '{S} Upper Arm': 'upperarm', '{S} Lower Arm': 'forearm', '{S} Hand': 'hand',
          '{S} Upper Leg': 'thigh', '{S} Lower Leg': 'calf', '{S} Foot': 'foot', '{S} Toes': 'toe', '{S} Eye': 'eye'},
         **_fingers('{S} {F} {J}', ('Thumb', 'Index', 'Middle', 'Ring', 'Little'), ('Proximal', 'Intermediate', 'Distal'))),
    ('Left', 'Right')))

# VRChat avatars out of Blender (CATS / MMD-style English names, .L / .R and _L / _R suffixes)
def _cats_fingers():
    """CATS / MMD-English fingers: Thumb0-2, then IndexFinger1-3 / Index1-3 ... (side suffix {S})"""
    out = {'Thumb0{S}': 'thumb1', 'Thumb1{S}': 'thumb2', 'Thumb2{S}': 'thumb3'}
    for key, spellings in (('index', ('IndexFinger', 'Index')), ('middle', ('MiddleFinger', 'Middle')),
                           ('ring', ('RingFinger', 'Ring')), ('pinky', ('LittleFinger', 'Little', 'Pinky'))):
        for sp in spellings:
            for j in (1, 2, 3):
                out['%s%d{S}' % (sp, j)] = '%s%d' % (key, j)
    return out


_cats = {}
for sfx in (('.L', '.R'), ('_L', '_R')):
    _cats.update(_rig({}, dict({'Shoulder{S}': 'clavicle', 'Upper_arm{S}': 'upperarm', 'UpperArm{S}': 'upperarm', 'Arm{S}': 'upperarm',
                                'Lower_arm{S}': 'forearm', 'LowerArm{S}': 'forearm', 'Elbow{S}': 'forearm', 'ForeArm{S}': 'forearm',
                                'Hand{S}': 'hand', 'Wrist{S}': 'hand',
                                'Upper_leg{S}': 'thigh', 'UpperLeg{S}': 'thigh', 'Leg{S}': 'thigh', 'Thigh{S}': 'thigh',
                                'Lower_leg{S}': 'calf', 'LowerLeg{S}': 'calf', 'Knee{S}': 'calf', 'Shin{S}': 'calf',
                                'Foot{S}': 'foot', 'Ankle{S}': 'foot', 'Toe{S}': 'toe', 'Toes{S}': 'toe', 'ToeBase{S}': 'toe',
                                'Eye{S}': 'eye', 'Breast{S}': 'breast', 'Breast_root{S}': 'breast',
                                'Upper_arm_twist{S}': 'upperarm_twist', 'Lower_arm_twist{S}': 'forearm_twist',
                                'Hand_twist{S}': 'forearm_twist'},
                               **_cats_fingers(),
                               **_fingers('{F}_{J}{S}', ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'), (1, 2, 3)),
                               **_fingers('{F} {J}{S}', ('Thumb', 'Index', 'Middle', 'Ring', 'Little'),
                                          ('Proximal', 'Intermediate', 'Distal'))), sfx))
# (CATS 'Left arm' style)
_cats.update(_rig({}, {'{S} shoulder': 'clavicle', '{S} arm': 'upperarm', '{S} elbow': 'forearm', '{S} wrist': 'hand',
                       '{S} leg': 'thigh', '{S} knee': 'calf', '{S} ankle': 'foot', '{S} toe': 'toe', '{S} eye': 'eye'},
                  ('Left', 'Right')))
_cats.update({'Hips': 'root', 'Pelvis': 'root', 'Spine': 'koshi', 'Chest': 'mune', 'Upper_chest': 'mune', 'UpperChest': 'mune',
              'Neck': 'kubi', 'Head': 'atama', 'Jaw': 'd_kuchi'})
RIGS['vrchat_blender'] = {'label': 'VRChat / Blender (.L .R, CATS)', 'map': _cats}

# Mixamo (names after the 'mixamorig:' namespace)
RIGS['mixamo'] = {
    'label': 'Mixamo',
    'map': _rig({'Hips': 'hips', 'Spine': 'spine', 'Spine1': 'chest', 'Spine2': 'chest', 'Neck': 'neck', 'Head': 'head'},
                dict({'{S}Shoulder': 'clavicle', '{S}Arm': 'upperarm', '{S}ForeArm': 'forearm', '{S}Hand': 'hand',
                      '{S}UpLeg': 'thigh', '{S}Leg': 'calf', '{S}Foot': 'foot', '{S}ToeBase': 'toe', '{S}Eye': 'eye'},
                     **_fingers('{S}Hand{F}{J}', ('Thumb', 'Index', 'Middle', 'Ring', 'Pinky'))),
                ('Left', 'Right')),
}

# Blender Rigify deform bones
RIGS['rigify'] = {
    'label': 'Blender Rigify (DEF-)',
    'map': _rig({'DEF-spine': 'hips', 'DEF-spine.001': 'spine', 'DEF-spine.002': 'spine', 'DEF-spine.003': 'chest',
                 'DEF-spine.004': 'neck', 'DEF-spine.005': 'neck', 'DEF-spine.006': 'head', 'DEF-jaw': 'jaw'},
                dict({'DEF-shoulder{S}': 'clavicle', 'DEF-upper_arm{S}': 'upperarm', 'DEF-upper_arm{S}.001': 'upperarm_twist',
                      'DEF-forearm{S}': 'forearm', 'DEF-forearm{S}.001': 'forearm_twist', 'DEF-hand{S}': 'hand',
                      'DEF-thigh{S}': 'thigh', 'DEF-thigh{S}.001': 'thigh_twist', 'DEF-shin{S}': 'calf', 'DEF-shin{S}.001': 'calf',
                      'DEF-foot{S}': 'foot', 'DEF-toe{S}': 'toe', 'DEF-eye{S}': 'eye', 'DEF-breast{S}': 'breast'},
                     **_fingers('DEF-{F}.0{J}{S}', ('thumb', 'f_index', 'f_middle', 'f_ring', 'f_pinky'))),
                ('.L', '.R')),
}

# 3ds Max Biped (Bip01 / Bip001 prefixes are stripped before matching)
_biped = dict({'{S} Clavicle': 'clavicle', '{S} UpperArm': 'upperarm', '{S} Forearm': 'forearm', '{S} Hand': 'hand',
               '{S} Thigh': 'thigh', '{S} Calf': 'calf', '{S} Foot': 'foot', '{S} Toe0': 'toe',
               '{S} UpArmTwist': 'upperarm_twist', '{S} ForeTwist': 'forearm_twist', '{S} ForeTwist1': 'forearm_twist2',
               '{S} ThighTwist': 'thigh_twist'},
              **_fingers('{S} Finger{F}{J}', ('0', '1', '2', '3', '4'), ('', '1', '2')))
RIGS['max_biped'] = {
    'label': '3ds Max Biped',
    'map': _rig({'Pelvis': 'hips', 'Spine': 'spine', 'Spine1': 'chest', 'Spine2': 'chest', 'Spine3': 'chest', 'Neck': 'neck',
                 'Head': 'head'}, _biped, ('L', 'R')),
}

# Unreal Mannequin (UE4 and UE5 Manny / Quinn)
RIGS['ue_mannequin'] = {
    'label': 'Unreal Mannequin',
    'map': _rig({'pelvis': 'hips', 'spine_01': 'spine', 'spine_02': 'spine', 'spine_03': 'chest', 'spine_04': 'chest',
                 'spine_05': 'chest', 'neck_01': 'neck', 'neck_02': 'neck', 'head': 'head'},
                dict({'clavicle_{S}': 'clavicle', 'upperarm_{S}': 'upperarm', 'lowerarm_{S}': 'forearm', 'hand_{S}': 'hand',
                      'thigh_{S}': 'thigh', 'calf_{S}': 'calf', 'foot_{S}': 'foot', 'ball_{S}': 'toe',
                      'upperarm_twist_01_{S}': 'upperarm_twist', 'upperarm_twist_02_{S}': 'upperarm_twist',
                      'lowerarm_twist_01_{S}': 'forearm_twist', 'lowerarm_twist_02_{S}': 'forearm_twist2',
                      'thigh_twist_01_{S}': 'thigh_twist', 'thigh_twist_02_{S}': 'thigh_twist',
                      'calf_twist_01_{S}': 'calf', 'calf_twist_02_{S}': 'calf'},
                     **_fingers('{F}_0{J}_{S}', ('thumb', 'index', 'middle', 'ring', 'pinky'))),
                ('l', 'r')),
}

# WWE '12 - 2K17 (Yuke's Biped: tools/w13_char.name13to11)
_wwe = dict({'Bip {S} Clavicle': 'clavicle', 'Bip {S} UpperArm': 'upperarm', '{S}_UpperArm_x': 'upperarm_twist',
             'Bip {S} Forearm': 'forearm', '{S}_Wrist_x': 'forearm_twist', '{S}_Wrist_x2': 'forearm_twist2', 'Bip {S} Hand': 'hand',
             'Bip {S} Thigh': 'thigh', '{S}_Thigh_x': 'thigh_twist', 'Bip {S} Calf': 'calf', 'Bip {S} Foot': 'foot',
             'Bip {S} Toe0': 'toe'},
            **_fingers('Bip {S} Finger{F}{J}', ('0', '1', '2', '3', '4'), ('', '1', '2')))
RIGS['wwe_biped'] = {
    'label': "WWE '12 - 2K17 (Yuke's Biped)",
    'map': _rig({'Bip Pelvis': 'hips', 'Bip Spine': 'spine', 'Bip Spine1': 'chest', 'Bip Neck': 'neck', 'Neck_x': 'neck_twist',
                 'Bip Head': 'head', 'd_kuchi_t': 'jaw'}, _wwe, ('L', 'R')),
}
# (their face bones share 2011's names: l_mayu_00, hana, d_kuchi ... map to themselves)
RIGS['wwe_biped']['same_names'] = True

# the core bones a rig must hit for detection (the 21 motion bones' sources)
CORE = {'root', 'koshi', 'mune', 'kubi', 'atama', 'l_sakotsu', 'l_ninoude', 'l_kote', 'l_te', 'r_sakotsu', 'r_ninoude',
        'r_kote', 'r_te', 'l_momo', 'l_sune', 'l_ashi', 'r_momo', 'r_sune', 'r_ashi'}

# generic aliases for rigs not listed: normalised base name (lowercase, no separators, no side) -> key
GENERIC = {
    'hips': 'hips', 'hip': 'hips', 'pelvis': 'hips', 'root': None, 'armature': None,
    'spine': 'spine', 'spine0': 'spine', 'spine1': 'spine', 'spine01': 'spine', 'abdomen': 'spine', 'waist': 'spine',
    'chest': 'chest', 'upperchest': 'chest', 'spine2': 'chest', 'spine02': 'chest', 'spine3': 'chest', 'spine03': 'chest',
    'thorax': 'chest', 'torso': 'spine', 'uppertorso': 'chest', 'upperbody': 'chest', 'lowerbody': 'spine', 'head1': 'head',
    'head01': 'head', 'neck': 'neck', 'neck1': 'neck', 'neck01': 'neck', 'head': 'head', 'jaw': 'jaw',
    'shoulder': 'clavicle', 'clavicle': 'clavicle', 'collar': 'clavicle',
    'upperarm': 'upperarm', 'arm': 'upperarm', 'uparm': 'upperarm', 'shoulderjoint': 'upperarm',
    'forearm': 'forearm', 'lowerarm': 'forearm', 'elbow': 'forearm', 'hand': 'hand', 'wrist': 'hand',
    'thigh': 'thigh', 'upperleg': 'thigh', 'upleg': 'thigh', 'leg': 'thigh', 'calf': 'calf', 'shin': 'calf',
    'lowerleg': 'calf', 'knee': 'calf', 'foot': 'foot', 'ankle': 'foot', 'toe': 'toe', 'toes': 'toe', 'toebase': 'toe',
    'ball': 'toe', 'toe0': 'toe', 'eye': 'eye', 'breast': 'breast', 'boob': 'breast', 'bust': 'breast',
}
FINGER_ALIASES = {'thumb': 'thumb', 'index': 'index', 'indexfinger': 'index', 'middle': 'middle', 'middlefinger': 'middle',
                  'ring': 'ring', 'ringfinger': 'ring', 'pinky': 'pinky', 'little': 'pinky', 'littlefinger': 'pinky',
                  'finger0': 'thumb', 'finger1': 'index', 'finger2': 'middle', 'finger3': 'ring', 'finger4': 'pinky',
                  'fthumb': 'thumb', 'findex': 'index', 'fmiddle': 'middle', 'fring': 'ring', 'fpinky': 'pinky'}
JOINT_WORDS = {'proximal': 1, 'intermediate': 2, 'distal': 3, 'metacarpal': 0}

RULES = {
    'spine': 'hips -> root; the bones on the path from the hips to the one the clavicles (or upper arms) hang off: first -> '
             'koshi, last -> mune, any between fold into koshi; on the path from there to the head the first -> kubi',
    'many_to_one': 'several source bones on one 2011 bone (two neck bones, a calf twist): weights add up; the joint position '
                   'comes from the one nearest the root',
    'above_hips': 'bones above the hips (rig root, armature) stay unmapped; weights on them go to root',
    'twist': "bones named *twist* / *roll* (or Rigify '.001' arm / thigh segments) -> the twist bone of their segment: upper "
             'arm -> ninoude_x, forearm -> kote_x (a second one -> kote_x2), thigh -> momo_x, neck -> kubi_x; calf twists '
             'fold into sune',
    'fold': 'an unmapped bone adds its weights to its nearest mapped ancestor (hair / skirt / coat chains, props, metacarpals, '
            'finger ends, extra toes, IK / control bones that deform)',
    'breast': 'breast bones -> l_boin / r_boin when the template table has them (a diva template), else fold into mune',
    'fingers': '2011 has 3 joints per finger: a 4th (Mixamo *4, an end bone) folds into joint 3; metacarpals fold into the hand',
    'eyes': 'eye bones -> l_eye / r_eye (eyeball meshes rigid on them)',
    'face': 'no generic face mapping: the head weights go to atama and the face does not animate (WWE Biped face bones share '
            "2011's names and map 1:1)",
    'min_hits': 8,
}


def strip_ns(n):
    n = n.split(':')[-1].split('|')[-1].split('/')[-1]
    n = re.sub(r'^(Bip0*1|Bip001|Bip01|Biped)\s+', '', n)
    return n.strip()


def side_of(n):
    """-> (side 'l' / 'r' / None, the name without the side token)"""
    pats = [(r'^(Left|Right)[\s_.]*', 1), (r'[\s_.](L|R|Left|Right|l|r)$', 1), (r'^(L|R|l|r)[\s_.]', 1),
            (r'(Left|Right)', 1), (r'\.(L|R)(?=\.\d+$)', 1)]
    for p, g in pats:
        m = re.search(p, n)
        if m:
            s = m.group(g)[0].lower()
            return s, (n[:m.start()] + ' ' + n[m.end():]).strip()
    return None, n


def norm(n):
    return re.sub(r'[^a-z0-9]', '', n.lower())


def generic_key(name):
    """-> 2011 bone name pattern or None"""
    n = strip_ns(name)
    n = re.sub(r'^(DEF|ORG|MCH)-', '', n)
    s, base = side_of(n)
    b = norm(base)
    tw = 'twist' in b or 'roll' in b
    b2 = b.replace('twist', '').replace('roll', '')
    # fingers: name + joint number or word
    for word, f in sorted(FINGER_ALIASES.items(), key=lambda x: -len(x[0])):
        if b2.startswith(word) or b2.startswith('hand' + word) or b2.startswith('f' + word):
            rest = b2.split(word, 1)[1]
            j = None
            for w, k in JOINT_WORDS.items():
                if w in rest:
                    j = k
            if j is None:
                d = re.findall(r'\d+', rest)
                j = int(d[-1]) if d else 1
                if f in ('thumb',) and word.startswith('finger'):
                    j += 1     # (Biped Finger0 / 01 / 02)
            if j == 0:
                return ('fold', 'hand', s)
            return ('limb', '%s%d' % (f, min(3, j)), s)
    base_key = GENERIC.get(b2)
    if base_key is None:
        return None
    if tw:
        base_key = {'upperarm': 'upperarm_twist', 'forearm': 'forearm_twist', 'hand': 'forearm_twist',
                    'thigh': 'thigh_twist', 'neck': 'neck_twist'}.get(base_key, base_key)
    if base_key in CENTRE:
        return ('centre', base_key, None)
    if s:
        return ('limb', base_key, s)
    return None


def match(bones, template_bones=None):
    """bones: [(name, parent name or None)] -> dict(rig, map, folded, unmapped)"""
    tmpl = set(template_bones or [])
    names = [b[0] for b in bones]
    parent = {b[0]: b[1] for b in bones}
    low = {strip_ns(n).lower(): n for n in names}
    best, hits = None, 0
    for rid, rig in RIGS.items():
        rl = {k.lower(): v for k, v in rig['map'].items()}
        h = len({v for k, v in rl.items() if k in low and v in CORE})
        if h > hits:
            best, hits = rid, h
    out = {}
    if best and hits >= RULES['min_hits']:
        rl = {k.lower(): v for k, v in RIGS[best]['map'].items()}
        for k, n in low.items():
            if k in rl:
                out[n] = rl[k]
        if RIGS[best].get('same_names'):
            for n in names:
                if n not in out and n in tmpl:
                    out[n] = n
    else:
        best = 'generic'
    for n in names:
        if n in out:
            continue
        g = generic_key(n)
        if not g:
            continue
        kind, key, s = g
        if kind == 'centre':
            out[n] = CENTRE[key]
        elif kind == 'limb' and s:
            out[n] = LIMB[key].format(s=s)
    # spine by structure: the path from the hips up to the bone the clavicles (or upper arms) hang off;
    # first -> koshi, last -> mune, between fold into koshi; the path from there to the head: first -> kubi
    inv = {}
    for n, v in out.items():
        inv.setdefault(v, n)
    hips = inv.get('root')
    arm = inv.get('l_sakotsu') or inv.get('l_ninoude') or inv.get('r_sakotsu') or inv.get('r_ninoude')
    if hips and arm:
        path, k = [], parent.get(arm)
        while k and out.get(k) in ('kubi', 'kubi_x', 'atama'):   # (Max Biped hangs the clavicles off the neck)
            k = parent.get(k)
        while k and k != hips:
            path.append(k)
            k = parent.get(k)
        if k == hips and path:
            path.reverse()
            for n in path:
                if out.get(n) in (None, 'koshi', 'mune', 'kubi'):
                    out.pop(n, None)
            out[path[0]] = 'koshi'
            if len(path) > 1:
                out[path[-1]] = 'mune'
            head = inv.get('atama')
            if head:
                hp, k = [], parent.get(head)
                while k and k != path[-1]:
                    hp.append(k)
                    k = parent.get(k)
                if k == path[-1] and hp:
                    hp.reverse()
                    for n in hp:
                        if out.get(n) in (None, 'kubi', 'kubi_x'):
                            out.pop(n, None)
                    out[hp[0]] = 'kubi'
    # twist without a template bone, breasts without boin -> fold
    for n, v in list(out.items()):
        if tmpl and v not in tmpl:
            out.pop(n)
    # second forearm twist -> kote_x2
    for s in 'lr':
        fx = [n for n, v in out.items() if v == '%s_kote_x' % s]
        if len(fx) > 1 and (not tmpl or '%s_kote_x2' % s in tmpl):
            out[sorted(fx)[-1]] = '%s_kote_x2' % s
    folded, unmapped = {}, []
    for n in names:
        if n in out:
            continue
        k = parent.get(n)
        while k and k not in out:
            k = parent.get(k)
        if k:
            folded[n] = out[k]
        else:
            unmapped.append(n)   # (above the hips: rig roots / armature; any weights there go to root)
    return {'rig': best, 'core_hits': hits, 'map': out, 'folded': folded, 'unmapped': unmapped}


# ---------------------------------------------------------------- self test (synthetic hierarchies)
def _chain(*ns, parent=None):
    out=[]; p=parent
    for n in ns: out.append((n,p)); p=n
    return out
def _t_mixamo():
    P='mixamorig:'; b=_chain(*[P+x for x in ('Hips','Spine','Spine1','Spine2','Neck','Head','HeadTop_End')])
    for S in ('Left','Right'):
        b+=_chain(*[P+S+x for x in ('Shoulder','Arm','ForeArm','Hand')],parent=P+'Spine2')
        for f in ('Thumb','Index','Middle','Ring','Pinky'): b+=_chain(*[P+S+'Hand'+f+str(j) for j in (1,2,3,4)],parent=P+S+'Hand')
        b+=_chain(*[P+S+x for x in ('UpLeg','Leg','Foot','ToeBase','Toe_End')],parent=P+'Hips')
    return b
def _t_ue5():
    b=[('root',None)]+_chain('pelvis','spine_01','spine_02','spine_03','spine_04','spine_05','neck_01','neck_02','head',parent='root')
    for s in 'lr':
        b+=_chain('clavicle_'+s,'upperarm_'+s,'lowerarm_'+s,'hand_'+s,parent='spine_05'); b+=[('upperarm_twist_01_'+s,'upperarm_'+s),('lowerarm_twist_01_'+s,'lowerarm_'+s),('lowerarm_twist_02_'+s,'lowerarm_'+s)]
        for f in ('thumb','index','middle','ring','pinky'):
            pre=[] if f=='thumb' else [f+'_metacarpal_'+s]
            b+=_chain(*(pre+[f+'_0%d_'%j+s for j in (1,2,3)]),parent='hand_'+s)
        b+=_chain('thigh_'+s,'calf_'+s,'foot_'+s,'ball_'+s,parent='pelvis'); b+=[('thigh_twist_01_'+s,'thigh_'+s),('calf_twist_01_'+s,'calf_'+s)]
    return b
def _t_rigify():
    b=_chain('DEF-spine','DEF-spine.001','DEF-spine.002','DEF-spine.003','DEF-spine.004','DEF-spine.005','DEF-spine.006')
    for S in ('.L','.R'):
        b+=_chain('DEF-shoulder'+S,'DEF-upper_arm'+S,'DEF-upper_arm'+S+'.001','DEF-forearm'+S,'DEF-forearm'+S+'.001','DEF-hand'+S,parent='DEF-spine.003')
        b+=[('DEF-palm.01'+S,'DEF-hand'+S)]
        for f in ('thumb','f_index','f_middle','f_ring','f_pinky'): b+=_chain(*['DEF-%s.0%d%s'%(f,j,S) for j in (1,2,3)],parent='DEF-hand'+S)
        b+=_chain('DEF-thigh'+S,'DEF-thigh'+S+'.001','DEF-shin'+S,'DEF-shin'+S+'.001','DEF-foot'+S,'DEF-toe'+S,parent='DEF-spine')
        b+=[('DEF-breast'+S,'DEF-spine.003')]
    return b
def _t_biped():
    b=_chain('Bip01','Bip01 Pelvis','Bip01 Spine','Bip01 Spine1','Bip01 Spine2','Bip01 Neck','Bip01 Head')
    for S in 'LR':
        b+=_chain(*['Bip01 %s %s'%(S,x) for x in ('Clavicle','UpperArm','Forearm','Hand')],parent='Bip01 Neck')
        b+=[('Bip01 %s ForeTwist'%S,'Bip01 %s Forearm'%S),('Bip01 %s UpArmTwist'%S,'Bip01 %s UpperArm'%S)]
        for f in range(5): b+=_chain(*['Bip01 %s Finger%d%s'%(S,f,j) for j in ('','1','2')],parent='Bip01 %s Hand'%S)
        b+=_chain(*['Bip01 %s %s'%(S,x) for x in ('Thigh','Calf','Foot','Toe0')],parent='Bip01 Pelvis')
    return b
def _t_wwe():
    import w13_skeletons
    return [(n[0], w13_skeletons.NODES13[n[3]][0] if n[3]>=0 else None) for n in w13_skeletons.NODES13]
def _t_generic():  # an unlisted rig: 'Hip', 'L_UpperArm' etc.
    b=_chain('Root','Hip','Waist','Torso','UpperTorso','Neck1','Head1','HairBase','Hair1',)
    for S in ('L','R'):
        b+=_chain(*['%s_%s'%(S,x) for x in ('Collar','UpperArm','ForeArm','Hand')],parent='UpperTorso')
        b+=_chain(*['%s_%s'%(S,x) for x in ('Thigh','Shin','Foot','Toe')],parent='Hip')
    return b

SELFTEST = {'mixamo': _t_mixamo, 'ue5': _t_ue5, 'rigify': _t_rigify, 'max_biped': _t_biped, 'wwe_biped': _t_wwe,
            'generic': _t_generic}


def selftest(dump=None):
    """every synthetic rig must map all core bones; -> {rig: result}; dump: write <rig>.json {bones, expect}"""
    sk = json.load(open(os.path.join(HERE, 'svr11_skeleton.json'), encoding='utf-8'))
    tmpl = [b['name'] for b in sk['bones']]
    out, ok = {}, True
    for name, fn in SELFTEST.items():
        bones = fn()
        r = match(bones, tmpl)
        miss = sorted(c for c in CORE if c not in set(r['map'].values()))
        ok &= not miss
        out[name] = {'rig': r['rig'], 'mapped': len(r['map']), 'folded': len(r['folded']), 'missing_core': miss}
        if dump:
            os.makedirs(dump, exist_ok=True)
            with open(os.path.join(dump, name + '.json'), 'w', encoding='utf-8', newline=chr(10)) as f:
                json.dump({'bones': bones, 'expect': r}, f, indent=1)
    return ok, out


def build_json():
    return {'version': 1,
            'targets': 'tools/svr11_skeleton.json bone names',
            'compare': 'case-insensitive, after stripping a namespace (text up to the last ":" "|" "/") and a '
                       '"Bip01 " / "Bip001 " prefix',
            'rules': RULES,
            'rigs': {k: {'label': v['label'], 'map': v['map'], **({'same_names': True} if v.get('same_names') else {})}
                     for k, v in RIGS.items()},
            'core': sorted(CORE),
            'generic': {'aliases': GENERIC, 'fingers': FINGER_ALIASES, 'joint_words': JOINT_WORDS,
                        'targets': {'centre': CENTRE, 'limb': LIMB},
                        'side': 'Left / Right prefix or word, .L / .R / _L / _R / " L" suffix, L_ / l_ prefix'},
            'reference': 'tools/rig_maps.py match() implements these rules'}


def main(argv):
    if len(argv) >= 2 and argv[1] == 'selftest':
        ok, res = selftest(argv[2] if len(argv) > 2 else None)
        print(json.dumps(res, indent=1))
        return 0 if ok else 3
    if len(argv) >= 3 and argv[1] == 'match':
        bones = json.load(open(argv[2], encoding='utf-8'))
        sk = json.load(open(os.path.join(HERE, 'svr11_skeleton.json'), encoding='utf-8'))
        print(json.dumps(match(bones, [b['name'] for b in sk['bones']]), indent=1))
        return 0
    out = os.path.join(HERE, 'rig_maps.json')
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(build_json(), f, indent=1)
    print(out, {k: len(v['map']) for k, v in RIGS.items()})
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
