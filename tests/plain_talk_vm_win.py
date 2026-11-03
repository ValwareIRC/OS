"""The companion talks plainly (no "the city hums on"): a disk with a character written the old way boots with
the built-in model; the character is rewritten once to talk plainly, and the welcome back is a plain line with no
made-up memory. Prints what it said, for reading. The disk is used in snapshot mode (never written).
Windows QEMU, silent sound.

  python tests\\plain_talk_vm_win.py DISK [OUTDIR]"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

disk = os.path.abspath(sys.argv[1])
out = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(vmkit.ROOT, "build", "plain-talk-vm"))
os.makedirs(out, exist_ok=True)
os.environ.pop("DISK_RW", None)
vm = vmkit.Vm(out, ["Okay."] * 5, memory=os.environ.get("MEM", "20G"), disk=disk, facet=False,
              mind={"provider": "builtin", "base_url": "", "api_key": "", "model": "os"})
said = []
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 300)
    vm.wait_for(r"mind: (awake|never woke)", 300)
    hi = vm.wait_for(r"shell: ai (.+)", 400)
    vm.wait_for(r"persona: (rewritten to talk plainly|couldn't be rewritten)", 1500)
    time.sleep(3)
    vm.shot("1.png")
    # how the rewritten one talks
    for q in ["hey, how are you doing?", "what do you think of my desktop?"]:
        vm.focus_companion()
        vm.say(q, r"shell: ai .+", 600)
    vm.shot("2.png")
finally:
    vm.stop()
log = open(os.path.join(out, "serial.log"), errors="replace").read()
for l in re.findall(r"shell: (?:ai .*|persona: .*)", log):
    print(l[:600])
