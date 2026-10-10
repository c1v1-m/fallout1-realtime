# A set piece's model as a .blend to edit (setpieces.js): its .glb imported, colours on, the view
# on it. From then on build.js exports the .glb from the .blend whenever the .blend is newer --
# edit, Ctrl+S, rebuild.
#
#   blender -b --factory-startup --python blender_setpiece.py -- <model.glb> <model.blend>

import bpy, sys, math
from mathutils import Vector, Euler

args = sys.argv[sys.argv.index('--') + 1:]
glb, out = args[0], args[1]

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=glb)
for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.cameras, bpy.data.lights):
    for block in list(coll):
        if block.users == 0:
            coll.remove(block)

scene = bpy.context.scene
scene.view_settings.view_transform = 'Standard'  # the colours as painted, not washed out by AgX
meshes = [o for o in scene.objects if o.type == 'MESH']
pts = [o.matrix_world @ Vector(c) for o in meshes for c in o.bound_box]
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
centre, size = (lo + hi) / 2, max(hi - lo)
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type == 'VIEW_3D':
            space = area.spaces.active
            space.shading.type = 'MATERIAL'
            space.region_3d.view_perspective = 'PERSP'
            space.region_3d.view_location = centre
            space.region_3d.view_distance = size * 1.6
            space.region_3d.view_rotation = Euler((math.radians(65), 0, math.radians(35))).to_quaternion()
bpy.ops.wm.save_as_mainfile(filepath=out, compress=True)
print('SAVED', out)
