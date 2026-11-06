import fs from 'node:fs/promises';
import path from 'node:path';

// Milestones and the boot-log lines that prove each one. Mirrors the
// EXPECT list in tests/boot_test.sh; userland checks are parsed separately.
const MILESTONES = [
  { id: 'core', name: 'Kernel core', checks: [
    ['Boot + CPU tables', 'cpu: gdt, tss and idt loaded'],
    ['Own page tables', 'mm: running on kernel page tables'],
    ['Interrupts', 'selftest: interrupts ok'],
    ['Physical memory', 'selftest: pmm ok'],
    ['Virtual memory', 'selftest: vmm ok'],
    ['Kernel heap', 'selftest: heap ok'],
  ] },
  { id: 'smp', name: 'Cores and scheduling', checks: [
    ['All cores online', 'smp: '],
    ['Scheduler', 'selftest: scheduler ok'],
    ['Preemption', 'selftest: preemption ok'],
  ] },
  { id: 'user', name: 'User space (Linux programs)', checks: [
    ['Kernel boot finished', 'OS BOOT OK'],
    ['PID 1 started', 'init: user space is alive (pid 1)'],
    ['Userland suite passed', 'USERLAND TESTS PASSED'],
  ] },
];
const FIRMWARES = ['uefi', 'bios'];
const MAX_RUNS = 40;

let running = null; // { id, startedAt, lines: [] }
let history = [];   // newest first

const stripAnsi = (s) => s.replace(/\x1b\[[0-9;=?]*[A-Za-z]/g, '').replace(/\r/g, '');

function wslPath(win) {
  const m = /^([A-Za-z]):[\\/](.*)$/.exec(win);
  return m ? `/mnt/${m[1].toLowerCase()}/${m[2].replace(/\\/g, '/')}` : win;
}

function parseLog(text) {
  const log = stripAnsi(text || '');
  const milestones = MILESTONES.map((m) => {
    const checks = m.checks.map(([label, needle]) => ({ label, ok: log.includes(needle) }));
    return { id: m.id, name: m.name, ok: checks.every((c) => c.ok), checks };
  });
  const utests = [];
  for (const line of log.split('\n')) {
    const u = /^utest: (.*?) (ok|FAILED.*)$/.exec(line.trim());
    if (u) utests.push({ name: u[1], ok: u[2] === 'ok', detail: u[2] === 'ok' ? '' : u[2] });
  }
  const panic = (/\*\*\* KERNEL PANIC: .*/.exec(log) || [null])[0];
  const notImpl = [...log.matchAll(/syscall (\d+) not implemented \(pid \d+, ([^)]*)\)/g)].map((x) => `${x[1]} (${x[2]})`);
  const facts = {};
  for (const [k, re] of [['cpus', /smp: (\d+) cpu/], ['memory', /mm: (\d+) MiB usable/], ['tsc', /TSC (\d+) MHz/], ['apic', /apic: (\S+),/]]) {
    const m = re.exec(log); if (m) facts[k] = m[1];
  }
  // Excerpt: from the kernel's first line, without the per-test chatter.
  const start = log.indexOf('Operating System kernel starting');
  const excerpt = (start >= 0 ? log.slice(start) : log).split('\n').filter((l) => l.trim()).slice(-120).join('\n');
  return { milestones, utests, panic, notImpl, facts, excerpt };
}

function summarize(output) {
  const out = stripAnsi(output);
  const result = {};
  for (const fw of FIRMWARES) {
    const pass = new RegExp(`\\[${fw}\\] PASS`).test(out);
    const fail = new RegExp(`\\[${fw}\\] FAIL`).test(out);
    const skip = new RegExp(`\\[${fw}\\] SKIP`).test(out);
    const code = (new RegExp(`\\[${fw}\\] QEMU exit code (\\d+)`).exec(out) || [])[1];
    result[fw] = { status: pass ? 'pass' : fail ? 'fail' : skip ? 'skip' : 'none', exitCode: code ? Number(code) : null,
      timedOut: code === '124' };
  }
  return result;
}

async function dataFile(api, name) {
  await fs.mkdir(path.join(api.dataDir, 'runs'), { recursive: true });
  return path.join(api.dataDir, name);
}

async function saveHistory(api) {
  await fs.writeFile(await dataFile(api, 'history.json'), JSON.stringify(history));
}

async function readBootLog(api, fw) {
  try { return await fs.readFile(path.join(api.root, 'build', `boot-${fw}.log`), 'latin1'); } catch { return ''; }
}

function overall(run) {
  if (run.buildFailed) return 'fail';
  const st = FIRMWARES.map((fw) => run.firmware[fw]?.status);
  if (st.includes('fail')) return 'fail';
  if (st.every((s) => s === 'pass' || s === 'skip') && st.includes('pass')) return 'pass';
  return 'unknown';
}

