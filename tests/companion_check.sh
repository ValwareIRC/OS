#!/bin/sh
# Run on the desktop as the person (fw_cfg opt/os/autorun.sh, tests/companion_vm_win.py):
# Prism's sandbox uses the companion API (docs/companion-api.md) the way an app
# does, over $XDG_RUNTIME_DIR/app/com.valware.prism/os-companion.sock, and
# says on the console how each part went (APPCHECK lines).
sleep 20
echo "APPCHECK voice: $(ls -d /dev/dsp /usr/bin/piper /usr/bin/whisper-cli /usr/share/os/voice/* 2>&1 | tr '\n' ' ')"
flatpak run --command=python3 com.valware.prism - <<'EOF' 2>&1 | sed 's/^/APPCHECK /'
import json, os, socket, threading, time

SOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/run/user/1000"), "app", "com.valware.prism", "os-companion.sock")
print("socket", "there" if os.path.exists(SOCK) else "missing", SOCK, flush=True)


def request(method, path, body=None, timeout=200):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(timeout)
    s.connect(SOCK)
    data = json.dumps(body).encode() if body is not None else b""
    head = f"{method} {path} HTTP/1.1\r\nHost: os\r\nConnection: close\r\nContent-Length: {len(data)}\r\n"
    if body is not None:
        head += "Content-Type: application/json\r\n"
    s.sendall(head.encode() + b"\r\n" + data)
    raw = b""
    while True:
        try:
            got = s.recv(65536)
        except socket.timeout:
            break
        if not got:
            break
        raw += got
    s.close()
    head, _, rest = raw.partition(b"\r\n\r\n")
    status = int(head.split(b" ")[1]) if head.startswith(b"HTTP/1.1") else 0
    return status, rest.decode("utf-8", "replace")


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + (": " + detail[:300] if detail else ""), flush=True)


def stream(path, into, stop):
    """An event stream, read into a list until stop is set."""
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(0.5)
    s.connect(SOCK)
    s.sendall(f"GET {path} HTTP/1.1\r\nHost: os\r\n\r\n".encode())
    buf, head = b"", True
    while not stop.is_set():
        try:
            got = s.recv(4096)
        except socket.timeout:
            continue
        if not got:
            break
        buf += got
        if head:  # the reply's own head first
            if b"\r\n\r\n" not in buf:
                continue
            buf, head = buf.split(b"\r\n\r\n", 1)[1], False
        while b"\n\n" in buf:
            ev, buf = buf.split(b"\n\n", 1)
            into.append(ev.decode("utf-8", "replace"))
    s.close()


try:
    st, b = request("GET", "/consent")
    check("consent starts unasked", st == 200 and json.loads(b) == {"ai": None, "memory": None}, b)

    t0 = time.time()
    st, b = request("GET", "/v1/models")  # held while the prompt is up
    held = time.time() - t0
    m = json.loads(b) if st == 200 else {}
    check("models after the prompt", st == 200 and m["data"][0]["id"] == "os", f"{st} {b} (held {held:.0f} s)")
    check("the request was held for the prompt", held > 2, f"{held:.1f} s")
    check("tools are on for this mind", st == 200 and m["data"][0].get("tools") is True, b)

    tools = [{"type": "function", "function": {"name": "read_file", "description": "Reads a file",
                                               "parameters": {"type": "object", "properties": {"path": {"type": "string"}}}}}]
    st, b = request("POST", "/v1/chat/completions", {"model": "os", "stream": True, "tools": tools,
                                                     "stream_options": {"include_usage": True},
                                                     "messages": [{"role": "user", "content": "hello from prism"}]})
    check("a streamed reply", st == 200 and "data:" in b and "[DONE]" in b, f"{st} {b[:200]}")

    st, b = request("POST", "/v1/chat/completions", {"model": "os", "messages": [{"role": "user", "content": "again"}]})
    check("a whole reply", st == 200 and json.loads(b)["choices"][0]["message"]["content"], f"{st} {b[:200]}")

    st, b = request("GET", "/identity")
    ident = json.loads(b) if st == 200 else {}
    check("identity", st == 200 and ident.get("name") and ident.get("persona") and "version" in ident, b[:200])

    events, stop = [], threading.Event()
    th = threading.Thread(target=stream, args=("/events", events, stop), daemon=True)
    th.start()
    time.sleep(1.5)
    st, b = request("POST", "/avatar", {"act": "wave"})
    check("avatar", st == 200 and json.loads(b).get("ok"), b)

    st, b = request("POST", "/memory", {"text": "likes green tea in the afternoon", "kind": "note"})
    entry = json.loads(b) if st == 200 else {}
    check("memory added", st == 200 and entry.get("id", "").startswith("m_") and entry.get("source") == "app", b)
    st, b = request("GET", "/memory")
    mem = json.loads(b) if st == 200 else {}
    check("memory lists it", any(e.get("id") == entry.get("id") for e in mem.get("entries", [])), b[:300])
    v = mem.get("version", 0)
    st, b = request("POST", "/memory", {"id": entry.get("id"), "text": "likes jasmine tea in the afternoon"})
    check("memory corrected, same id", st == 200 and json.loads(b).get("id") == entry.get("id")
          and "jasmine" in json.loads(b).get("text", ""), b)
    st, b = request("DELETE", "/memory/" + entry.get("id", "x"))
    check("memory forgotten", st == 200 and json.loads(b).get("deleted") is True, b)
    st, b = request("GET", f"/memory?since={v}")
    ch = json.loads(b) if st == 200 else {}
    check("the change list says forgotten",
          any(e.get("id") == entry.get("id") and e.get("deleted") for e in ch.get("entries", [])), b[:300])

    st, b = request("POST", "/voice/speak", {"text": "Hello from Prism."}, timeout=120)
    check("speak waits for the line", st == 200 and "done" in json.loads(b), b)

    st, b = request("POST", "/voice/listen/claim", {})
    claim = json.loads(b) if st == 200 else {}
    check("microphone claimed", claim.get("granted") is True and len(claim.get("token", "")) == 32, b)
    heard, stop2 = [], threading.Event()
    th2 = threading.Thread(target=stream, args=("/voice/listen?token=" + claim.get("token", ""), heard, stop2), daemon=True)
    th2.start()
    time.sleep(3)
    check("listening stream open", any("event: ready" in e for e in heard), repr(heard[:3]))
    st, b = request("POST", "/voice/listen/release", {})
    check("microphone given back", st == 200, b)
    time.sleep(1)
    stop2.set()

    time.sleep(1)
    stop.set()
    names = [e.split("\n")[0] for e in events]
    check("events: hello first", names[:1] == ["event: hello"], repr(names[:6]))
    check("no echo of its own wave", not any("act" in e and "wave" in e for e in events), repr(events[:6]))
    check("memory events arrive", any(e.startswith("event: memory") for e in events), repr(names))

    st, b = request("GET", "/nothing-here")
    check("unknown path", st == 404, b)
except Exception as e:
    check("the run", False, repr(e))
print("done", flush=True)
EOF
