#!/usr/bin/env python3
"""Writes the README's gallery of starter designs from the themes themselves
(their names and the title in theme-about), between the markers
<!-- gallery --> and <!-- /gallery -->, two to a row, pictures from
docs/screenshots/<name>.jpg.
  python3 tests/theme_gallery.py"""
import html, os, re

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
themes = os.path.join(root, "userland", "rootfs", "usr", "share", "os", "themes")
names = sorted(f[:-4] for f in os.listdir(themes) if f.endswith(".xml"))
CAPTION = {
    "aqua": "glass menu bar and dock", "aquarium": "apps are bubbles among the fish", "arcade": "8-bit title screen",
    "bento": "a grid of widget cards", "beos": "Deskbar and yellow tabs", "blueprint": "apps as drawn parts",
    "brutalist": "enormous raw type", "calm": "a clock and a few words", "commander": "Norton Commander, two panes",
    "corkboard": "notes and polaroids pinned up", "crt": "green phosphor terminal", "cube": "a turning 3D cube of apps",
    "frost": "apps orbit the orb", "fsn": "3D file system, after Jurassic Park", "gears": "clockwork and brass plaques",
    "hud": "a targeting reticle", "luna": "Windows XP, made modern", "matrix": "falling code, red pill or blue",
    "metro": "Windows 8 live tiles", "neon": "synthwave drive", "notebook": "a handwritten to-do page",
    "orrery": "apps are planets", "paper": "a newspaper front page", "platinum": "Mac OS 9",
    "progman": "Windows 3.1", "radar": "apps are contacts on the scope", "shelf": "apps are books",
    "sky": "the real sky, apps are clouds", "subway": "apps are stations", "swiss": "a modernist poster",
    "tiling": "a tiling window manager", "workbench": "Amiga, with the Boing ball",
}
cells = []
for n in names:
    src = open(os.path.join(themes, n + ".xml"), encoding="utf-8").read()
    m = re.search(r'theme-about="([^"]*)"', src)
    about = html.unescape(m.group(1)) if m else n
    head, _, rest = about.partition(":")
    desc = rest.strip()
    # the caption: the title, unless it only repeats the name; then the
    # description's first phrase
    title = head.strip()
    first = desc.split(",")[0].split("(")[0].strip()
    if title.lower().replace(" ", "") in (n, n + "s") or len(title) < 12:
        title = first
    title = title[0].upper() + title[1:]
    alt = (desc.split("(")[0].strip().rstrip(".") or title)[:180]
    title = CAPTION.get(n, title)
    pretty = {"fsn": "FSN", "hud": "HUD", "crt": "CRT", "beos": "BeOS", "progman": "Program Manager"}.get(n, n.capitalize())
    cells.append((f"![{pretty}: {alt}](docs/screenshots/{n}.jpg)", f"**{pretty}** · {title}"))
rows = ["| | |", "|---|---|"]
for i in range(0, len(cells), 2):
    pair = cells[i:i + 2] + [("", "")] * (2 - len(cells[i:i + 2]))
    rows.append(f"| {pair[0][0]} | {pair[1][0]} |")
    rows.append(f"| {pair[0][1]} | {pair[1][1]} |")
table = "\n".join(rows)
readme = os.path.join(root, "README.md")
text = open(readme, encoding="utf-8").read()
start, end = "<!-- gallery -->", "<!-- /gallery -->"
if start not in text:
    raise SystemExit("README has no <!-- gallery --> marker")
before, rest = text.split(start, 1)
after = rest.split(end, 1)[1]
open(readme, "w", encoding="utf-8").write(before + start + "\n" + table + "\n" + end + after)
print(f"{len(names)} designs in the gallery")
