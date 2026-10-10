# Renders a model (.glb) from four sides -- 0, 90, 180, 270 degrees around it -- into one picture,
# and optionally saves it as a .blend ready to double-click (textures on, the view on the model).
#
#   blender -b --factory-startup --python blender_preview.py -- <model.glb> <out.png> [out.blend]

import bpy, sys, os, math
import numpy as np
from mathutils import Vector, Euler

args = sys.argv[sys.argv.index('--') + 1:]
glb, out_png = args[0], args[1]
out_blend = args[2] if len(args) > 2 else None

for o in list(bpy.data.objects):
    bpy.data.objects.remove(o, do_unlink=True)
bpy.ops.import_scene.gltf(filepath=glb)
scene = bpy.context.scene
meshes = [o for o in scene.objects if o.type == 'MESH']
pts = [o.matrix_world @ Vector(c) for o in meshes for c in o.bound_box]
lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
centre, size = (lo + hi) / 2, hi - lo
print('MODEL %d meshes, %d vertices, %d faces, size %s' % (len(meshes), sum(len(o.data.vertices) for o in meshes),
      sum(len(o.data.polygons) for o in meshes), tuple(round(v, 3) for v in size)))
for m in bpy.data.materials:
    if m.use_nodes:
        print('MATERIAL', m.name, ['%s %dx%d' % (n.image.name, *n.image.size) for n in m.node_tree.nodes if n.type == 'TEX_IMAGE' and n.image])

# Workbench with texture colour and studio light: the texture as painted, the shape readable
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.display.shading.color_type = 'TEXTURE'
scene.view_settings.view_transform = 'Standard'
scene.world = scene.world or bpy.data.worlds.new('World')
scene.world.color = (0.92, 0.92, 0.92)
W, H = 512, 768
scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage = W, H, 100
scene.render.film_transparent = False

cam_data = bpy.data.cameras.new('preview')
cam_data.type = 'ORTHO'
cam_data.ortho_scale = max(size.z, max(size.x, size.y) * H / W) * 1.1  # spans the taller side
cam = bpy.data.objects.new('preview', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam

tiles, stem = [], os.path.splitext(out_png)[0]
for a in (0, 90, 180, 270):
    r, d = math.radians(a), max(size) * 3
    cam.location = centre + Vector((math.sin(r) * d, -math.cos(r) * d, 0))
    cam.rotation_euler = Euler((math.radians(90), 0, r))  # looking at the centre, level
    scene.render.filepath = '%s_%d.png' % (stem, a)
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(scene.render.filepath)
    px = np.empty(W * H * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    tiles.append(px.reshape(H, W, 4))
    bpy.data.images.remove(img)
    os.remove(scene.render.filepath)
sheet = bpy.data.images.new('sheet', W * 4, H)
sheet.pixels.foreach_set(np.concatenate(tiles, axis=1).ravel())
sheet.filepath_raw, sheet.file_format = out_png, 'PNG'
sheet.save()
bpy.data.images.remove(sheet)
print('PREVIEW', out_png)

if out_blend:
    bpy.data.objects.remove(cam, do_unlink=True)
    bpy.data.cameras.remove(cam_data)
    scene.render.engine = 'BLENDER_EEVEE_NEXT' if 'BLENDER_EEVEE_NEXT' in [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties['engine'].enum_items] else 'BLENDER_EEVEE'
    for screen in bpy.data.screens:  # every 3D view: textures on, looking at the model
        for area in screen.areas:
            if area.type == 'VIEW_3D':
                space = area.spaces.active
                space.shading.type = 'MATERIAL'
                space.region_3d.view_perspective = 'PERSP'
                space.region_3d.view_location = centre
                space.region_3d.view_distance = max(size) * 2.2
                space.region_3d.view_rotation = Euler((math.radians(80), 0, math.radians(-20))).to_quaternion()
    bpy.ops.wm.save_as_mainfile(filepath=out_blend, compress=True)
    print('SAVED', out_blend)
