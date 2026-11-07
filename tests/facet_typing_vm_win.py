"""Typing into a Facet app. The companion opens Todo (its permission question answered), and its input (autofocus)
is typed into:
- issue 23: the desktop gave the keyboard only to its own windows, so a Facet app's fields never got a key;
- issue 25: the companion's own "get to know you" chat came up over the app just opened and took the keyboard;
- issue 26: Todo empties its field after adding (set draft = ""), which a field being typed in never showed.
Todo saves to ~/Todo.txt; a script running as the person reports the file.
  set ISO=build\\dev.iso & python tests\\facet_typing_vm_win.py [OUTDIR]"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(__file__))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "facet-typing-vm"))
os.makedirs(out, exist_ok=True)
check = os.path.join(out, "autorun.sh")
open(check, "w", newline="\n").write(r'''(
f="$HOME/Todo.txt"
i=0; while [ $i -lt 300 ] && [ ! -s "$f" ]; do sleep 1; i=$((i+1)); done
echo "typingtest: first: $(head -1 "$f")"
i=0; while [ $i -lt 300 ] && [ "$(wc -l < "$f")" -lt 1 ]; do sleep 1; i=$((i+1)); done
echo "typingtest: second: $(tail -1 "$f")"
echo typingtest: done
) &
''')
os.environ["AUTORUN"] = check
vm = vmkit.Vm(out, ["Hello again.", "Here it is. [[open:Todo]]", "Still here."], memory="8G")
results = {}
try:
    vm.start()
    results["desktop"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    time.sleep(8)
    vm.focus_companion()
    vm.say("open todo", r"rect appgate-allow \d+ \d+ \d+ \d+")  # it asks first (appgate.c): allowed
    r = re.findall(r"rect appgate-allow (\d+) (\d+) (\d+) (\d+)", vm.log_text())
    if r:
        x, y, w, h = map(int, r[-1])
        vm.click(x + w // 2, y + h // 2)
    results["todo opened"] = bool(vm.wait_for(r"facet: opened [^\n]*Todo", 30))
    time.sleep(20)  # past when the companion would start a chat of its own: it must not cover Todo (issue 25)
    vm.type("buy milk\n")  # into Todo's input (autofocus): the app just opened has the keyboard
    hit = vm.wait_for(r"typingtest: first: [^\n]*", 60)
    print("  saw:", hit.group(0) if hit else "nothing")
    results["typed into the app"] = bool(hit) and hit.group(0).rstrip().endswith("buy milk")
    vm.shot("1-todo.png")
    time.sleep(2)
    vm.type("eggs\n")  # the field was emptied by Todo: this is a line of its own, not "buy milkeggs"
    hit = vm.wait_for(r"typingtest: second: [^\n]*", 60)
    print("  saw:", hit.group(0) if hit else "nothing")
    results["the app emptied its field"] = bool(hit) and hit.group(0).rstrip().endswith(" eggs") and "milk" not in hit.group(0)
    vm.shot("2-todo.png")
finally:
    vm.stop()
for k, v in results.items():
    print(f"{k}: {v}")