function brief(run) {
  if (!run) return null;
  const fwBrief = {};
  for (const fw of FIRMWARES) {
    const f = run.firmware[fw] || {};
    fwBrief[fw] = { status: f.status, exitCode: f.exitCode, timedOut: f.timedOut,
      milestones: (f.milestones || []).map((m) => ({ id: m.id, ok: m.ok })),
      utestsPassed: (f.utests || []).filter((u) => u.ok).length, utestsTotal: (f.utests || []).length };
  }
  return { id: run.id, startedAt: run.startedAt, durationMs: run.durationMs, result: run.result, buildFailed: run.buildFailed,
    source: run.source, firmware: fwBrief };
}

async function finishRun(api, output, code, startedAt, source) {
  const id = String(startedAt);
  const firmware = summarize(output);
  const buildFailed = code !== 0 && FIRMWARES.every((fw) => firmware[fw].status === 'none');
  for (const fw of FIRMWARES) {
    if (firmware[fw].status === 'none' || buildFailed) continue;
    const raw = await readBootLog(api, fw);
    Object.assign(firmware[fw], parseLog(raw));
    await fs.writeFile(await dataFile(api, `runs/${id}-${fw}.log`), stripAnsi(raw)).catch(() => {});
  }
  const tail = stripAnsi(output).split('\n').filter((l) => l.trim()).slice(-60).join('\n');
  const run = { id, startedAt, durationMs: Date.now() - startedAt, exitCode: code, buildFailed, firmware,
    outputTail: buildFailed ? tail : '', source };
  run.result = overall(run);
  history.unshift(run);
  history = history.slice(0, MAX_RUNS);
  await saveHistory(api);
  return run;
}

