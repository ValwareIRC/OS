"""USB and NVMe, as on a real computer: a USB keyboard, a USB tablet, a hub with a mouse behind it, a USB stick
and an NVMe disk that belong to someone else (each starts with a partition table, as a Windows disk does).
  - every device is found and taken by its driver (the log says so);
  - the keys typed on the USB keyboard reach the companion, and a click of the USB tablet lands where it should;
  - the stick and the NVMe disk are left read-only, and after the whole run they are byte for byte as they were.

  set ISO=build\\dev.iso & python tests\\usb_vm_win.py [OUTDIR]"""
import hashlib, json, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = vmkit.ROOT
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "usb-vm"))
os.makedirs(out, exist_ok=True)


def foreign_disk(path, size):
    """A disk with someone's partition table on it (an MBR's signature and a GPT header), the rest zeros."""
    vmkit.Vm.new_disk(path, size)
    with open(path, "r+b") as f:
        mbr = bytearray(512)
        mbr[446:462] = bytes([0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF])
        mbr[510], mbr[511] = 0x55, 0xAA
        f.write(mbr)
        f.write(b"EFI PART" + bytes(504))
    return path


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


stick = foreign_disk(os.path.join(out, "stick.img"), 64 << 20)
nvme = foreign_disk(os.path.join(out, "nvme.img"), 256 << 20)
before = {stick: digest(stick), nvme: digest(nvme)}
extra = ["-device", "qemu-xhci,id=xhci",
         "-device", "usb-kbd,id=ukbd,bus=xhci.0",
         "-device", "usb-tablet,id=utab,bus=xhci.0",
         "-device", "usb-hub,id=hub,bus=xhci.0,port=3",
         "-device", "usb-mouse,id=umouse,bus=xhci.0,port=3.1",
         "-drive", "id=stick,file=" + stick.replace(os.sep, "/") + ",format=raw,if=none",
         "-device", "usb-storage,drive=stick,bus=xhci.0",
         "-drive", "id=nv0,file=" + nvme.replace(os.sep, "/") + ",format=raw,if=none",
         "-device", "nvme,drive=nv0,serial=OSTEST1"]
replies = [{"when": r"typed on usb", "reply": "Got it."}, {"when": r"clicked with usb", "reply": "Yes."}]
for _ in range(10):
    replies.append("Mm.")
disk = vmkit.Vm.new_disk(os.path.join(out, "disk.img"), 4 << 30)
vm = vmkit.Vm(out, replies, alive="proactive=0\n", disk=disk, qport=4497, aiport=8941,
              memory=os.environ.get("MEM", "8G"), extra=extra)
results = {}


def usb_keys(text):
    names = {" ": "spc", "\n": "ret"}
    for ch in text:
        q = names.get(ch, ch)
        for down in (True, False):
            vm.cmd("input-send-event", events=[{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": q}}}])
            time.sleep(0.03)
        time.sleep(0.03)


def usb_click(x, y):
    vm.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (vmkit.W - 1))}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (vmkit.H - 1))}}])
    time.sleep(0.3)
    for down in (True, False):
        vm.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
        time.sleep(0.1)


try:
    vm.start()
    results["desktop"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    log = vm.log_text()
    for name, pat in [("the USB controller", r"xhci0: .* ports"),
                      ("the USB keyboard", r"usb: .*: keyboard"),
                      ("the USB tablet", r"usb: .*Tablet.*: pointer|usb: .*: pointer"),
                      ("the hub", r"is a hub with \d+ ports"),
                      ("the mouse behind the hub", r"usb: .*Mouse.*behind a hub"),
                      ("the USB stick", r"usb: .*a drive of 64 MiB"),
                      ("the stick left read-only", r"storage: sd.*not this system's: left untouched, read-only"),
                      ("the NVMe disk", r"nvme0: .*256 MiB"),
                      ("the NVMe disk left read-only", r"storage: sd. \(.*\) is not this system's disk")]:
        results[name] = bool(re.search(pat, log))
    print("\n".join(l for l in log.splitlines() if re.search(r"xhci|usb:|nvme|storage:", l)), flush=True)
    time.sleep(6)
    vm.focus_companion()
    m = len(vm.log_text())
    usb_keys("typed on usb keyboard\n")
    results["the keys reach the companion"] = bool(vm.wait_for(r"shell: ai Got it", 90, m))
    results["...through the USB keyboard"] = bool(re.search(r"usb: .*Keyboard: its first input came", vm.log_text()))
    vm.shot("1-keyboard.png")
    r = re.findall(r"rect companion-input (\d+) (\d+) (\d+) (\d+)", vm.log_text())
    if r:
        x, y, w, h = map(int, r[-1])
        usb_click(x + w // 2, y + h // 2)
        time.sleep(1)
        m = len(vm.log_text())
        usb_keys("clicked with usb tablet\n")
        results["a click lands where it points"] = bool(vm.wait_for(r"shell: ai Yes", 90, m))
        results["...through the USB tablet"] = bool(re.search(r"usb: .*Tablet: its first input came", vm.log_text()))
    vm.shot("2-tablet.png")
except Exception:
    import traceback
    traceback.print_exc()
    results["the test ran to its end"] = False
finally:
    vm.stop()
    time.sleep(2)
    results["the stick is byte for byte as it was"] = digest(stick) == before[stick]
    results["the NVMe disk is byte for byte as it was"] = digest(nvme) == before[nvme]
    for k, v in results.items():
        print(f"{k}: {v}")
    sys.exit(0 if results and all(results.values()) else 1)
