// Feature test runs: follows build/feature-checks/events.jsonl, written by
// scripts/feature_checks.py as the suite goes, and can start or stop it.
import fs from 'node:fs';
import path from 'node:path';

let api;
let suites = [];        // every suite in the file, oldest first
let offset = 0, partial = '';
let proc = null;        // a suite this gadget started
let timer = null;
let lastNotified = 0;
let loaded = false;

const file = () => path.join(api.root, 'build', 'feature-checks', 'events.jsonl');
const wslPath = p => '/mnt/' + p[0].toLowerCase() + p.slice(2).replace(/\\/g, '/');
const design = name => (/\(([^)]+)\)/.exec(name) || [])[1] || '';

function apply(e) {
  if (e.ev === 'suite') {
    suites.push({ id: e.t, started: e.t, pid: e.pid, ended: null, failed: null, stopped: false, last: e.t,
      runs: e.runs.map(name => ({ name, design: design(name), status: 'waiting', ok: 0, fails: [], checks: [], errors: [], summary: [] })) });
    return;
  }
  const s = suites[suites.length - 1];
  if (!s) return;
  s.last = e.t;
  const run = s.runs.find(r => r.name === e.name);
  if (e.ev === 'start' && run) Object.assign(run, { status: 'running', started: e.t, log: e.log, out: e.out, ok: 0, fails: [], checks: [] });
  else if (e.ev === 'check' && run) {
    if (e.passed) run.ok++; else run.fails.push(e.text);
    run.checks.push({ passed: e.passed, text: e.text, t: e.t });
    if (run.checks.length > 60) run.checks.shift();
  } else if (e.ev === 'end' && run) {
    Object.assign(run, { status: e.passed ? 'passed' : 'failed', ok: e.ok, fails: e.fails, secs: e.secs, errors: e.errors || [],
      summary: e.summary || [], timedOut: e.timed_out, ended: e.t });
  } else if (e.ev === 'done') { s.ended = e.t; s.failed = e.failed; }
  else if (e.ev === 'stopped') {
    s.ended = e.t; s.stopped = true;
    for (const r of s.runs) if (r.status === 'running') r.status = 'stopped';
  }
}

function read() {
  let st;
  try { st = fs.statSync(file()); } catch { return false; }
  if (st.size < offset) { suites = []; offset = 0; partial = ''; }   // started afresh
  if (st.size === offset) return false;
  const fd = fs.openSync(file(), 'r');
  const buf = Buffer.alloc(st.size - offset);
  fs.readSync(fd, buf, 0, buf.length, offset);
  fs.closeSync(fd);
  offset = st.size;
  const text = partial + buf.toString('utf8');
  const lines = text.split('\n');
  partial = lines.pop();
  for (const l of lines) { if (!l.trim()) continue; try { apply(JSON.parse(l)); } catch {} }
  return true;
}

// How long each run usually takes, from the runs that finished before.
function typical() {
  const t = {};
  for (const s of suites) for (const r of s.runs) if (r.secs && r.status !== 'stopped') (t[r.name] ||= []).push(r.secs);
  const out = {};
  for (const [k, v] of Object.entries(t)) out[k] = Math.round(v.slice(-5).reduce((a, b) => a + b, 0) / Math.min(5, v.length));
  return out;
}

function running() {
  const s = suites[suites.length - 1];
  if (!s || s.ended) return false;
  if (proc) return true;
  return Date.now() / 1000 - s.last < 30 * 60;   // started elsewhere: alive while it keeps writing
}

function state() {
  const s = suites[suites.length - 1] || null;
  const history = suites.slice(-11, -1).reverse().map(x => ({
    started: x.started, ended: x.ended, stopped: x.stopped, total: x.runs.length,
    passed: x.runs.filter(r => r.status === 'passed').length, failed: x.runs.filter(r => r.status === 'failed').length }));
  return { suite: s, running: running(), typical: typical(), history, now: Date.now() / 1000 };
}