function startRun(api) {
  if (running) return running.done;
  const startedAt = Date.now();
  const lines = [];
  let output = '';
  let resolve;
  const done = new Promise((r) => { resolve = r; });
  running = { id: String(startedAt), startedAt, lines, done, phase: 'Building' };
  api.emit('run', { running: true, startedAt, phase: running.phase, lines: [] });
  const cmd = `cd '${wslPath(api.root)}' && make test`;
  let partial = '';
  const handle = api.spawn('wsl', ['-d', 'Ubuntu', '--', 'bash', '-lc', cmd], {
    onData(chunk) {
      const text = stripAnsi(String(chunk));
      output += text;
      partial += text;
      const parts = partial.split('\n');
      partial = parts.pop();
      for (const raw of parts) {
        const line = raw.trim();
        if (!line) continue;
        if (/boot_test\.sh/.test(line)) running.phase = 'Booting under UEFI';
        if (/^\[uefi\] (PASS|FAIL|SKIP)/.test(line)) running.phase = 'Booting under BIOS';
        // Only the interesting lines: compiler invocations are noise.
        if (/^(cc|ld|gcc|musl-gcc|cp|rm|mkdir|for|cd|xorriso|vendor\/|chmod|make|build\/|\[ -e|OVMF=|\s*-|\t)/.test(line)) continue;
        lines.push(line);
        if (lines.length > 200) lines.shift();
        api.emit('run', { running: true, startedAt, phase: running.phase, lines: lines.slice(-30) });
      }
    },
    async onExit(code) {
      try {
        const run = await finishRun(api, output, code ?? -1, startedAt, 'run');
        running = null;
        api.emit('run', { running: false });
        api.emit('history', history.map(brief));
        api.ui.notify(run.result === 'pass' ? 'Kernel tests passed' : 'Kernel tests failed',
          `UEFI: ${run.firmware.uefi.status}, BIOS: ${run.firmware.bios.status}`);
        resolve(run);
      } catch (e) {
        api.log('finish failed', e);
        running = null;
        api.emit('run', { running: false });
        resolve(null);
      }
    },
  });
  running.handle = handle;
  return done;
}

function describe(run) {
  if (!run) return 'No test runs recorded yet.';
  const when = new Date(run.startedAt).toLocaleString();
  const out = [`Run ${when} (${Math.round(run.durationMs / 1000)} s): ${run.result.toUpperCase()}`];
  if (run.buildFailed) {
    out.push('The build failed before any boot. Last output:', run.outputTail);
    return out.join('\n');
  }
  for (const fw of FIRMWARES) {
    const f = run.firmware[fw];
    out.push(`\n${fw.toUpperCase()}: ${f.status}${f.exitCode != null ? ` (QEMU exit ${f.exitCode}${f.timedOut ? ', timed out' : ''})` : ''}`);
    for (const m of f.milestones || []) {
      const missing = m.checks.filter((c) => !c.ok).map((c) => c.label);
      out.push(`  ${m.ok ? 'ok  ' : 'FAIL'} ${m.name}${missing.length ? ' — missing: ' + missing.join(', ') : ''}`);
    }
    if (f.utests?.length) {
      const bad = f.utests.filter((u) => !u.ok);
      out.push(`  userland checks: ${f.utests.length - bad.length}/${f.utests.length} ok${bad.length ? ' — failed: ' + bad.map((u) => `${u.name} (${u.detail})`).join('; ') : ''}`);
    }
    if (f.panic) out.push(`  ${f.panic}`);
    if (f.notImpl?.length) out.push(`  unimplemented syscalls hit: ${[...new Set(f.notImpl)].join(', ')}`);
  }
  const failed = FIRMWARES.find((fw) => run.firmware[fw].status === 'fail');
  if (failed) out.push(`\nLast lines of the ${failed.toUpperCase()} boot log:\n` + (run.firmware[failed].excerpt || '').split('\n').slice(-25).join('\n'));
  return out.join('\n');
}

export default {
  async start(api) {
    try { history = JSON.parse(await fs.readFile(await dataFile(api, 'history.json'), 'utf8')); } catch { history = []; }
    // First open: adopt the boot logs already on disk as a starting point.
    if (!history.length) {
      const logs = await Promise.all(FIRMWARES.map((fw) => readBootLog(api, fw)));
      if (logs.some(Boolean)) {
        let mtime = Date.now();
        try { mtime = (await fs.stat(path.join(api.root, 'build', 'boot-uefi.log'))).mtimeMs; } catch {}
        const fake = logs.map((l, i) => {
          const fw = FIRMWARES[i];
          if (!l) return `[${fw}] SKIP`;
          const p = parseLog(l);
          return p.milestones.every((m) => m.ok) ? `[${fw}] PASS` : `[${fw}] FAIL`;
        }).join('\n');
        await finishRun(api, fake, 0, Math.round(mtime), 'found on disk');
        history[0].durationMs = 0;
        await saveHistory(api);
      }
    }
  },
  async stop() { running?.handle?.kill(); },
  handlers: {
    async state() {
      return { running: running ? { startedAt: running.startedAt, phase: running.phase, lines: running.lines.slice(-30) } : null,
        history: history.map(brief) };
    },
    async run(args, api) {
      startRun(api);
      return { started: true };
    },
    async details({ id }) {
      const run = id ? history.find((r) => r.id === id) : history[0];
      return run || null;
    },
    async cancel() {
      running?.handle?.kill();
      return { cancelled: !!running };
    },
    async openLog({ id, fw }, api) {
      const p = path.join(api.dataDir, 'runs', `${id}-${fw}.log`);
      try { await fs.access(p); api.ui.openFile(p); return true; } catch { return false; }
    },
  },
  tools: [
    {
      name: 'kernel_test_run',
      description: 'Run `make test` for the OS kernel (builds, then boots under UEFI and BIOS in QEMU) and wait for the result. Takes a few minutes. Returns pass/fail per firmware and milestone, failed userland checks, panics and a boot-log excerpt on failure.',
      parameters: { type: 'object', properties: {} },
      async execute(args, api) {
        const run = await startRun(api);
        return describe(run);
      },
    },
    {
      name: 'kernel_test_latest',
      description: 'The most recent kernel test result (or a given run id): per-firmware pass/fail, milestones, userland checks, panics, unimplemented syscalls, and boot-log lines on failure.',
      parameters: { type: 'object', properties: { id: { type: 'string', description: 'run id from kernel_test_history; omit for the latest' } } },
      async execute({ id } = {}) {
        const run = id ? history.find((r) => r.id === id) : history[0];
        const prefix = running ? `(A run started at ${new Date(running.startedAt).toLocaleTimeString()} is still in progress: ${running.phase}.)\n` : '';
        return prefix + describe(run);
      },
    },
    {
      name: 'kernel_test_history',
      description: 'List recent kernel test runs (newest first) with id, time, duration and UEFI/BIOS results.',
      parameters: { type: 'object', properties: { limit: { type: 'number' } } },
      async execute({ limit = 15 } = {}) {
        if (!history.length) return 'No runs yet.';
        return history.slice(0, limit).map((r) => `${r.id}  ${new Date(r.startedAt).toLocaleString()}  ${r.result.padEnd(7)} UEFI ${r.firmware.uefi?.status}  BIOS ${r.firmware.bios?.status}  ${Math.round(r.durationMs / 1000)}s${r.buildFailed ? '  (build failed)' : ''}`).join('\n');
      },
    },
  ],
};
