#!/usr/bin/env python3
"""A real computer's own drive must never be written: boots with a disk that
looks like someone's (an MBR partition table and data at its start), lets the
desktop come up and save, and checks the disk is byte for byte the same.
  python3 tests/foreign_disk_check.py OUT_DIR"""
import hashlib, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
disk = os.path.join(out, "foreign.img")
with open(disk, "wb") as f:
    f.truncate(2 << 30)
    f.seek(0)
    mbr = bytearray(512)
    mbr[446:462] = bytes([0x80, 0, 2, 0, 0x07, 0xfe, 0xff, 0xff, 0x00, 0x08, 0, 0, 0x00, 0xf8, 0x3f, 0])  # NTFS
    mbr[510:512] = b"\x55\xaa"
    f.write(mbr)
    f.seek(1 << 20)
    f.write(b"\xebR\x90NTFS    " + os.urandom(4096))
    f.seek(400 << 20)
    f.write(os.urandom(1 << 20))


def digest():
    h = hashlib.sha256()
    with open(disk, "rb") as f:
        for off in (0, 1 << 20, 320 << 20, 400 << 20):
            f.seek(off)
            h.update(f.read(2 << 20))
    return h.hexdigest()


before = digest()
os.environ["OS_DEMO_DISK"] = disk
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}",
               "-fw_cfg", f"name=opt/os/facet.xml,file={os.path.join(demo.ROOT, 'tests', 'designs', 'xp.xml')}"])
try:
    demo.wait_for(r"shell: screen desktop", 900)
    time.sleep(30)  # long enough for a save
finally:
    q.quit()
log = demo.log_text()
for line in log.splitlines():
    if "storage:" in line or "os-data" in line or "sda" in line or "store" in line:
        print(line[:200])
after = digest()
print("PASS: the disk is untouched" if before == after else "FAIL: the disk was written")
