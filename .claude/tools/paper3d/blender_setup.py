# Makes ready-to-open Blender files from an exported map, for someone opening Blender for the first
# time: double-click the .blend and the map is there, textured, the camera on the vault dweller.
#
#   blender -b --factory-startup --python blender_setup.py -- <MAP.glb> <MAP.blend> [dude.blend] [preview.png]
#
# MAP.blend   the whole map; viewport in Material Preview (textures on), view on the dude, and the
#             scene camera set to the game's own view (Numpad 0 shows the map as Fallout draws it)
# dude.blend  only the vault dweller and his walk/run frames -- a small, calm file to learn on
# preview.png a render through the game's camera, to check the file without opening Blender

import bpy, sys, math
from mathutils import Euler, Vector

args = sys.argv[sys.argv.index('--') + 1:]
glb, out_map = args[0], args[1]
out_dude = args[2] if len(args) > 2 else None
out_png = args[3] if len(args) > 3 else None


def clear_scene():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)


def purge():
    # data nothing uses any more (the startup cube's mesh and material, the default camera...)
    for coll in (bpy.data.meshes, bpy.data.materials, bpy.data.cameras, bpy.data.lights, bpy.data.images):
        for block in list(coll):
            if block.users == 0:
                coll.remove(block)


def set_views(target, distance, rot):
    # every 3D view in every workspace: textures on, looking at target
    for screen in bpy.data.screens:
        for area in screen.areas:
            if area.type != 'VIEW_3D':
                continue
            space = area.spaces.active
            space.shading.type = 'MATERIAL'
            space.clip_end = 2000
            r3d = space.region_3d
            r3d.view_perspective = 'PERSP'
            r3d.view_location = target
            r3d.view_distance = distance
            r3d.view_rotation = rot.to_quaternion()


clear_scene()
bpy.ops.import_scene.gltf(filepath=glb)
purge()
scene = bpy.context.scene
objs = bpy.data.objects
# The art is unlit pixel colour: Blender's default AgX view transform would wash it out.
scene.view_settings.view_transform = 'Standard'
scene.view_settings.look = 'None'

cam = objs.get('Original_Camera')
if cam:
    scene.camera = cam
dude = objs.get('Dude')
target = (dude.matrix_world.translation + Vector((0, 0, 1))) if dude else Vector((0, 0, 1))
three_quarter = Euler((math.radians(68), 0, math.radians(-30)))
set_views(target, 9.0, three_quarter)
root = bpy.data.collections.get('Collection')
if root and objs.get(glb.replace('\\', '/').split('/')[-1].split('.')[0]):
    root.name = glb.replace('\\', '/').split('/')[-1].split('.')[0]

if out_png and cam:
    scene.render.engine = 'BLENDER_EEVEE' if 'BLENDER_EEVEE' in [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties['engine'].enum_items] else scene.render.engine
    scene.render.resolution_x, scene.render.resolution_y = 960, 540
    scene.render.film_transparent = False
    scene.world = scene.world or bpy.data.worlds.new('World')
    scene.render.filepath = out_png
    bpy.ops.render.render(write_still=True)
    print('PREVIEW', out_png)

bpy.ops.wm.save_as_mainfile(filepath=out_map, compress=True)
print('SAVED', out_map)

if out_dude:
    # keep only the dude and his reference frames, stood at the origin facing Blender's front view
    keep = {'Dude', 'Reference_Dude_Animation'}
    ref = objs.get('Reference_Dude_Animation')
    if ref:
        keep |= {c.name for c in ref.children}
    for o in list(objs):
        if o.name not in keep:
            bpy.data.objects.remove(o, do_unlink=True)
    purge()
    # (the glTF importer leaves objects in quaternion rotation mode, where rotation_euler is ignored)
    if dude:
        dude.parent = None
        dude.rotation_mode = 'XYZ'
        dude.location = (0, 0, 0)
        dude.rotation_euler = (0, 0, math.radians(-90))  # his forward (+X) turned to face the front view (-Y)
    if ref:
        ref.rotation_mode = 'XYZ'
        ref.location = (3, 0, 0)
        ref.rotation_euler = (0, 0, 0)
    set_views(Vector((0, 0, 1)), 6.0, Euler((math.radians(80), 0, math.radians(-20))))
    scene.camera = None
    bpy.ops.wm.save_as_mainfile(filepath=out_dude, compress=True)
    print('SAVED', out_dude)
