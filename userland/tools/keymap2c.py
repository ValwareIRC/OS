#!/usr/bin/env python3
"""Turns an xkb keymap file into a C array (with the trailing NUL clients expect).

  keymap2c.py keymap.xkb keymap.inc
"""
import sys

data = open(sys.argv[1], "rb").read() + b"\0"
with open(sys.argv[2], "w") as out:
    out.write("/* xkb keymap, US layout (xkbcli compile-keymap --layout us). Generated; do not edit. */\n")
    out.write("static const unsigned char os_keymap[] = {\n")
    for i in range(0, len(data), 24):
        out.write(",".join(str(b) for b in data[i:i + 24]) + ",\n")
    out.write("};\n")
