# Headless Blender round-trip edit (Phase 3 test), run with:
#   blender -b --factory-startup --python blender_edit_test.py -- <export dir> <out.fbx>
# Imports <export dir>/arena.fbx, makes edits a modder would, and exports
# an FBX the way Blender's exporter does by default:
#  - a new 2 m cube "mod_cube" between the ring and the ramp, with a new
#    texture textures/mod_cube.png (magenta/white grid)
#  - the rope texture ar_rop01.png repainted red/black (barbed-wire look)
import os
import sys
import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
src, out = argv[0], argv[1]
# mode: "full" (default) or "cube-only" (the cube uses an existing arena
# texture and nothing else changes)
mode = argv[2] if len(argv) > 2 else "full"
tex_dir = os.path.join(src, "textures")

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=os.path.join(src, "arena.fbx"))

# new texture
if mode == "cube-only":
    img = bpy.data.images.load(os.path.join(tex_dir, "rin_mat00.png"))
else:
  img = bpy.data.images.new("mod_cube", 256, 256)
  px = []
  for y in range(256):
      for x in range(256):
          on = ((x // 32) + (y // 32)) % 2 == 0
          px += [1.0, 0.0, 1.0, 1.0] if on else [1.0, 1.0, 1.0, 1.0]
  img.pixels = px
  img.filepath_raw = os.path.join(tex_dir, "mod_cube.png")
  img.file_format = 'PNG'
  img.save()

mat = bpy.data.materials.new("mod_cube_mat")
mat.use_nodes = True
bsdf = next(n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
tn = mat.node_tree.nodes.new('ShaderNodeTexImage')
tn.image = img
mat.node_tree.links.new(tn.outputs['Color'], bsdf.inputs['Base Color'])

bpy.ops.mesh.primitive_cube_add(size=2.0, location=(0.0, 8.0, 1.0))
cube = bpy.context.active_object
cube.name = "mod_cube"
cube.data.materials.append(mat)
bpy.ops.object.mode_set(mode='EDIT')
bpy.ops.uv.smart_project()
bpy.ops.object.mode_set(mode='OBJECT')

# rope texture: red/black stripes
rope = os.path.join(tex_dir, "ar_rop01.png")
if os.path.exists(rope) and mode == "full":
    r = bpy.data.images.load(rope)
    w, h = r.size
    px = []
    for y in range(h):
        for x in range(w):
            on = (x // max(1, w // 16)) % 2 == 0
            px += [0.85, 0.05, 0.05, 1.0] if on else [0.05, 0.05, 0.05, 1.0]
    r.pixels = px
    r.save()

bpy.ops.export_scene.fbx(filepath=out, use_custom_props=True, path_mode='RELATIVE')
print("EDIT done", out)
