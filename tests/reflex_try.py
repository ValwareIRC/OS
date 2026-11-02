"""The companion's reflexes (userland/shell/reflex.c), tried against a llama-server on 127.0.0.1:8092 the way
reflex.c asks: worked examples, then the event; decision-model style reads the probability of each answer and
decides as reflex.c does; small-LLM style takes the letter it writes. Cases whose right answer is clear.
  python tests/reflex_try.py [jev|llm|both]"""
import json, math, sys, time, urllib.request
sys.stdout.reconfigure(encoding="utf-8")

SYSTEM = ("You judge events on a personal computer for its built-in assistant. You answer each question about the "
          "event with one letter.")
EXAMPLES = [
    ("The person is at the computer.", "they closed \"Remix\" (the person did this)", "B", "their own action"),
    ("The person is at the computer.", "they opened Files (the person did this)", "A", "their own action"),
    ("The person is at the computer.", "the clock passed 15:00 (it happened by itself)", "A", "nothing happened"),
    ("The person is away.", "a backup finished after 2 hours (it happened by itself)", "B", "good to know later"),
    ("The person is at the computer.", "an app finished updating (it happened by itself)", "B", "worth knowing, not now"),
    ("The person is at the computer.", "a calendar reminder: dentist in 15 minutes (it happened by itself)", "C", "they need to know now"),
    ("The person is at the computer.", "a message from Sam: \"are you still coming tonight?\" (it happened by itself)", "C", "someone is waiting for them"),
    ("The person is at the computer.", "the battery is at 3% (it happened by itself)", "D", "work will be lost"),
    ("The person is at the computer.", "the GPU is at 96°C (it happened by itself)", "D", "damage soon"),
    ("The person is typing.", "Wi-Fi reconnected after 20 seconds (it happened by itself)", "A", "already fine"),
    ("The person is at the computer.", "Wi-Fi dropped (it happened by itself)", "C", "they will notice soon"),
    ("The person is at the computer.", "memory is at 85% (it happened by itself)", "B", "not a problem yet"),
    ("The person is at the computer.", "a USB drive was plugged in (it happened by itself)", "B", "worth knowing"),
    ("The person is watching something full screen.", "a system update is ready to install (it happened by itself)", "B", "later"),
]
at_pc = "It is Sunday 21:40. The person is at the computer. "
away = "It is Sunday 21:40. The person is away from the computer for a while. "
typing = "It is Sunday 21:40. The person is typing. "
CASES = [
    (at_pc, "a long task finished: chromium, after 12 minutes (it used 300 s of CPU) (it happened by itself)", "AB"),
    (away, "a long task finished: ffmpeg, after 40 minutes (it used 2200 s of CPU) (it happened by itself)", "BC"),
    (at_pc, "the internet just dropped (it happened by itself)", "BC"),
    (at_pc, "the internet is back (it was down for 1 minute) (it happened by itself)", "AB"),
    (at_pc, "this computer is almost out of memory (180 MB left; chromium is the biggest) (it happened by itself)", "CD"),
    (at_pc, "their PC's CPU is heating up fast: 82°C and rising (it was 61°C two minutes ago) (it happened by itself)", "BC"),
    (typing, "a new Logitech mouse appeared on their PC: G502 (it happened by itself)", "AB"),
    (at_pc, "a download finished: ~/Downloads/report.pdf (2.1 MB) (it happened by itself)", "BC"),
    (at_pc, "the disk is 99% full: 300 MB left (it happened by itself)", "CD"),
    (at_pc, "a notification from Mail: \"Your flight tomorrow is cancelled\" (it happened by itself)", "CD"),
    (typing, "the clock passed 22:00 (it happened by itself)", "A"),
    (at_pc, "they've been at the computer for 3 hours without a real break (it happened by itself)", "BC"),
    (at_pc, "they closed \"Files\" (the person did this)", "AB"),
]
Q = "\nA ignore, B remember, C interrupt now, D urgent?"


def ask(sit, ev, mode):
    msgs = [{"role": "system", "content": SYSTEM}]
    for es, ee, a, why in EXAMPLES:
        msgs += [{"role": "user", "content": f"{es}\nEvent: {ee}{Q}"},
                 {"role": "assistant", "content": a if mode == "jev" else f"{a}: {why}"}]
    msgs.append({"role": "user", "content": f"{sit}\nEvent: {ev}{Q}"})
    body = {"messages": msgs, "temperature": 0, "chat_template_kwargs": {"enable_thinking": False}}
    body.update({"max_tokens": 1, "logprobs": True, "top_logprobs": 20} if mode == "jev" else
                {"max_tokens": 24, "grammar": 'root ::= [ABCD] ": " [^\n]{2,80}'})
    t = time.time()
    r = json.loads(urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:8092/v1/chat/completions",
                                                                 json.dumps(body).encode(), {"Content-Type": "application/json"}), timeout=60).read())
    ms = (time.time() - t) * 1000
    ch = r["choices"][0]
    text = ch["message"].get("content") or ""
    if mode == "llm":
        return (text[:1] if text[:1] in "ABCD" else "?"), 1.0, text, ms
    p = dict.fromkeys("ABCD", 0.0)
    for c in (ch.get("logprobs") or {}).get("content", [{}])[0].get("top_logprobs", []):
        tok = c["token"].strip()
        if tok[:1] in p and (len(tok) == 1 or tok[1] in ": "):
            p[tok[:1]] += math.exp(c["logprob"])
    s = sum(p.values()) or 1
    p = {k: v / s for k, v in p.items()}
    if p["D"] >= 0.65: d, conf = "D", p["D"]
    elif p["C"] + p["D"] >= 0.5: d, conf = "C", p["C"] + p["D"]
    elif p["B"] >= p["A"]: d, conf = "B", p["B"]
    else: d, conf = "A", p["A"]
    return d, conf, " ".join(f"{k}{v:.2f}" for k, v in p.items()), ms


for mode in (["jev", "llm"] if len(sys.argv) < 2 or sys.argv[1] == "both" else [sys.argv[1]]):
    good = 0
    for sit, ev, ok in CASES:
        d, conf, info, ms = ask(sit, ev, mode)
        good += d in ok
        print(f"{'ok ' if d in ok else 'BAD'} {d} {conf:.2f} {ms:5.0f}ms  {ev[:62]:62}  {info[:60]}")
    print(f"== {mode}: {good}/{len(CASES)}")
