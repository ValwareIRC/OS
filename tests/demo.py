#!/usr/bin/env python3
"""Drives a full the OS first boot in QEMU and saves screenshots.

The guest's mind is the host's Ollama (reached through tests/host-relay.sh).
A scripted person ("Sam") talks to it, clicks the visual picks, waits for
the AI to design the desktop, opens the start menu and an app, and asks for
a redesign. Coordinates come from the shell's own log on the serial port.

Usage (inside WSL): python3 tests/demo.py [out_dir]
Each run boots with a fresh, empty disk; OS_DEMO_DISK=path keeps one between runs.
"""
import json, os, re, socket, subprocess, sys, time, zlib, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "demo")
QMP = os.environ.get("OS_QMP", "/tmp/os-demo-qmp.sock")
LOG = os.path.join(OUT, "serial.log")
W, H = 1600, 900

ANSWERS = [
    "Hi! I'm Sam.",
    "Why don't you choose a name for yourself? I'd love to hear what you pick.",
    "I'm a sound designer. Most days I edit audio for films, walk by the sea at lunch, and forget to eat.",
    "Help me keep my projects in order, and nudge me to take breaks.",
    "Somewhere calm and warm, like late light through a window.",
    "That sounds right.",
    "Yes, please.",
    "I think you know me well enough now.",
]


def make_disk(path, size=4 << 30):
    """Creates an empty sparse disk image (NTFS under /mnt needs to be told first)."""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    open(path, "wb").close()
    if path.startswith("/mnt/"):
        win = subprocess.run(["wslpath", "-w", path], capture_output=True, text=True).stdout.strip()
        subprocess.run(["fsutil.exe", "sparse", "setflag", win], capture_output=True)
    os.truncate(path, size)


def disk_args():
    """Every run gets a fresh, empty disk (/dev/sda), so it starts as a first
    boot. OS_DEMO_DISK=path keeps that image instead (created if missing),
    so a later run boots as the same person; OS_DEMO_DISK=none attaches no disk."""
    keep = os.environ.get("OS_DEMO_DISK")
    if keep == "none":
        return []
    path = os.path.abspath(keep) if keep else os.path.join(OUT, "disk.img")
    if not keep and os.path.exists(path):
        os.unlink(path)  # a running QEMU keeps its own open copy
    if not os.path.exists(path):
        make_disk(path)
    return ["-drive", f"id=disk0,file={path.replace(',', ',,')},format=raw,if=none",
            "-device", "ide-hd,drive=disk0,bus=ide.0"]


