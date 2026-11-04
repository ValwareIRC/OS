"""The disk survives the power being pulled mid-write (kernel/fs/ext2.c's journal): init writes files nonstop,
making sure of them in batches (OS_MODE=journal-write); the VM is killed at a random moment, as a power cut; the
next start (OS_MODE=journal-check:N) replays the journal (e2fsck), checks the whole disk read-only, and reads back
every file it was told was made sure of. Three rounds, each cut at a different moment.
  python tests\\journal_crash_win.py [ROUNDS]"""
import os, random, re, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

iso = os.environ.get("ISO", os.path.join(vmkit.ROOT, "build", "dev.iso"))
out = os.path.join(vmkit.ROOT, "build", "journal-crash")
os.makedirs(out, exist_ok=True)
rounds = int(sys.argv[1]) if len(sys.argv) > 1 else 3


def boot(mode, disk, log, until=None, kill_after=None, timeout=600):
    if os.path.exists(log):
        os.remove(log)
    args = [vmkit.QEMU, "-M", "q35", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg", "-cpu",
            "Skylake-Client-noTSX-IBRS", "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-m", "4G", "-smp", "4",
            "-no-reboot", "-display", "none", "-fw_cfg", "name=opt/os/mode,string=" + mode, "-audiodev", "none,id=snd0",
            "-drive", "id=d0,file=" + disk.replace(os.sep, "/") + ",format=raw,if=none,cache=writethrough",
            "-device", "ide-hd,drive=d0,bus=ide.0", "-serial", "file:" + log.replace(os.sep, "/"),
            "-cdrom", iso.replace(os.sep, "/")]
    if os.path.basename(iso) == "dev.iso":
        args += ["-drive", "id=big,file=" + os.path.join(os.path.dirname(iso), "big.iso").replace(os.sep, "/") +
                 ",media=cdrom,if=none,readonly=on", "-device", "ide-cd,drive=big,bus=ide.3"]
    p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "qemu.err"), "w"))
    t0, text = time.time(), ""
    while p.poll() is None and time.time() - t0 < timeout:
        time.sleep(0.2)
        text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
        if until and kill_after and re.search(until, text):
            time.sleep(kill_after)
            p.kill()  # the power, pulled: no sync, no shutdown
            break
    if p.poll() is None:
        p.kill()
    p.wait()
    return open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""


ok = True
disk = vmkit.Vm.new_disk(os.path.join(out, "disk.img"), 4 << 30)
for k in range(rounds):
    cut = random.uniform(1.5, 6.0)
    text = boot("journal-write", disk, os.path.join(out, f"write{k}.log"), until=r"journal-write: synced 50\b",
                kill_after=cut)
    synced = [int(x) for x in re.findall(r"journal-write: synced (\d+)", text)]
    n = max(synced) if synced else 0
    journal = re.search(r"ext2: \S+: journal of \d+ MiB", text)
    print(f"round {k + 1}: power cut {cut:.1f} s after 50 synced; {n} made sure of; "
          f"{journal.group(0) if journal else 'NO JOURNAL'}", flush=True)
    text = boot(f"journal-check:{n}", disk, os.path.join(out, f"check{k}.log"))
    res = re.search(r"journal-check: (\d+) files made sure of: (\d+) missing, (\d+) different; the disk (.*)", text)
    replayed = "recovering journal" in text
    print("  " + (res.group(0) if res else "no result (see build/journal-crash/check%d.log)" % k)
          + ("; the journal was replayed" if replayed else ""), flush=True)
    good = bool(res) and res.group(2) == "0" and res.group(3) == "0" and "checks clean" in res.group(4) and journal
    ok &= bool(good)
    print(("PASS" if good else "FAIL") + f" round {k + 1}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
