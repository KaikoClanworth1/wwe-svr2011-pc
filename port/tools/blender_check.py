# Headless Blender check of an arena export (run with blender -b --factory-startup):
#   blender -b --factory-startup --python blender_check.py -- <arena.fbx> <out.png> [view]
# Imports the FBX, prints object/material/texture counts, a few custom
# properties and the scene bounds (metres), and renders a textured view.
import sys
import bpy
import mathutils

argv = sys.argv[sys.argv.index("--") + 1:]
fbx, out = argv[0], argv[1]
view = argv[2] if len(argv) > 2 else "persp"

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=fbx)
objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
mats = {s.material for o in objs for s in o.material_slots if s.material}
imgs = [i for i in bpy.data.images if i.source == 'FILE']
missing = [i.name for i in imgs if not i.has_data and not i.packed_file and not __import__('os').path.exists(bpy.path.abspath(i.filepath))]
print(f"CHECK objects {len(objs)} materials {len(mats)} images {len(imgs)} missing {len(missing)} {missing[:5]}")
for o in objs[:3]:
    print("CHECK props", o.name, {k: o[k] for k in o.keys() if k.startswith('svr_')})
lo = mathutils.Vector((1e9, 1e9, 1e9)); hi = -lo
for o in objs:
    for c in o.bound_box:
        w = o.matrix_world @ mathutils.Vector(c)
        lo = mathutils.Vector(map(min, lo, w)); hi = mathutils.Vector(map(max, hi, w))
print("CHECK bounds m", tuple(round(v, 2) for v in lo), tuple(round(v, 2) for v in hi))
ring = [o for o in objs if o.get('svr_name') == 'ar_ring']
if ring:
    r = ring[0]
    bb = [r.matrix_world @ mathutils.Vector(c) for c in r.bound_box]
    print("CHECK ring mat size m", round(max(v.x for v in bb) - min(v.x for v in bb), 2), round(max(v.y for v in bb) - min(v.y for v in bb), 2), "z", round(min(v.z for v in bb), 2), round(max(v.z for v in bb), 2))

scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'FLAT'
scene.display.shading.color_type = 'TEXTURE'
scene.render.resolution_x, scene.render.resolution_y = 1280, 720
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
scene.collection.objects.link(cam)
scene.camera = cam
cam.data.clip_end = 1000
if view == "top":
    cam.location = (0, 0, 200); cam.rotation_euler = (0, 0, 0)
    cam.data.type = 'ORTHO'; cam.data.ortho_scale = 240
elif view == "ring":
    cam.location = (7, -7, 4.0)
    d = mathutils.Vector((0, 0, 1.0)) - cam.location
    cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
else:
    cam.location = (0, -30, 12)
    d = mathutils.Vector((0, 0, 1)) - cam.location
    cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
scene.render.filepath = out
bpy.ops.render.render(write_still=True)
print("CHECK rendered", out)