class Qemu:
    def __init__(self, extra=()):
        os.makedirs(OUT, exist_ok=True)
        if os.path.exists(QMP):
            os.unlink(QMP)
        # The host's Ollama for the guest at 10.0.2.2:11434, only when asked (OS_OLLAMA=1):
        # without an Ollama on Windows the bridge loops back into itself (WSL
        # forwards Windows' localhost to its own) and buries WSL in relays.
        if os.environ.get("OS_OLLAMA"):
            subprocess.run(["bash", os.path.join(ROOT, "scripts", "host-bridge.sh")], capture_output=True)
        # OS_NO_KVM=1: emulated (slow), for when WSL's KVM is unavailable
        use_kvm = os.access("/dev/kvm", os.W_OK) and not os.environ.get("OS_NO_KVM")
        kvm = ["-enable-kvm", "-cpu", "host"] if use_kvm else ["-cpu", "max"]
        self.proc = subprocess.Popen(
            ["qemu-system-x86_64", "-M", "q35,vmport=on", "-m", os.environ.get("OS_MEM", "6G"), "-smp", "4", *kvm,
             "-display", "none", "-vga", "std",
             *(["-nic", "none"] if os.environ.get("OS_DEMO_NONET") else ["-netdev", "user,id=n0", "-device", "e1000,netdev=n0"]),
             *disk_args(),
             "-audiodev", f"wav,id=snd0,path={QMP}.wav",
             "-device", "intel-hda", "-device", "hda-duplex,audiodev=snd0",
             "-serial", f"file:{LOG}", "-qmp", f"unix:{QMP},server,nowait",
             "-cdrom", os.path.join(ROOT, "build", "os.iso"), *extra],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            if os.path.exists(QMP):
                break
            time.sleep(0.1)
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(QMP)
        self.f = self.sock.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            reply = json.loads(self.f.readline())
            if "return" in reply or "error" in reply:
                return reply

    def screenshot(self, name):
        self.screenshot_to(os.path.join(OUT, f"{name}.png"))

    def screenshot_to(self, png):
        ppm = QMP + ".ppm"
        self.cmd("screendump", filename=ppm)
        time.sleep(0.3)
        ppm_to_png(ppm, png)
        print("screenshot", png, flush=True)

    def move(self, x, y):
        self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}}])

    def click(self, x, y):
        self.move(x, y)
        time.sleep(0.15)
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.1)
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(0.3)

    def key(self, qcode, shift=False):
        evs = []
        if shift:
            evs.append({"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "shift"}}})
        evs.append({"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": qcode}}})
        self.cmd("input-send-event", events=evs)
        time.sleep(0.05)  # the emulated PS/2 controller buffers only 16 bytes
        evs = [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}}]
        if shift:
            evs.append({"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "shift"}}})
        self.cmd("input-send-event", events=evs)
        time.sleep(0.05)

    def type(self, text):
        special = {" ": ("spc", False), ".": ("dot", False), ",": ("comma", False), "'": ("apostrophe", False),
                   "!": ("1", True), "?": ("slash", True), "-": ("minus", False), ":": ("semicolon", True),
                   ";": ("semicolon", False), "$": ("4", True), "(": ("9", True), ")": ("0", True),
                   "*": ("8", True), "/": ("slash", False), "=": ("equal", False), "|": ("backslash", True),
                   ">": ("dot", True), "<": ("comma", True), "_": ("minus", True), '"': ("apostrophe", True),
                   "~": ("grave_accent", True), "&": ("7", True), "+": ("equal", True), "[": ("bracket_left", False),
                   "]": ("bracket_right", False), "{": ("bracket_left", True), "}": ("bracket_right", True),
                   "^": ("6", True), "\\": ("backslash", False), "%": ("5", True), "#": ("3", True), "@": ("2", True)}
        for ch in text:
            if ch in special:
                self.key(*special[ch])
            elif ch.isalpha():
                self.key(ch.lower(), ch.isupper())
            elif ch.isdigit():
                self.key(ch)
        self.key("ret")

    def quit(self):
        try:
            self.cmd("quit")
        except Exception:
            pass
        self.proc.wait(timeout=10)


PERSONA = ("You are Sam, 34, a film sound designer who lives in a small flat by the sea. You work alone most "
           "days, editing audio for films with headphones on for hours; you're at your best alone with a "
           "difficult mix, but you miss your sister who moved abroad. You walk the shore at lunch, love warm "
           "late-afternoon light, old jazz records, and the hush after rain. Hard days are the ones full of "
           "notes from directors and too many open windows; you lose track of time and forget to eat. The "
           "place you felt most at home was your grandmother's kitchen in Porto, tiled, sunlit, smelling of "
           "coffee. You are switching on a new computer whose AI is getting to know you. Reply as Sam in one "
           "or two short, natural sentences, plain text only, no emojis. If it asks what to call it, ask it "
           "to choose its own name.")
PERSONAS = {
    "sam": PERSONA,
    "noor": ("You are Noor, 29, a night-shift ER nurse who makes ambient electronic music on days off and "
             "loves astronomy. You're at your best in the middle of a busy shift with your team, but you need "
             "total quiet afterwards. Your days are upside down: you sleep until 2pm. Hard days are the ones "
             "when someone doesn't make it, or when your phone won't stop buzzing on a day off. The place you "
             "felt most at home was a rooftop in Amman at 3am, city lights below, stars above, warm wind. You "
             "like deep blues, violets and neon, rain on windows, science fiction films. You are switching on "
             "a new computer whose AI is getting to know you. Reply as Noor in one or two short, natural "
             "sentences, plain text only, no emojis. If it asks what to call it, say you'd like to call it "
             "Vega."),
}
PERSONA = PERSONAS.get(os.environ.get("DEMO_PERSONA", "sam"), PERSONA)
CURL = "/mnt/c/Windows/System32/curl.exe"  # Windows-side curl reaches the host's Ollama


def simulated_reply(ai_text, history):
    """Asks a local model to answer as the demo person."""
    msgs = [{"role": "system", "content": PERSONA}]
    for who, text in history[-8:]:
        msgs.append({"role": "user" if who == "ai" else "assistant", "content": text})
    msgs.append({"role": "user", "content": ai_text})
    body = json.dumps({"model": "qwen3:8b", "messages": msgs, "stream": False, "think": False,
                       "options": {"temperature": 0.7}})
    try:
        out = subprocess.run([CURL, "-s", "-m", "120", "http://127.0.0.1:11434/api/chat", "--data-binary", "@-"],
                             input=body, capture_output=True, text=True, timeout=150).stdout
        text = json.loads(out)["message"]["content"]
    except Exception as e:  # fall back to a neutral answer
        print("simulated reply failed:", e, flush=True)
        text = "That sounds lovely."
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.S).strip()
    # Keep to what the demo can type.
    text = text.replace("’", "'").replace("‘", "'").replace("—", " - ").replace("–", "-")
    text = text.replace("“", '"').replace("”", '"').replace("…", "...")
    text = "".join(ch for ch in text if ch.isalnum() or ch in " .,'!?-:;\"")
    return text[:400] or "Yes."


def ppm_to_png(ppm, png):
    data = open(ppm, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    px = parts[3]
    raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
    chunk = lambda t, b: struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    open(png, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                          + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def log_text():
    # /mnt/c reads now and then fail with ENODATA while QEMU writes: retry.
    for _ in range(20):
        try:
            return open(LOG, "rb").read().decode("utf-8", "replace")
        except FileNotFoundError:
            return ""
        except OSError:
            time.sleep(0.2)
    return ""


def wait_for(pattern, timeout, since=0):
    end = time.time() + timeout
    while time.time() < end:
        text = log_text()
        m = re.search(pattern, text[since:])
        if m:
            return m
        time.sleep(0.5)
    raise TimeoutError(pattern)


def open_companion(q):
    """The companion's window opens from its subtitle (or the orb) when closed."""
    pos = last_rect("companion-input")
    if pos:
        return pos
    for name in ("caption", "ui-orb", "ui-core"):
        p = last_rect(name)
        if p:
            q.click(*p)
            time.sleep(1.5)
            pos = last_rect("companion-input")
            if pos:
                return pos
    return None


def last_rect(name):
    rects = re.findall(r"shell: rect " + re.escape(name) + r" (-?\d+) (-?\d+) (\d+) (\d+)", log_text())
    if not rects:
        return None
    x, y, w, h = map(int, rects[-1])
    return x + w // 2, y + h // 2


def main():
    q = Qemu()
    try:
        wait_for(r"shell: screen (splash|choose)", 90)
        time.sleep(1.5)
        q.screenshot("01-splash")
        wait_for(r"shell: screen choose", 30)
        time.sleep(1)
        q.screenshot("02-choose-a-mind")
        # Ollama card: column "On your network", first card.
        q.click(160 + 2 * 256 + 120, 230 + 46)
        wait_for(r"shell: screen connect", 10)
        time.sleep(0.5)
        q.key("ret")  # Connect with the prefilled address
        time.sleep(6)
        q.screenshot("03-connect-models")
        q.key("ret")  # Begin with the suggested model
        wait_for(r"shell: screen onboard", 15)

        turn = 0
        history = []
        mark = log_text().rfind("shell: screen onboard")  # the first pick comes right after it
        onboard_at = mark
        while turn < 26:
            m = wait_for(r"shell: (ai|ai-error|pick shown) ", 300, mark)
            time.sleep(1.5)
            mark = len(log_text())
            q.screenshot(f"04-onboarding-{turn:02d}")
            text = log_text()
            if "shell: screen designing" in text[onboard_at:]:
                break
            if m.group(1) == "ai-error":
                print("AI error; stopping conversation", flush=True)
                break
            ai_at = text.rfind("shell: ai ")
            last_ai = text[ai_at:] if ai_at >= 0 else text[max(text.rfind("shell: pick shown"), 0):]
            if "[[done]]" in last_ai:
                break
            pick = re.search(r"shell: pick shown (\w+)", last_ai)
            if pick:
                time.sleep(1.2)
                kind = pick.group(1)
                clicked = False
                for _ in range(5):  # several picks in one message come one after another
                    choice = {"palette": 1, "room": 1, "comfort": 1, "tempo": 1}.get(kind, 0)
                    pos = last_rect(f"pick-{kind}-{choice}") or last_rect(f"pick-{kind}-0")
                    if not pos:
                        break
                    q.click(*pos)
                    clicked = True
                    time.sleep(2.5)
                    nxt = re.findall(r"shell: pick shown (\w+)", log_text()[mark:])
                    if not nxt or nxt[-1] == kind:
                        break
                    kind = nxt[-1]
                    time.sleep(1.2)
                    q.screenshot(f"04-onboarding-{turn:02d}b-{kind}")
                    mark = len(log_text())
                if clicked:
                    mark = len(log_text())
                    turn += 1
                    continue
            ai_said = re.sub(r"\[\[.*?\]\]", "", last_ai[len("shell: ai "):].split("\n")[0]).strip()
            reply = simulated_reply(ai_said, history)
            history += [("ai", ai_said), ("sam", reply)]
            print(f"AI:  {ai_said}\nSAM: {reply}", flush=True)
            q.type(reply)
            mark = len(log_text())
            turn += 1

        wait_for(r"shell: screen designing", 60)
        time.sleep(3)
        q.screenshot("05-designing")
        wait_for(r"shell: screen desktop", 400)
        time.sleep(4)
        mark = len(log_text())
        try:
            wait_for(r"shell: (ai|ai-error) ", 120, mark)
        except TimeoutError:
            pass
        time.sleep(1)
        q.screenshot("06-desktop")

        # The AI chose the ways in; the home key opens whichever it gave it to.
        q.key("meta_l")
        time.sleep(1.8)
        q.screenshot("07-way-in")
        files = last_rect("ui-app-files")
        if not files:
            q.key("esc")
            time.sleep(0.6)
            files = last_rect("ui-app-files")
        if files:
            q.click(*files)
        else:
            search = [m for m in re.findall(r"shell: rect (ui-[\w-]+) ", log_text()) if "search" in m or "ask" in m or "bar" in m]
            if search and last_rect(search[-1]):
                q.click(*last_rect(search[-1]))
                q.type("Files")
        time.sleep(1.8)
        q.screenshot("08-files")
        # A small change, made by the companion through the UI itself.
        companion_input = open_companion(q)
        if companion_input:
            q.click(*companion_input)
        mark = len(log_text())
        q.type("Could you add a little clock somewhere and make the corners rounder?")
        try:
            wait_for(r"shell: (ai|ai-error) ", 240, mark)
            time.sleep(2)
        except TimeoutError:
            pass
        q.screenshot("08b-small-change")
        mark = len(log_text())
        # Ask the companion for a redesign.
        companion_input = open_companion(q)
        if companion_input:
            q.click(*companion_input)
        q.type("Could you make my space feel like a warm autumn evening?")
        try:
            wait_for(r"shell: screen designing", 240, mark)
            q.screenshot("09-redesigning")
            wait_for(r"shell: screen desktop", 400, mark + 10)
            time.sleep(5)
            q.screenshot("10-redesigned")
        except TimeoutError:
            q.screenshot("09-after-request")
    finally:
        q.quit()
        print("serial log:", LOG)


if __name__ == "__main__":
    main()
