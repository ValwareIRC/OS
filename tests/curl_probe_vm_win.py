"""Why a download fails inside the OS: the companion's [[sh]] runs curl against Hugging Face; what it printed is
read from what the shell sends the (fake) AI.  python tests\\curl_probe_vm_win.py"""
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.join(vmkit.ROOT, "build", "curl-probe")
os.makedirs(out, exist_ok=True)
url = "https://huggingface.co/LiquidAI/LFM3-1.7B-GGUF/resolve/main/LFM3-1.7B-Q4_K_M.gguf"
replies = [{"when": r"probe the net",
            "reply": "[[sh]]date; curl -sSIL -m 30 " + url + " 2>&1 | grep -iE 'HTTP/|content-length|location|curl' | cut -c1-160; "
                     "echo ---; curl -sS -L -r 0-99 -o /tmp/x " + url + " ; echo exit=$?; ls -la /tmp/x; "
                     "tail -5 /tmp/os-reflex-download.log[[/sh]]"},
           {"when": r"curl", "reply": "Done."}] + ["Mm."] * 4
vm = vmkit.Vm(out, replies, qport=4476, aiport=8920)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 300)
    time.sleep(70)  # the reflex download has been tried by now
    vm.focus_companion()
    vm.say("probe the net", r"agent: step 1", 120)
    time.sleep(40)
finally:
    vm.stop()
    log = open(vm.ailog, encoding="utf-8", errors="replace").read()
    i = log.rfind("What your body did")
    print(log[i:i + 3000] if i >= 0 else log[-3000:])
