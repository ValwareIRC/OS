"""A casino set made only with osui, as the companion would make one: a stack of poker chips (each a part:
the top one deals), a die, and a gold-trimmed "Play" plaque. Exported to $OS_MODELS/casino.glb.
  OS_MODELS=build/models PYTHONPATH=userland/rootfs/usr/share/os/blender blender -b -P tests/blender/casino.py"""
import math
from osui import *

reset()
root = empty("Casino")

# poker chips: a body, a rim of alternating stripes, an inset face
chip_cols = ["#b0101c", "#0f2f8a", "#0b6b34", "#14141a"]
for i, col in enumerate(chip_cols):
    chip = empty(f"chip-{i}", parent=root)
    chip.location = (-1.1, 0, -0.42 + i * 0.13)
    body = cylinder(0.5, 0.12, material=mat("ceramic", col), axis="Z", parent=chip, name=f"chip-body-{i}")
    for k in range(8):  # edge inserts
        a = k * 45
        s = ring_slice(0.40, 0.505, a, a + 18, depth=0.124, bevel_width=0.004, material=mat("ceramic", "#f4f0e6"),
                       parent=chip, name=f"stripe-{i}-{k}", facing="Z")
        s.location = (0, 0, -0.062)
    face = cylinder(0.33, 0.126, material=mat("ceramic", "#f4f0e6"), axis="Z", parent=chip, name=f"chip-face-{i}",
                    bevel_width=0.006)
    inner = cylinder(0.29, 0.128, material=mat("ceramic", col), axis="Z", parent=chip, name=f"chip-inner-{i}",
                     bevel_width=0.004)
    if i == len(chip_cols) - 1:
        part(chip, action="open:store", hover="lift", title="Deal")

# a die: a rounded ivory cube with sunk black pips
die = box((0.7, 0.7, 0.7), at=(0.25, 0, -0.1), material=mat("ceramic", "#f6f1e4"), bevel_width=0.09, parent=root,
          name="die")
die.rotation_euler = (math.radians(12), math.radians(-8), math.radians(28))
pips = {(0, -1, 0): [(0, 0)], (1, 0, 0): [(-1, -1), (1, 1)], (0, 0, 1): [(-1, -1), (0, 0), (1, 1)]}
black = mat("obsidian")
for (nx, ny, nz), spots in pips.items():
    for u, v in spots:
        p = sphere(0.06, material=black, parent=die, name="pip")
        o = 0.18
        if ny:
            p.location = (u * o, -0.355, v * o)
        elif nx:
            p.location = (0.355, u * o, v * o)
        else:
            p.location = (u * o, v * o, 0.355)
        p.scale = (1, 0.5, 1) if ny else (0.5, 1, 1) if nx else (1, 1, 0.5)
part(die, action="toggle:#games", hover="lift", title="Games")

# a plaque: lacquered, gold edge, raised gold lettering
plaque = empty("plaque", parent=root)
plaque.location = (1.45, 0, -0.2)
box((1.1, 0.12, 0.5), material=mat("lacquer", "#5a0812"), bevel_width=0.04, parent=plaque, name="plaque-body")
box((1.18, 0.1, 0.58), material=mat("gold"), bevel_width=0.04, parent=plaque, name="plaque-edge").location = (0, 0.02, 0)
text("PLAY", size=0.24, depth=0.02, at=(0, -0.075, -0.07), material=mat("gold"), parent=plaque, name="plaque-text")
part(plaque, action="open:arcade", hover="both", title="Play")

turn(root, "Z", 20, frames=48, name="idle")
export("casino")
