"""An expressive voice for Operating System, on this PC's graphics card: Chatterbox (Resemble AI, MIT), which speaks
with feeling (its "exaggeration": how strongly) and in any voice from a few seconds of it (cloning).

This is the stand-in for the day the OS drives the card itself (docs/gpu.md): it runs on Windows, beside the VM, and
the OS reaches it at 10.0.2.2:4572 (QEMU's way to this PC). It listens only on this PC (127.0.0.1).

  GET  /status                      {"ready": true, "model": "chatterbox", "voices": [...], "device": "cuda"}
  POST /speak  {"text", "voice", "exaggeration", "cfg", "temperature"}
               -> 24 kHz, 16-bit, mono PCM, one sentence after another as each is made (chunked)
  POST /voice?name=NAME   a recording (WAV, any rate) -> kept as the voice NAME (8 s or more is best)
  POST /forget?name=NAME  that voice gone

Voices live in build/voice-gpu/voices/NAME.wav. "default" is Chatterbox's own voice.

  python scripts/voice-gpu-server.py [--exit-with-vm]   (from the voice-gpu venv: the launcher's "Expressive voice"
  box starts it; --exit-with-vm: gone a minute after the last VM closes, so the card's memory is given back)"""
import io, json, os, re, sys, threading, time, wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOME = os.path.join(ROOT, "build", "voice-gpu")
VOICES = os.path.join(HOME, "voices")
os.makedirs(VOICES, exist_ok=True)
os.environ.setdefault("HF_HOME", os.path.join(HOME, "hf"))  # the model's files stay with the rest of it
PORT = int(os.environ.get("OS_VOICE_PORT", "4572"))
LOG = open(os.path.join(HOME, "server.log"), "a", buffering=1, encoding="utf-8")


def log(*a):
    line = time.strftime("%H:%M:%S ") + " ".join(str(x) for x in a)
    print(line, flush=True)
    LOG.write(line + "\n")


import torch  # noqa: E402

device = "cuda" if torch.cuda.is_available() else "cpu"
log("loading Chatterbox on", device, torch.cuda.get_device_name(0) if device == "cuda" else "")
from chatterbox.tts import ChatterboxTTS  # noqa: E402

t0 = time.time()
model = ChatterboxTTS.from_pretrained(device=device)
SR = model.sr
log(f"ready in {time.time() - t0:.1f}s, {SR} Hz")
lock = threading.Lock()  # one line at a time on the card
NAME_OK = re.compile(r"^[A-Za-z0-9_-]{1,40}$")


def voices():
    return sorted(f[:-4] for f in os.listdir(VOICES) if f.endswith(".wav"))


def sentences(text):
    """One at a time, so the first is heard while the rest are made; very long ones split at commas."""
    parts = [p.strip() for p in re.split(r"(?<=[.!?。！？])\s+", text.strip()) if p.strip()]
    out = []
    for p in parts:
        while len(p) > 280:
            cut = p.rfind(",", 0, 280)
            cut = cut if cut > 80 else 280
            out.append(p[: cut + 1].strip())
            p = p[cut + 1:].strip()
        if p:
            out.append(p)
    return out


def pcm16(wav):
    w = wav.squeeze().detach().float().cpu().clamp(-1, 1)
    return (w * 32767).to(torch.int16).numpy().tobytes()


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, code, body, kind="application/json"):
        b = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def body(self):
        n = int(self.headers.get("Content-Length", 0))
        if n > 40 << 20:
            raise ValueError("too big")
        return self.rfile.read(n)

    def do_GET(self):
        if urlparse(self.path).path == "/status":
            return self.reply(200, {"ready": True, "model": "chatterbox", "device": device, "voices": voices(),
                                    "rate": SR})
        self.reply(404, {"error": "no such thing"})

    def do_POST(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        try:
            if u.path == "/speak":
                return self.speak(json.loads(self.body() or b"{}"))
            if u.path == "/voice":
                return self.keep(q.get("name", [""])[0], self.body())
            if u.path == "/forget":
                name = q.get("name", [""])[0]
                p = os.path.join(VOICES, name + ".wav")
                if NAME_OK.match(name) and os.path.exists(p):
                    os.remove(p)
                    log("forgot", name)
                    return self.reply(200, {"ok": True})
                return self.reply(404, {"error": "no voice called " + name})
        except Exception as e:  # what went wrong goes back, in words
            log("error:", repr(e))
            try:
                self.reply(500, {"error": str(e)})
            except Exception:
                pass
            return
        self.reply(404, {"error": "no such thing"})

    def keep(self, name, data):
        if not NAME_OK.match(name):
            return self.reply(400, {"error": "a voice's name is letters, digits, - or _"})
        try:
            with wave.open(io.BytesIO(data)) as w:
                seconds = w.getnframes() / w.getframerate()
        except Exception:
            return self.reply(400, {"error": "that isn't a WAV recording"})
        if seconds < 3:
            return self.reply(400, {"error": f"only {seconds:.1f} s: it needs at least 3 (8 or more is best)"})
        open(os.path.join(VOICES, name + ".wav"), "wb").write(data)
        log(f"kept the voice {name} ({seconds:.1f} s)")
        self.reply(200, {"ok": True, "seconds": round(seconds, 1)})

    def speak(self, req):
        text = str(req.get("text", ""))[:4000]
        voice = str(req.get("voice", "default"))
        ref = os.path.join(VOICES, voice + ".wav") if NAME_OK.match(voice) else ""
        if voice != "default" and not os.path.exists(ref):
            return self.reply(404, {"error": "no voice called " + voice})
        ex = min(2.0, max(0.25, float(req.get("exaggeration", 0.5))))
        cfg = min(1.0, max(0.0, float(req.get("cfg", 0.5))))
        temp = min(1.5, max(0.3, float(req.get("temperature", 0.8))))
        self.send_response(200)
        self.send_header("Content-Type", "audio/L16; rate=%d; channels=1" % SR)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        t0 = time.time()
        first = None
        for s in sentences(text):
            with lock:
                wav = model.generate(s, audio_prompt_path=ref if voice != "default" else None,
                                     exaggeration=ex, cfg_weight=cfg, temperature=temp)
            b = pcm16(wav)
            if first is None:
                first = time.time() - t0
            self.wfile.write(b"%x\r\n" % len(b) + b + b"\r\n")
            self.wfile.flush()
        self.wfile.write(b"0\r\n\r\n")
        log(f"said {len(text)} chars as {voice} (exaggeration {ex}, cfg {cfg}): first sound after {first or 0:.2f}s, "
            f"all in {time.time() - t0:.2f}s")


def watch_vm():
    import subprocess
    seen, gone_since = False, None
    while True:
        time.sleep(15)
        try:
            out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq qemu-system-x86_64.exe", "/NH"], capture_output=True,
                                 text=True, timeout=10).stdout
        except Exception:
            continue
        running = "qemu-system-x86_64" in out
        seen |= running
        if seen and not running:
            gone_since = gone_since or time.time()
            if time.time() - gone_since > 60:
                log("the VM has closed: stopping")
                os._exit(0)
        else:
            gone_since = None


if "--exit-with-vm" in sys.argv:
    threading.Thread(target=watch_vm, daemon=True).start()
log(f"listening on 127.0.0.1:{PORT}")
ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
