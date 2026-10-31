#!/bin/sh
# Run on the desktop as the person (fw_cfg opt/os/autorun.sh, tests/companion_vm_win.py with BUILTIN=1):
# with no network at all, Prism's sandbox asks the model that comes with the
# system for a tool call through the companion API, and reads the look.
sleep 20
flatpak run --command=python3 com.valware.prism - <<'EOF' 2>&1 | sed 's/^/APPCHECK /'
import json, os, socket, time

SOCK = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/run/user/1000"), "app", "com.valware.prism", "os-companion.sock")


def request(method, path, body=None, timeout=900):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(timeout)
    s.connect(SOCK)
    data = json.dumps(body).encode() if body is not None else b""
    s.sendall(f"{method} {path} HTTP/1.1\r\nHost: os\r\nConnection: close\r\nContent-Length: {len(data)}\r\n\r\n".encode() + data)
    raw = b""
    while True:
        got = s.recv(65536)
        if not got:
            break
        raw += got
    s.close()
    head, _, rest = raw.partition(b"\r\n\r\n")
    return int(head.split(b" ")[1]) if head.startswith(b"HTTP/1.1") else 0, rest.decode("utf-8", "replace")


def check(name, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + name + (": " + detail[:600] if detail else ""), flush=True)


try:
    st, b = request("GET", "/v1/models")
    check("the built-in mind", st == 200 and json.loads(b)["data"][0].get("tools") is True, b)
    st, b = request("GET", "/look")
    lk = json.loads(b) if st == 200 else {}
    check("look", st == 200 and lk.get("colors", {}).get("accent", "").startswith("#") and "fonts" in lk, b)
    tools = [{"type": "function", "function": {"name": "read_file", "description": "Reads a file in the project",
                                               "parameters": {"type": "object", "properties": {"path": {"type": "string"}},
                                                              "required": ["path"]}}}]
    t0 = time.time()
    st, b = request("POST", "/v1/chat/completions", {
        "model": "os", "tools": tools, "max_tokens": 400,
        "messages": [{"role": "system", "content": "You are a coding agent. Use tools to look at files. /no_think"},
                     {"role": "user", "content": "What is the first line of README.md? Read it."}]})
    took = time.time() - t0
    msg = json.loads(b)["choices"][0]["message"] if st == 200 else {}
    calls = msg.get("tool_calls") or []
    check("the model calls a tool", any(c.get("function", {}).get("name") == "read_file" for c in calls),
          f"{st} after {took:.0f} s: {b[:500]}")
except Exception as e:
    check("the run", False, repr(e))
print("done", flush=True)
EOF
