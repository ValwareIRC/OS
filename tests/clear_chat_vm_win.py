"""Clearing the conversation, as Valerie asked Mita to before taking a video: the companion is asked, answers
with [[chat:clear]], and once that answer has been said the whole conversation is gone, from the window and from
what is kept: the next question goes to the model with nothing before it. (Asked of the companion without the
tag, it had searched the system for a way for five minutes, and there was none.)

  set ISO=build\\dev.iso & python tests\\clear_chat_vm_win.py [OUTDIR]"""
import json, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = vmkit.ROOT
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "clear-chat-vm"))
os.makedirs(out, exist_ok=True)
replies = [
    {"when": r"remember the word", "reply": "Pineapple, got it."},
    {"when": r"tell me a colour", "reply": "Teal."},
    {"when": r"clear our chat", "reply": "All clear. [[chat:clear]]"},
    {"when": r"what did we say", "reply": "Nothing yet, this is a fresh start."},
]
for _ in range(10):
    replies.append("Mm.")
disk = vmkit.Vm.new_disk(os.path.join(out, "disk.img"), 4 << 30)
vm = vmkit.Vm(out, replies, alive="proactive=0\n", disk=disk, qport=4496, aiport=8940, memory=os.environ.get("MEM", "8G"))
results = {}


def ai_log():
    try:
        return [json.loads(l) for l in open(vm.ailog, encoding="utf-8") if l.strip()]
    except OSError:
        return []


try:
    vm.start()
    results["desktop"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    time.sleep(6)
    vm.focus_companion()
    vm.say("remember the word pineapple", r"shell: ai Pineapple")
    vm.say("tell me a colour", r"shell: ai Teal")
    vm.say("clear our chat please", r"shell: ai All clear")
    results["cleared once it was said"] = bool(vm.wait_for(r"companion: the conversation cleared", 60))
    vm.shot("1-cleared.png")
    vm.say("what did we say before", r"shell: ai Nothing yet")
    asked = [e for e in ai_log() if any("what did we say" in t.get("content", "") for t in e["tail"])]
    tail = asked[-1]["tail"] if asked else []
    print("    the request after clearing:", [t.get("content", "")[:60] for t in tail])
    results["the next question has nothing before it"] = bool(asked) and not any(
        re.search(r"pineapple|colour|Teal|clear our chat", t.get("content", ""), re.I) for t in tail)
    vm.shot("2-after.png")
finally:
    vm.stop()
    for k, v in results.items():
        print(f"{k}: {v}")
    sys.exit(0 if results and all(results.values()) else 1)
