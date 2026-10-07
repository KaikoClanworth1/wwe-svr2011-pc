"""Raw Is War (1997) from WWE '13 textures on SvR2011's RAW arena (bg01).

The WWE '13 export (a folder of PNGs, same Yuke's naming) has no models: the
arena keeps RAW's geometry. Textures with the same name replace RAW's; the
Raw Is War-only ones go on the parts with the same role (titantron, curtain,
barrier, ads, stands, VIP boxes, W logos). Also a banner and a VS screen
background. Writes runs/riw/raw_is_war.svrmod (and the map and pictures).

    python riw_build.py [<WWE13 folder>]
"""
import os
import subprocess
import sys
import zipfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
SRC = sys.argv[1] if len(sys.argv) > 1 else r'D:\Xbox Games Ports\Arenas R\Raw Is War 1997  WWE13'
GAME = os.path.join(PORT, 'runs', 'opt_arena_game')
OUT = os.path.join(PORT, 'runs', 'riw')
SVRMOD = os.path.join(PORT, 'out', 'modmaker', 'svrmod.exe')
BG01 = os.path.join(GAME, 'pac', 'bg', 'bg01.pac')


def src(name):
    return os.path.join(SRC, name + '.png')


def composite():
    os.makedirs(OUT, exist_ok=True)
    pics = {}
    # the long billboard (1024 x 64): four ads side by side
    bb = Image.new('RGBA', (1024, 64))
    for i, n in enumerate(['ad_1', 'ad_3', 'ad_4', 'ad_2']):
        bb.paste(Image.open(src(n)).convert('RGBA').resize((256, 64)), (256 * i, 0))
    pics['billboard'] = os.path.join(OUT, 'billboard.png')
    bb.save(pics['billboard'])
    # banner: the RAW IS WAR flag on black
    ban = Image.new('RGBA', (256, 128), (0, 0, 0, 255))
    flag = Image.open(src('flag3b')).convert('RGBA').resize((256, 128))
    ban.alpha_composite(flag)
    pics['banner'] = os.path.join(OUT, 'banner.png')
    ban.save(pics['banner'])
    # VS background (RAW theme RAW01, 1024 x 512): the red curtain, the flag in the middle
    vs = Image.new('RGBA', (1024, 512))
    cur = Image.open(src('raw_curtain')).convert('RGBA').resize((256, 512))
    for x in range(0, 1024, 256):
        vs.paste(cur, (x, 0))
    logo = Image.open(src('flag3b')).convert('RGBA').resize((640, 320))
    vs.alpha_composite(logo, (192, 60))
    pics['vs_bg'] = os.path.join(OUT, 'vs_raw01.png')
    vs.save(pics['vs_bg'])
    # VS plate (RAW02, 1024 x 128): the RAW IS WAR strip
    plate = Image.new('RGBA', (1024, 128), (0, 0, 0, 255))
    strip = Image.open(src('rin_ep00')).convert('RGBA').resize((1024, 256)).crop((0, 64, 1024, 192))
    plate.alpha_composite(strip)
    pics['vs_plate'] = os.path.join(OUT, 'vs_raw02.png')
    plate.save(pics['vs_plate'])
    return pics


def texture_map(pics):
    have = {os.path.splitext(f)[0] for f in os.listdir(SRC) if f.endswith('.png')}
    m = {}
    # 1. same names
    for n in sorted(have):
        m[n] = src(n)
    # 2. Raw Is War on RAW's own parts, by role
    role = {
        'raw_titan': 'flag3b', 'raw_titan_l': 'flag3a', 'RAWtest01': 'flag3b',
        'st_door00': 'raw_curtain', 'st_door_lig00': 'raw_curtain', 'st_cloth00': 'raw_ent0',
        'ar_fence': 'fleet_fence3', 'ar_fence2': 'fleet_fence4', 'ar_fen_wwe': 'ad_1',
        'st_vip00': 'fleet_vip1', 'st_vip01': 'fleet_vip2', 'st_vip02': 'fleet_vip5', 'st_vip03': 'fleet_vip8',
        'st_vip04': 'fleet_vip6',
        'st_wall00': 'fleet_wall3', 'st_wall01': 'fleet_wall2', 'st_wall11': 'fleet_wall5', 'st_wall20': 'ad_4',
        'st_roof00': 'fleet_roof3', 'st_roof01': 'fleet_roof1', 'st_roof02': 'fleet_roof2',
        'st_tv00': 'fleet_score0', 'st_tv01': 'fleet_score1', 'st_sheat02': 'fleet_2d_audi',
        'slo_00': 'raw_slope_01', 'slo_window': 'raw_slope_00', 'raw_window': 'raw_slope_03',
        'raw_tiwall': 'raw_pipe0',
        'wwe_wlogo0': 'fleet_flag1', 'wwe_wlogo1': 'fleet_flag2', 'wwe_wlogo5': 'ji_mark', 'wwe_wlogo6': 'fleet_flag1',
        'wwe_wlogo7': 'fleet_flag2',
    }
    for i in range(10):  # the titantron's animation: Raw Is War flags in turn
        role['raw_anim%02d' % i] = 'flag3a' if i % 2 else 'flag3b'
    for t, s in role.items():
        m[t] = src(s)
    m['st_billboard'] = pics['billboard']
    path = os.path.join(OUT, 'map.txt')
    with open(path, 'w') as f:
        for t, s in m.items():
            f.write('%s=%s\n' % (t, s))
    return path


def main():
    pics = composite()
    mp = texture_map(pics)
    pac = os.path.join(OUT, 'arena.pac')
    r = subprocess.run([SVRMOD, 'retexture', BG01, mp, pac], capture_output=True, text=True)
    print(r.stdout.strip())
    if r.returncode:
        sys.exit(r.stderr)
    for name, w, h, key in [('banner', 256, 128, 'banner'), ('RAW01', 1024, 512, 'vs_bg'), ('RAW02', 1024, 128, 'vs_plate')]:
        subprocess.run([SVRMOD, 'dds', pics[key], str(w), str(h), os.path.join(OUT, name + '.dds')], check=True)
    manifest = ('type=arena\nid=raw_is_war\nname=Raw Is War 1997\nauthor=WWE 13 textures (test)\nversion=1.0\n'
                "base=arena_RAW\nmade_with=Port tools (WWE '13)\n")
    out = os.path.join(OUT, 'raw_is_war.svrmod')
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_STORED) as z:
        z.writestr('manifest.txt', manifest)
        z.write(pac, 'arena.pac')
        z.write(os.path.join(OUT, 'banner.dds'), 'banner.dds')
        z.write(os.path.join(OUT, 'RAW01.dds'), 'vs/RAW01.dds')
        z.write(os.path.join(OUT, 'RAW02.dds'), 'vs/RAW02.dds')
    print('wrote', out, os.path.getsize(out))


if __name__ == '__main__':
    main()
