# Models side by side, one row each: the original sprite, the model seen through the GAME's camera
# (same direction, 25.7 deg from above -- it should match the sprite), then front, right, back,
# left at eye level. For judging sprite3d.js against the old voxel carve.
#
#   blender -b --factory-startup --python blender_compare.py -- <out.png> <sprite.png> <a.glb> [<b.glb> ...]
#
# Models are in the critter's own axes as sprite3d.js writes them: +X forward, +Y up, +Z to his
# right (Blender's importer turns +Y up into +Z up, so his right becomes -Y). The sprite column is
# direction SE, the one the game camera looks at from the front right.

import bpy, sys, os, math
import numpy as np
from mathutils import Vector

args = sys.argv[sys.argv.index('--') + 1:]
out_png, sprite_png, glbs = args[0], args[1], args[2:]
W, H = 384, 640
SIN = math.sqrt(3) / 4
COS = math.sqrt(13) / 4

scene = bpy.context.scene
scene.render.engine = 'BLENDER_WORKBENCH'
scene.display.shading.light = 'STUDIO'
scene.display.shading.show_specular_highlight = False  # matte: the sprite colours, not a plastic shine
scene.view_settings.view_transform = 'Standard'
scene.world = scene.world or bpy.data.worlds.new('World')
scene.world.color = (0.92, 0.92, 0.92)
scene.render.resolution_x, scene.render.resolution_y, scene.render.resolution_percentage = W, H, 100
scene.render.film_transparent = False
cam_data = bpy.data.cameras.new('compare')
cam_data.type = 'ORTHO'
cam = bpy.data.objects.new('compare', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam

# directions TOWARDS the camera, in Blender axes (x forward, y = his left, z up)
a = math.radians(60)  # direction SE: the game camera sees him from 60 deg round his front
VIEWS = [
    ('game', Vector((COS * math.sin(a), -COS * math.cos(a), SIN))),
    ('front', Vector((1, 0, 0))),
    ('right', Vector((0, -1, 0))),
    ('back', Vector((-1, 0, 0))),
    ('left', Vector((0, 1, 0))),
]


def tile_of(path):
    img = bpy.data.images.load(path)
    w, h = img.size
    px = np.empty(w * h * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return px.reshape(h, w, 4)


def sprite_tile(path):
    # the sprite cropped to what isn't white, fitted into a tile (nearest neighbour: pixels stay pixels)
    px = tile_of(path)
    solid = np.where(px[:, :, :3].min(axis=2) < 0.99)
    y0, y1, x0, x1 = solid[0].min(), solid[0].max() + 1, solid[1].min(), solid[1].max() + 1
    crop = px[y0:y1, x0:x1]
    k = min(W * 0.9 / crop.shape[1], H / 1.1 / crop.shape[0])
    ch, cw = int(crop.shape[0] * k), int(crop.shape[1] * k)
    ys = (np.arange(ch) / k).astype(int).clip(0, crop.shape[0] - 1)
    xs = (np.arange(cw) / k).astype(int).clip(0, crop.shape[1] - 1)
    tile = np.ones((H, W, 4), dtype=np.float32)
    oy, ox = (H - ch) // 2, (W - cw) // 2
    tile[oy:oy + ch, ox:ox + cw] = crop[ys][:, xs]
    return tile


rows = []
sprite = sprite_tile(sprite_png)
for glb in glbs:
    for o in list(bpy.data.objects):
        if o != cam:
            bpy.data.objects.remove(o, do_unlink=True)
    bpy.ops.import_scene.gltf(filepath=glb)
    meshes = [o for o in scene.objects if o.type == 'MESH']
    textured = any(n.type == 'TEX_IMAGE' for o in meshes for m in o.data.materials if m and m.use_nodes for n in m.node_tree.nodes)
    scene.display.shading.color_type = 'TEXTURE' if textured else 'VERTEX'
    pts = [o.matrix_world @ Vector(c) for o in meshes for c in o.bound_box]
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    centre, size = (lo + hi) / 2, hi - lo
    cam_data.ortho_scale = max(size.z, max(size.x, size.y) * H / W) * 1.1
    tiles = [sprite]
    for name, d in VIEWS:
        cam.location = centre + d.normalized() * max(size) * 4
        cam.rotation_euler = (-d).to_track_quat('-Z', 'Y').to_euler()  # looks down its -Z, its +Y up
        scene.render.filepath = os.path.splitext(out_png)[0] + '_tmp.png'
        bpy.ops.render.render(write_still=True)
        tiles.append(tile_of(scene.render.filepath))
        os.remove(scene.render.filepath)
    rows.append(np.concatenate(tiles, axis=1))
    print('ROW', os.path.basename(glb), 'colour', scene.display.shading.color_type, 'size', tuple(round(v, 3) for v in size))

sheet = np.concatenate(rows[::-1], axis=0)  # Blender images start at the bottom: first model on top
img = bpy.data.images.new('sheet', sheet.shape[1], sheet.shape[0])
img.pixels.foreach_set(sheet.ravel())
img.filepath_raw, img.file_format = out_png, 'PNG'
img.save()
print('SHEET', out_png)
