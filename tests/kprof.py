"""Names a /proc/kprofile dump (tests/appsize_vm_win.py writes them as kprof-PHASE.txt) with the kernel's symbols:
where the kernel's working time went, by function and by system call.

  nm -n build/kernel.elf > build/kernel.syms      (in WSL, after building)
  python tests\\kprof.py build\\kernel.syms PROFILE [TOP]"""
import bisect, collections, re, sys

syms = []
for line in open(sys.argv[1]):
    parts = line.split()
    if len(parts) == 3 and parts[1].lower() in "tw":
        syms.append((int(parts[0], 16), parts[2]))
syms.sort()
addrs = [a for a, _ in syms]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
text = open(sys.argv[2]).read()
ticks = re.search(r"ticks user (\d+) kernel (\d+) idle (\d+)", text)
by_fn, by_sys = collections.Counter(), collections.Counter()
for addr, n in re.findall(r"^pc ([0-9a-f]+) (\d+)", text, re.M):
    i = bisect.bisect_right(addrs, int(addr, 16)) - 1
    by_fn[syms[i][1] if i >= 0 else "?"] += int(n)
for name, n in re.findall(r"^sys (.+) (\d+)$", text, re.M):
    by_sys[name] += int(n)
if ticks:
    u, k, i = map(int, ticks.groups())
    print(f"ticks: user {u}, kernel {k}, idle {i}  (kernel {100 * k / max(1, u + k):.0f}% of the busy time)")
total = sum(by_fn.values()) or 1
print(f"\nkernel time by function (of the {total} ticks the top 150 places had):")
for fn, n in by_fn.most_common(top):
    print(f"  {100 * n / total:5.1f}%  {fn}")
stotal = sum(by_sys.values()) or 1
print("\nkernel time by what it was doing:")
for name, n in by_sys.most_common(15):
    print(f"  {100 * n / stotal:5.1f}%  {name}")
progs = sorted(((int(k), int(u), name, pid) for pid, name, k, u in
                re.findall(r"^prog (\d+) (\S+) kernel (\d+) user (\d+)", text, re.M)), reverse=True)
print("\nby program (ticks in the kernel, in its own code):")
for k, u, name, pid in progs[:12]:
    print(f"  {name} ({pid}): kernel {k}, own {u}")
for line in re.findall(r"^(?:pgfault|proc) .*$", text, re.M):
    print("  " + line[:160])
