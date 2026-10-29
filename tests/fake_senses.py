#!/usr/bin/env python3
"""A stand-in for the PC's host senses service (scripts/host-senses.mjs),
and optionally a scripted model, for testing the companion's senses and
hands without real hardware or a real model.

  python3 tests/fake_senses.py [--senses-port 4591] [--mind-port 4590]

The guest reaches WSL's 127.0.0.1 as 10.0.2.2. Point it here with
"senses_url": "http://10.0.2.2:4591" in the mind.json handed to it.

Senses: GET /senses, GET /health, POST /lights, POST /lights/restore, and
POST /scenario (test control: merge JSON into the state, e.g.
{"cpu_temp": 88} or {"add_light": {...}} or {"down": true}).

Mind (OpenAI format, POST /v1/chat/completions): answers by script, keyed
on words in the newest message, and records every request in
FakeWorld.prompts so a test can check what the companion was told.
"""
import json, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class FakeWorld:
    def __init__(self):
        self.lock = threading.Lock()
        self.cpu_temp = 61.0
        self.gpu_temp = 48.0
        self.down = False
        self.missing = []
        self.lights = [
            {"id": "corsair-k70", "name": "K70 RGB PRO", "kind": "keyboard", "vendor": "Corsair", "leds": 123,
             "color": "#ff8800", "colors": None, "controllable": True},
            {"id": "fan-1", "name": "LL120 #1", "kind": "fan", "vendor": "Corsair", "leds": 16, "color": "#00aaff",
             "colors": None, "controllable": True},
            {"id": "fan-2", "name": "LL120 #2", "kind": "fan", "vendor": "Corsair", "leds": 16, "color": "#00aaff",
             "colors": None, "controllable": True},
            {"id": "g502", "name": "G502 HERO", "kind": "mouse", "vendor": "Logitech", "leds": 2, "color": None,
             "colors": None, "controllable": True},
        ]
        self.posts = []     # (path, body) of every POST to /lights*
        self.prompts = []   # (system, messages) of every model request
        self.replies = []   # what the fake mind answered

    def senses(self):
        with self.lock:
            return {
                "time": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "host": {
                    "name": "VALERIE-PC",
                    "cpu": {"model": "AMD Ryzen 7 7800X3D", "load": 23.5,
                            "temp_c": self.cpu_temp, "clock_mhz": 4650},
                    "gpu": [{"name": "NVIDIA GeForce RTX 4080", "load": 31, "temp_c": self.gpu_temp, "fan_pct": 30,
                             "power_w": 92, "vram_used_mb": 3100, "vram_total_mb": 16376}],
                    "memory": {"used_mb": 18650, "total_mb": 32700},
                    "fans": [{"name": "CPU fan", "rpm": 980, "pct": 35}, {"name": "Case fan 1", "rpm": 1210, "pct": 42}],
                },
                "lights": json.loads(json.dumps(self.lights)),
                "missing": list(self.missing),
            }

    def apply_light(self, body):
        target, color, effect = body.get("target", "all"), body.get("color"), body.get("effect")
        with self.lock:
            for l in self.lights:
                if target in ("all", l["id"], l["kind"]):
                    if color:
                        l["color"] = color
                    if effect == "off":
                        l["color"] = "#000000"


WORLD = FakeWorld()


class SensesHandler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, obj):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if WORLD.down:
            return self.send(503, {"error": "down"})
        if self.path == "/senses":
            return self.send(200, WORLD.senses())
        if self.path == "/health":
            return self.send(200, {"ok": True})
        self.send(404, {"error": "no"})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n).decode() if n else "{}"
        try:
            body = json.loads(raw or "{}")
        except ValueError:
            return self.send(400, {"error": "bad json"})
        if self.path == "/scenario":
            with WORLD.lock:
                for k, v in body.items():
                    if k == "add_light":
                        WORLD.lights.append(v)
                    elif k == "remove_light":
                        WORLD.lights = [l for l in WORLD.lights if l["id"] != v]
                    else:
                        setattr(WORLD, k, v)
            return self.send(200, {"ok": True})
        if WORLD.down:
            return self.send(503, {"error": "down"})
        if self.path in ("/lights", "/lights/restore"):
            WORLD.posts.append((self.path, body))
            print("senses: POST", self.path, raw, flush=True)
            if self.path == "/lights":
                WORLD.apply_light(body)
            return self.send(200, {"ok": True})
        self.send(404, {"error": "no"})


def scripted_reply(system, messages):
    """The fake mind: what it says depends on the newest message."""
    last = messages[-1]["content"] if messages else ""
    if "(What you looked up" in last:
        seen = last.split("(What you looked up", 1)[1]
        return "LOOKED " + " ".join(seen.split())[:160]
    if "(The command they let you run" in last:
        return "RAN " + " ".join(last.split("(The command they let you run", 1)[1].split())[:160]
    if "(Not something they said" in last:
        return "Your PC is running warm right now; maybe give it a minute."
    if "(They are seeing the desktop" in last or "(They have just switched" in last:
        return "Welcome home."
    tail = last.strip().splitlines()[-1] if last.strip() else ""
    table = {
        "LIGHTTEST": "Making your keyboard red. [[light:keyboard #ff0000]]",
        "LOOKTEST": "[[look:processes]]",
        "LOOK3TEST": "[[look:hardware]]",
        "RUNTEST": "I can run that once you say yes. [[run:echo hello-from-run]]",
        "SENSESTEST": "Here is everything I can sense. [[open:senses]]",
        "FOLLOWTEST": "[[set:lights-follow on]]",
        "UNFOLLOWTEST": "[[set:lights-follow off]]",
        "QUIETTEST": "I'll keep quiet. [[set:quiet on]]",
        "TEMPTEST": "Checking what I can feel.",
        "FILETEST": "[[look:file /root/notes.txt]]",
    }
    # longest first: UNFOLLOWTEST also contains FOLLOWTEST
    for k in sorted(table, key=len, reverse=True):
        if k in tail:
            return table[k]
    return "Okay."


class MindHandler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        req = json.loads(self.rfile.read(n).decode())
        msgs = req.get("messages", [])
        system = msgs[0]["content"] if msgs and msgs[0]["role"] == "system" else ""
        rest = [m for m in msgs if m["role"] != "system"]
        WORLD.prompts.append((system, rest))
        text = scripted_reply(system, rest)
        WORLD.replies.append(text)
        print("mind: ->", text[:120], flush=True)
        data = json.dumps({"choices": [{"message": {"role": "assistant", "content": text},
                                        "finish_reason": "stop"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        data = json.dumps({"data": [{"id": "fake"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def serve(handler, port):
    srv = ThreadingHTTPServer(("127.0.0.1", port), handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


if __name__ == "__main__":
    sp = int(sys.argv[sys.argv.index("--senses-port") + 1]) if "--senses-port" in sys.argv else 4591
    serve(SensesHandler, sp)
    if "--mind-port" in sys.argv:
        serve(MindHandler, int(sys.argv[sys.argv.index("--mind-port") + 1]))
    print(f"fake senses on 127.0.0.1:{sp}", flush=True)
    while True:
        time.sleep(3600)
