"""A picture of a model with Cycles, lit like the desktop's studio (the same dark room, soft key light, rim strips
and floor the live renderer uses), framed by its own camera or fitted, on a transparent background.
  os-blender render FILE.glb|.blend OUT.png [W H]"""
import bpy, math, os, sys
from mathutils import Vector

args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
src, out = args[0], args[1]
w = int(args[2]) if len(args) > 2 else 1200
h = int(args[3]) if len(args) > 3 else 1200

if src.lower().endswith(".blend"):
    bpy.ops.wm.open_mainfile(filepath=src)
else:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=src)
scene = bpy.context.scene

# the studio: a dark room, a large warm soft box from the top left, two cool rim strips behind, a fill card
world = bpy.data.worlds.new("Studio")
world.use_nodes = True
bg = world.node_tree.nodes["Background"]
bg.inputs["Color"].default_value = (0.012, 0.012, 0.014, 1)
bg.inputs["Strength"].default_value = 1.0
scene.world = world

objs = [o for o in scene.objects if o.type in {"MESH", "CURVE", "FONT"}]
lo = Vector((1e9, 1e9, 1e9))
hi = Vector((-1e9, -1e9, -1e9))
for o in objs:
    for c in o.bound_box:
        p = o.matrix_world @ Vector(c)
        lo = Vector(map(min, lo, p))
        hi = Vector(map(max, hi, p))
centre = (lo + hi) / 2 if objs else Vector((0, 0, 0))
radius = max((hi - lo).length / 2, 0.01) if objs else 1.0


def light(name, kind, loc, energy, size, colour, size_y=None):
    data = bpy.data.lights.new(name, kind)
    data.energy = energy
    data.color = colour
    if kind == "AREA":
        data.shape = "RECTANGLE"
        data.size = size
        data.size_y = size_y or size
    ob = bpy.data.objects.new(name, data)
    scene.collection.objects.link(ob)
    ob.location = centre + Vector(loc) * radius
    direction = centre - ob.location
    ob.rotation_euler = direction.to_track_quat("-Z", "Y").to_euler()
    return ob


k = radius * radius
light("Key", "AREA", (-2.2, -2.6, 2.8), 900 * k, 2.4 * radius, (1.0, 0.96, 0.9))
light("Rim L", "AREA", (-2.6, 2.4, 1.2), 500 * k, 0.4 * radius, (0.9, 0.95, 1.0), 3.0 * radius)
light("Rim R", "AREA", (2.6, 2.4, 1.2), 350 * k, 0.4 * radius, (0.9, 0.95, 1.0), 3.0 * radius)
light("Fill", "AREA", (0.6, -3.2, 0.2), 120 * k, 3.0 * radius, (1.0, 1.0, 1.0), 1.0 * radius)

# the camera: its own, or one fitted in front, a little above, as the live renderer frames it
if not scene.camera:
    cam = bpy.data.cameras.new("Camera")
    cam.lens = 70
    co = bpy.data.objects.new("Camera", cam)
    scene.collection.objects.link(co)
    fov = 2 * math.atan(cam.sensor_width / (2 * cam.lens))
    dist = radius / math.sin(fov / 2) * 1.08
    co.location = centre + Vector((0.35, -1.0, 0.3)).normalized() * dist
    co.rotation_euler = (centre - co.location).to_track_quat("-Z", "Y").to_euler()
    scene.camera = co

scene.render.engine = "CYCLES"
scene.cycles.samples = 96
scene.cycles.use_denoising = True
scene.render.film_transparent = True
scene.render.resolution_x, scene.render.resolution_y = w, h
scene.render.resolution_percentage = 100
try:
    scene.view_settings.view_transform = "Khronos PBR Neutral"  # the same tone mapping as the live renderer
except TypeError:
    scene.view_settings.view_transform = "Standard"
scene.render.image_settings.file_format = "PNG"
scene.render.image_settings.color_mode = "RGBA"
scene.render.filepath = out
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
bpy.ops.render.render(write_still=True)
print("osui: rendered", out)
