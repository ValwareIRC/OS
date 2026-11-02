"""Its voice and who it is, in the OS, with Windows QEMU (Hyper-V) and silent
sound. tests/fake_ai.py plays the companion:
  someone it met before it had a personality: it opens a short chat itself;
  they ask how it sounds: it plays a clip, they choose it, it is downloaded
  (from Kokoro's published voices) and it speaks with it, through os-speak;
  what it reads in them ([[trait:]]) never shows; at 15 its own traits are
  chosen and its character written, each in a context of its own, and its
  next reply is made knowing who it is;
  they ask it to be more cutesy: the wish holds at once, and it is rewritten with it.
  python tests\\voice_persona_vm_win.py [OUTDIR]"""
import json, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "voice-vm"))
os.makedirs(out, exist_ok=True)

character = ("You talk in short, dry sentences and you let silences sit. You love rain on a window, old synth records "
             "and well-named folders; you can't stand the word 'synergy', notification badges or being rushed. " * 3)
replies = [
    {"when": r"know them better", "reply": "I just noticed I know your desktop better than I know you. What's a good "
                                           "Saturday for you? [[trait:Curious]]"},
    {"when": r"how do you sound", "reply": "Have a listen to this one. [[voice:hear bm_george]] [[trait:Witty, Creative]]"},
    {"when": r"that one", "reply": "Then that's me. [[voice:use bm_george]] [[trait:Impatient, Outspoken]]"},
    {"when": r"say something", "reply": "Right. This is how I sound now. Better, I think. [[trait:Passionate, "
                                        "Determined, Restless, Independent]]"},
    {"when": r"tell me more", "reply": "Fair enough. [[trait:Eccentric, Innovative, Stubborn, Sceptical, "
                                       "Affectionate, Perfectionistic]]"},
    {"system": r"You cast characters", "reply": "Calm, Patient, Witty, Curious, Loyal, Sceptical, Stubborn, Organised, "
                                              "Observant, Honest, Playful, Practical, Even-tempered, Outspoken, Modest"},
    {"when": r"asked you to change", "system": r"novelist", "reply": "You are sweeter now, playful. " * 12},
    {"system": r"novelist", "reply": character},
    {"when": r"more cutesy", "reply": "Okay, okay. Softer and sillier it is. [[persona:tune more cutesy and playful]]"},
    {"when": r"still there", "system": r"WHAT THEY ASKED YOU TO BE[\s\S]*more cutesy and playful",
     "reply": "tune-seen."},
    {"when": r"are you there", "system": r"WHO YOU ARE", "reply": "persona-seen. Always."},
]
for _ in range(6):
    replies.append("Mm.")

import vmkit  # the QEMU, QMP and log helpers the VM tests share

vm = vmkit.Vm(out, replies, alive="proactive=0\n")
results = {}
try:
    vm.start()
    results["desktop"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    results["it opens a chat"] = bool(vm.wait_for(r"persona: 1 of 15", 60))
    time.sleep(4)
    vm.shot("1-it-asks.png")
    vm.focus_companion()
    results["clip played"] = bool(vm.say("how do you sound", r"voice: hearing bm_george"))
    results["voice chosen, fetching"] = bool(vm.say("that one", r"voice: fetching bm_george"))
    results["voice arrived"] = bool(vm.wait_for(r"voice: bm_george arrived", 120))
    m = len(vm.log_text())
    vm.say("say something", r"voice: speaking \(bm_george\)")
    said = vm.wait_for(r"voice: said \(bm_george\) in ([\d.]+)s([^\n]*)", 120, m)
    results["spoke with kokoro"] = bool(said) and float(said.group(1)) > 1.0 and not said.group(2)
    if said:
        print("   ", said.group(0))
    vm.say("tell me more", r"persona: 15 of 15 read")
    results["15 read"] = "persona: 15 of 15 read" in vm.log_text()
    results["written"] = bool(vm.wait_for(r"persona: written", 120))
    vm.shot("2-conversation.png")
    vm.say("are you there", r"companion|voice: speaking")
    time.sleep(3)
    results["it knows who it is"] = "persona-seen" in open(vm.ailog).read()
    vm.shot("3-after.png")
    vm.say("be more cutesy", r"persona: they asked it to change")
    results["wish taken"] = "persona: they asked it to change" in vm.log_text()
    results["rewritten with it"] = bool(vm.wait_for(r"persona: rewritten with their wish", 120))
    vm.say("still there", r"voice: speaking")
    time.sleep(3)
    results["it is what they asked"] = "tune-seen" in open(vm.ailog).read()
    vm.shot("4-tuned.png")
finally:
    vm.stop()
    for k, v in results.items():
        print(f"{k}: {v}")
