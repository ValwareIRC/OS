"""Their home on the disk itself (init's home_to_disk, the journaled apps' area), from a disk made by the system
before it (the home in the store):
  1. the image from before (OLD_ISO, build/os.iso) on a fresh disk: a note saved in their home, then a proper
     shut down (the store keeps it, as it always did);
  2. this image on that disk: the disk is given a journal, the home is moved onto it, the note is still there;
     then a 50 MB file (the store kept nothing over 32 MB) is written and the power pulled right after a sync;
  3. this image again: the journal replayed, the note and the big file there, byte for byte.
Windows QEMU, silent sound, the stand-in AI driving the companion's shell.  python tests\\home_disk_vm_win.py"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.join(vmkit.ROOT, "build", "home-disk")
os.makedirs(out, exist_ok=True)
old_iso = os.environ.get("OLD_ISO", os.path.join(vmkit.ROOT, "build", "os.iso"))
new_iso = os.environ.get("ISO", os.path.join(vmkit.ROOT, "build", "dev.iso"))
disk = vmkit.Vm.new_disk(os.path.join(out, "disk.img"), 12 << 30)
os.environ["DISK_RW"] = "1"
results = {}


def ask(vm, words, cmd, secs=180):
    """The companion runs `cmd` (its [[sh:]] tag); what came back, from the stand-in AI's log."""
    m = len(open(vm.ailog, encoding="utf-8", errors="replace").read())
    vm.focus_companion()
    vm.say(words, r"ai \[\[sh", 60)
    t0 = time.time()
    while time.time() - t0 < secs:
        a = open(vm.ailog, encoding="utf-8", errors="replace").read()[m:]
        got = re.search(r"What your body did.*?->\\n(.*?)\\n\\n", a)
        if got:
            return got.group(1).replace("\\n", "\n")
        time.sleep(1)
    return ""


def start(iso, replies):
    os.environ["ISO"] = iso
    vm = vmkit.Vm(out, replies, memory="8G", disk=disk, qport=4534, aiport=8978)
    vm.start()
    if not vm.wait_for(r"shell: screen desktop", 400):
        print("the desktop didn't come up")
    time.sleep(25)
    return vm


note = "a note from before " + str(int(time.time()))
# 1. the system from before: a note in their home, kept by the store at a proper shut down
replies = [{"when": r"write the note", "reply": f"[[sh: mkdir -p ~/Documents && echo '{note}' > ~/Documents/from-before.txt && cat ~/Documents/from-before.txt]]"},
           {"when": r"from before", "reply": "Saved."}] + ["Okay."] * 8
vm = start(old_iso, replies)
try:
    got = ask(vm, "write the note", "")
    results["the old system saves a note in their home"] = note in got
    m = len(vm.log_text())
    vm.cmd("system_powerdown")  # ACPI: as the power button, the system saves and shuts down
    vm.wait_for(r"store: saved|power", 60, m)
    time.sleep(8)
finally:
    vm.stop()

# 2. this system: a journal, the home moved, the note there; a 50 MB file and the power pulled
check = ("[[sh: cat ~/Documents/from-before.txt; cat /run/os/home-on-disk; "
         "dd if=/dev/urandom of=$HOME/Downloads/big.bin bs=1M count=50 2>/dev/null; sha256sum $HOME/Downloads/big.bin; sync; echo WRITTEN]]")
replies = [{"when": r"check the home", "reply": check}, {"when": r"WRITTEN", "reply": "Done."}] + ["Okay."] * 8
vm = start(new_iso, replies)
sha = ""
try:
    log = vm.log_text()
    results["the disk is given a journal"] = bool(re.search(r"given a journal: ok|journal of \d+ MiB", log))
    results["their home is moved onto the disk"] = "their home moved onto the disk" in log
    got = ask(vm, "check the home", "")
    results["the note from before is in it"] = note in got
    results["the home is on the disk"] = "/var/lib/os-data/home/" in got
    m2 = re.search(r"([0-9a-f]{64})\s+\S*big.bin", got)
    sha = m2.group(1) if m2 else ""
    results["a 50 MB file is written"] = bool(sha) and "WRITTEN" in got
finally:
    if vm.qemu:
        vm.qemu.kill()  # the power, pulled
    vm.stop()

# 3. this system again: replayed, both there
replies = [{"when": r"look again", "reply": "[[sh: cat ~/Documents/from-before.txt; sha256sum $HOME/Downloads/big.bin]]"},
           {"when": r"big.bin", "reply": "Done."}] + ["Okay."] * 8
vm = start(new_iso, replies)
try:
    log = vm.log_text()
    results["after the power cut: the disk checked (the journal replayed)"] = bool(re.search(r"checked /dev/sda2: (clean|repaired)", log))
    got = ask(vm, "look again", "")
    results["after the power cut: the note is there"] = note in got
    results["after the power cut: the 50 MB file is there, byte for byte"] = bool(sha) and sha in got
finally:
    vm.stop()
for l in re.findall(r"init: .*(?:journal|home|checked /dev/sda2).*", vm.log_text()):
    print("  " + l[:160])
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