function summaryText() {
  const { suite: s, running: on, typical: typ } = state();
  if (!s) return 'No feature test run yet. Start one with tests_start.';
  const done = s.runs.filter(r => r.status === 'passed' || r.status === 'failed');
  const lines = [`${on ? 'Running' : s.stopped ? 'Stopped' : 'Finished'}: ${done.filter(r => r.status === 'passed').length} passed, ${done.filter(r => r.status === 'failed').length} failed, ${s.runs.length - done.length} to go.`];
  for (const r of s.runs) {
    let l = `- ${r.name}: ${r.status}`;
    if (r.status === 'running') l += ` (${Math.round(Date.now() / 1000 - r.started)}s of about ${typ[r.name] || '?'}s, ${r.ok} checks passed so far)`;
    else if (r.secs != null) l += ` in ${r.secs}s, ${r.ok} checks passed`;
    if (r.fails.length) l += `; failed: ${r.fails.join('; ')}`;
    if (r.status === 'failed' && r.errors.length) l += `\n    serial: ${r.errors.slice(-3).join(' | ')}`;
    lines.push(l);
  }
  return lines.join('\n');
}

function tick() {
  if (!read() && !(running() && Math.random() < 0.2)) return;
  const st = state();
  api.emit('state', st);
  const s = st.suite;
  if (loaded && s && s.ended && !s.stopped && s.ended > lastNotified) {
    lastNotified = s.ended;
    const failed = s.runs.filter(r => r.status === 'failed');
    api.ui.notify('Feature tests', failed.length ? `${failed.length} of ${s.runs.length} failed: ${failed.map(r => r.name).join(', ')}` : `All ${s.runs.length} runs passed`);
  }
}

async function start(args = {}) {
  if (running()) return 'A run is already going.';
  let only = args.only || '';
  if (args.failed) {
    const s = suites[suites.length - 1];
    only = s ? s.runs.filter(r => r.status === 'failed' || r.status === 'stopped').map(r => r.name).join(',') : '';
    if (!only) return 'Nothing failed last time.';
  }
  const a = ['-d', 'Ubuntu', '--', 'python3', wslPath(path.join(api.root, 'scripts', 'feature_checks.py'))];
  if (only) a.push('--only', only);
  proc = api.spawn('wsl', a, { onExit: () => { proc = null; tick(); } });
  api.log('started', a.join(' '));
  return only ? `Started: ${only}` : 'Started the whole suite.';
}

async function stop() {
  const s = suites[suites.length - 1];
  if (!s || s.ended) return 'Nothing is running.';
  if (s.pid) await api.shell(`wsl -d Ubuntu -- kill -TERM -${s.pid}`, { timeoutMs: 15000 });
  if (proc) { proc.kill(); proc = null; }
  fs.appendFileSync(file(), JSON.stringify({ ev: 'stopped', t: Date.now() / 1000 }) + '\n');
  tick();
  return 'Stopped.';
}

function details(name) {
  const s = suites[suites.length - 1];
  const r = s && s.runs.find(x => x.name.toLowerCase().includes(String(name || '').toLowerCase()));
  if (!r) return null;
  let tail = '';
  try { tail = fs.readFileSync(path.join(api.root, r.log), 'utf8').split('\n').slice(-60).join('\n'); } catch {}
  return { ...r, tail, logPath: r.log ? path.join(api.root, r.log) : null,
    serialPath: r.out ? path.join(api.root, r.out, 'serial.log') : null };
}

export default {
  async start(a) {
    api = a;
    read();
    loaded = true;
    const s = suites[suites.length - 1];
    lastNotified = s && s.ended ? s.ended : 0;
    timer = setInterval(tick, 2000);
  },
  async stop() { clearInterval(timer); },
  handlers: {
    async state() { read(); return state(); },
    async start(args) { return start(args); },
    async stop() { return stop(); },
    async details({ name }) { return details(name); },
  },
  tools: [
    { name: 'tests_status', description: 'Where the feature test suite is: each run with pass/fail, checks passed, time, and serial-log errors of failed runs.',
      parameters: { type: 'object', properties: {} }, async execute() { read(); return summaryText(); } },
    { name: 'tests_start', description: 'Start the feature test suite (about an hour). only: comma-separated run names or parts (e.g. "tray,alive"); failed: true reruns what failed last time.',
      parameters: { type: 'object', properties: { only: { type: 'string' }, failed: { type: 'boolean' } } }, async execute(args) { return start(args); } },
    { name: 'tests_stop', description: 'Stop the running suite and its VMs.', parameters: { type: 'object', properties: {} }, async execute() { return stop(); } },
    { name: 'tests_details', description: 'One run in full: its checks, failures, serial-log errors and the last lines of its output.',
      parameters: { type: 'object', properties: { name: { type: 'string' } }, required: ['name'] },
      async execute({ name }) { read(); const d = details(name); return d ? JSON.stringify({ ...d, checks: d.checks.slice(-20) }, null, 1) : 'No run by that name in the last suite.'; } },
  ],
};
