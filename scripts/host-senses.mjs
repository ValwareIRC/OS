// The PC's senses for the companion: what this computer's hardware is doing
// (CPU, GPU, memory, fans) and the RGB lights on it, which it can also set.
//
// It answers on 127.0.0.1:4571 (the VM reaches it as http://10.0.2.2:4571):
//   GET  /senses           the latest readings (refreshed every 2 s in the background)
//   POST /lights           {"target": "all" | id | kind, "color": "#rrggbb"}
//                          {"target", "effect": "static"|"breathe"|"pulse"|"off"|"restore", "color"?}
//                          optional "duration_s": give the lights back after that long
//   POST /lights/restore   hand every light back to iCUE / Mystic Light / Armoury Crate
//   GET  /health           {"ok": true}
//
// The hardware itself is reached by scripts\host-senses\OperatingSystemSenses.exe
// (built here from SensesHelper.cs with the C# compiler Windows ships), which
// uses the vendors' own SDKs. Nothing here runs commands for the VM: requests
// are checked against a short list of shapes and the lights that exist.
//
// Lights the companion changed are handed back when it asks, when no VM is
// running (checked every 15 s), and when this service or its helper stops.
//
//   node scripts/host-senses.mjs [--port 4571] [--exit-with-vm] [--log file]
import http from 'node:http';
import os from 'node:os';
import { spawn, execFile, execFileSync } from 'node:child_process';
import { appendFileSync, existsSync, readFileSync, statSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const helperDir = join(here, 'host-senses');
const helperSrc = join(helperDir, 'SensesHelper.cs');
const helperExe = join(helperDir, 'bin', 'OperatingSystemSenses.exe');
const arg = (name, fallback) => { const i = process.argv.indexOf(name); return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback; };
const port = Number(arg('--port', 4571)) || 4571;
const logFile = arg('--log', null);
const exitWithVm = process.argv.includes('--exit-with-vm');

const log = (...a) => {
  const line = [new Date().toISOString().slice(11, 19), ...a].join(' ');
  console.log(line);
  if (logFile) try { appendFileSync(logFile, line + '\n'); } catch {}
};

const KINDS = ['keyboard', 'mouse', 'fan', 'motherboard', 'ram', 'ssd', 'strip', 'headset', 'cooler', 'gpu', 'other'];
const EFFECTS = ['static', 'breathe', 'pulse', 'off', 'restore'];

// ---------------------------------------------------------------- the helper

// Built when missing or older than its source (an installed copy has no source).
function buildHelper() {
  if (!existsSync(helperSrc)) return existsSync(helperExe);
  if (existsSync(helperExe) && statSync(helperExe).mtimeMs >= statSync(helperSrc).mtimeMs) return true;
  const csc = join(process.env.WINDIR || 'C:\\Windows', 'Microsoft.NET', 'Framework64', 'v4.0.30319', 'csc.exe');
  if (!existsSync(csc)) { log('no C# compiler at', csc); return existsSync(helperExe); }
  try {
    mkdirSync(dirname(helperExe), { recursive: true });
    execFileSync(csc, ['-nologo', '-optimize', '-platform:x64', '-target:exe', '-out:' + helperExe,
      '-r:System.Web.Extensions.dll', '-r:Microsoft.CSharp.dll', '-r:System.Core.dll', helperSrc],
      { windowsHide: true, stdio: ['ignore', 'pipe', 'pipe'] });
    log('built', helperExe);
    return true;
  } catch (e) {
    log('could not build the helper:', String(e.stdout || e.message).trim().split('\n').slice(-3).join(' | '));
    return existsSync(helperExe);
  }
}

let helper = null, nextId = 1, stopping = false, helperFails = 0;
const pending = new Map();

function startHelper() {
  if (stopping || !buildHelper()) return;
  const child = spawn(helperExe, [], { windowsHide: true, stdio: ['pipe', 'pipe', 'pipe'] });
  helper = child;
  let buf = '';
  child.stdout.setEncoding('utf8');
  child.stdout.on('data', d => {
    buf += d;
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i); buf = buf.slice(i + 1);
      let r; try { r = JSON.parse(line); } catch { continue; }
      const p = pending.get(r.id);
      if (p) { pending.delete(r.id); clearTimeout(p.timer); p.resolve(r); }
    }
  });
  child.stderr.setEncoding('utf8');
  child.stderr.on('data', d => String(d).split('\n').filter(Boolean).forEach(l => log('helper:', l.trim())));
  child.on('error', e => log('helper failed to start:', e.message));
  child.on('exit', code => {
    if (helper === child) helper = null;
    for (const [id, p] of pending) { clearTimeout(p.timer); p.resolve({ error: 'helper exited' }); pending.delete(id); }
    if (stopping) return;
    helperFails++;
    log(`helper exited (${code}); starting it again`);
    setTimeout(startHelper, Math.min(60000, 2000 * helperFails));
  });
  log('helper started, pid', child.pid);
}

function ask(op, extra = {}, ms = 6000) {
  return new Promise(resolve => {
    if (!helper) return resolve({ error: 'helper not running' });
    const id = nextId++;
    const timer = setTimeout(() => { pending.delete(id); resolve({ error: 'helper timed out' }); }, ms);
    pending.set(id, { resolve, timer });
    helper.stdin.write(JSON.stringify({ id, op, ...extra }) + '\n');
  });
}

// ---------------------------------------------------------------- readings

// CPU load from Node itself, for when the helper can't run.
let lastTimes = null;
function nodeCpuLoad() {
  const t = os.cpus().map(c => c.times);
  const sum = x => x.user + x.nice + x.sys + x.irq + x.idle;
  let load = null;
  if (lastTimes) {
    let busy = 0, all = 0;
    t.forEach((c, i) => { const d = sum(c) - sum(lastTimes[i]); all += d; busy += d - (c.idle - lastTimes[i].idle); });
    if (all > 0) load = Math.round(1000 * busy / all) / 10;
  }
  lastTimes = t;
  return load;
}

// GPU through nvidia-smi, for when NVML can't be reached from the helper.
function nvidiaSmi() {
  return new Promise(resolve => execFile('nvidia-smi', ['--query-gpu=name,utilization.gpu,temperature.gpu,fan.speed,power.draw,memory.used,memory.total',
    '--format=csv,noheader,nounits'], { windowsHide: true, timeout: 5000 }, (e, out) => {
    if (e) return resolve([]);
    const num = s => { const n = Number(String(s).trim()); return Number.isFinite(n) ? n : null; };
    resolve(out.trim().split('\n').filter(Boolean).map(l => {
      const f = l.split(',');
      return { name: f[0].trim(), load: num(f[1]), temp_c: num(f[2]), fans_pct: num(f[3]) === null ? [] : [num(f[3])],
               power_w: num(f[4]), vram_used_mb: num(f[5]), vram_total_mb: num(f[6]) };
    }));
  }));
}

// iCUE keeps a per-device "SDK" switch; devices with it off are invisible to
// other programs, so name them instead of pretending they aren't there.
function corsairHidden() {
  try {
    const cfg = readFileSync(join(process.env.APPDATA || '', 'Corsair', 'CUE5', 'config.cuecfg'), 'utf8');
    const devices = cfg.split(/<map name="Devices">/)[1] || '';
    const out = [];
    const re = /\n\t\t<map name="([^"]+)">([\s\S]*?)\n\t\t<\/map>/g;
    let m;
    while ((m = re.exec(devices))) if (/name="SdkSoftwareIntegrationEnabled">false</.test(m[2])) out.push(m[1]);
    return out;
  } catch { return []; }
}

function dominant(colors) {
  const counts = new Map();
  for (const c of colors) counts.set(c, (counts.get(c) || 0) + 1);
  // Unlit LEDs only win when everything is dark.
  const lit = [...counts].filter(([c]) => c !== '#000000');
  const pool = lit.length ? lit : [...counts];
  return pool.sort((a, b) => b[1] - a[1])[0]?.[0] ?? null;
}

function sample(colors, n = 16) {
  if (colors.length <= n) return colors.slice();
  return Array.from({ length: n }, (_, i) => colors[Math.floor((i + 0.5) * colors.length / n)]);
}

let snapshot = { time: new Date().toISOString(), host: { name: os.hostname(), cpu: { model: os.cpus()[0]?.model?.trim() ?? null, load: null, temp_c: null, clock_mhz: null },
  gpu: [], memory: { used_mb: null, total_mb: null }, fans: [] }, lights: [], missing: ['The senses are still waking up.'] };
let lastRead = null;
const heldByUs = new Set();

function missingNotes(r) {
  const m = [];
  const st = r?.status || {};
  if (!r || r.error) m.push('The hardware helper is not running, so lights and CPU temperature cannot be sensed (see build\\host-senses.log).');
  if (r && !r.error && r.cpu?.temp_c == null) {
    if (st.ryzen === 'needs-admin') m.push('CPU temperature needs the senses service to run as administrator once: run scripts\\host-senses\\enable-admin.ps1 in an administrator PowerShell; it registers a logon task, so it is a one-time step.');
    else if (st.ryzen === 'not-installed') m.push('CPU temperature needs the AMD Ryzen Master SDK, which is no longer installed.');
    else m.push(`CPU temperature could not be read through AMD's Ryzen Master SDK (${st.ryzen || 'no reading'}).`);
  }
  m.push('Case and CPU-cooler fan speeds cannot be read: the MSI motherboard\'s fan chip is only reachable through a kernel driver, and no installed program shares its readings. GPU fans are reported.');
  const c = st.corsair;
  if (c) {
    if (!c.sdk) m.push('Corsair lights need the iCUE SDK file iCUESDK.x64_2019.dll in scripts\\host-senses.');
    else if (c.state === 'refused') m.push('iCUE refused the senses service: in iCUE Settings, turn on Software and Games Integrations and allow OperatingSystemSenses.exe.');
    else if (c.state !== 'connected') m.push(`Corsair lights are not reachable: iCUE is ${c.state === 'closed' || c.state === 'timeout' ? 'not answering (is it running, with Software and Games Integrations on in its Settings?)' : c.state}.`);
    const seen = new Set((c.devices || []).map(d => d.toUpperCase()));
    for (const name of corsairHidden()) if (!seen.has(name.toUpperCase()))
      m.push(`The Corsair ${name} is hidden from other programs: in iCUE, open the ${name}'s Device Settings and turn on its SDK / software integration switch.`);
  }
  const unknown = (r?.lights || []).filter(l => l.vendor === 'corsair' && !heldByUs.has(l.id) && l.colors.length && l.colors.every(x => x === '#000000'));
  if (unknown.length) m.push(`iCUE does not tell other programs the colours of its own lighting effects, so the current colours of the ${unknown.map(l => l.name).join(' and ')} are unknown; they can still be set.`);
  const msi = st.msi;
  if (msi && !msi.ready) {
    if (msi.error === 'no-sdk-dll') m.push('MSI motherboard lighting needs MSI Center (Mystic Light) or iCUE\'s MSI plugin.');
    else if (!r.admin) m.push(`The MSI motherboard's lighting (Mystic Light: its ARGB header, where case-fan lights usually plug in) only answers an elevated program${msi.error === 'listing' ? ' (still asking; it takes about a minute)' : ''}; the same one-time administrator step (scripts\\host-senses\\enable-admin.ps1) enables it.`);
    else if (msi.error === 'listing' || msi.error === 'starting') m.push('MSI Mystic Light is still listing its lights (it takes up to a minute).');
    else m.push(`MSI Mystic Light did not answer (${msi.error}); is MSI Center running?`);
  }
  return m;
}

async function refresh() {
  const r = await ask('read', {}, 8000);
  const ok = r && !r.error;
  if (ok) lastRead = r;
  const nodeLoad = nodeCpuLoad();
  let gpu = ok ? r.gpu : [];
  if (!gpu.length) gpu = await nvidiaSmi();
  const lights = (ok ? r.lights : []).map(l => {
    const unknown = l.vendor === 'corsair' && !heldByUs.has(l.id) && l.colors.length > 0 && l.colors.every(x => x === '#000000');
    const out = { id: l.id, name: l.name, kind: KINDS.includes(l.kind) ? l.kind : 'other', vendor: l.vendor, leds: l.leds ?? (l.colors.length || null),
                  color: unknown ? null : dominant(l.colors), colors: unknown ? [] : sample(l.colors), controllable: !!l.controllable };
    if (l.effect) out.effect = l.effect;
    if (heldByUs.has(l.id)) out.set_by_companion = true;
    return out;
  });
  const free = os.freemem(), total = os.totalmem();
  snapshot = {
    time: new Date().toISOString(),
    host: {
      name: os.hostname(),
      cpu: { model: (ok && r.cpu.model) || os.cpus()[0]?.model?.trim() || null, load: ok && r.cpu.load != null ? r.cpu.load : nodeLoad,
             temp_c: ok ? r.cpu.temp_c : null, clock_mhz: ok ? r.cpu.clock_mhz : null },
      gpu: gpu.map(g => ({ name: g.name, load: g.load, temp_c: g.temp_c,
                           fan_pct: g.fans_pct?.length ? Math.round(g.fans_pct.reduce((a, b) => a + b, 0) / g.fans_pct.length) : null,
                           power_w: g.power_w, vram_used_mb: g.vram_used_mb, vram_total_mb: g.vram_total_mb })),
      memory: { used_mb: Math.round((total - free) / 1048576), total_mb: Math.round(total / 1048576) },
      fans: gpu.flatMap(g => (g.fans_pct || []).map((p, i) => ({ name: `${g.name} fan ${i + 1}`, rpm: null, pct: p }))),
    },
    lights,
    missing: missingNotes(r),
  };
}

let refreshing = false;
async function tick() {
  if (refreshing) return;
  refreshing = true;
  try { await refresh(); } catch (e) { log('refresh failed:', e.message); }
  refreshing = false;
}

// ---------------------------------------------------------------- lights

const restoreTimers = new Map();

async function restoreLights(ids) {
  for (const id of ids || [...restoreTimers.keys()]) { clearTimeout(restoreTimers.get(id)); restoreTimers.delete(id); }
  const r = await ask('restore', ids ? { ids } : {}, 15000);
  if (ids) ids.forEach(id => heldByUs.delete(id)); else heldByUs.clear();
  log('restored', ids ? ids.join(', ') : 'all lights', r.error ? `(${r.error})` : '');
  setTimeout(tick, 300);
  return r;
}

function resolveTarget(target) {
  const ctl = snapshot.lights.filter(l => l.controllable);
  if (target === 'all') return ctl;
  if (KINDS.includes(target)) return ctl.filter(l => l.kind === target);
  return ctl.filter(l => l.id === target);
}

async function setLights(q) {
  const targets = resolveTarget(q.target);
  if (!targets.length) return [404, { error: `No controllable light matches "${q.target}".`, lights: snapshot.lights.map(l => l.id) }];
  const ids = targets.map(l => l.id);
  if (q.effect === 'restore') {
    const r = await restoreLights(ids);
    return [r.error ? 502 : 200, { ok: !r.error, restored: ids, error: r.error }];
  }
  const effect = q.effect || 'static';
  const r = await ask('set', { ids, effect, color: effect === 'off' ? '#000000' : q.color.toLowerCase() }, 10000);
  if (r.error) return [502, { error: r.error }];
  const names = Object.fromEntries(targets.map(l => [l.id, l.name]));
  for (const c of r.changed) {
    heldByUs.add(c.id);
    c.name = names[c.id];
    clearTimeout(restoreTimers.get(c.id)); restoreTimers.delete(c.id);
    if (q.duration_s) restoreTimers.set(c.id, setTimeout(() => restoreLights([c.id]), q.duration_s * 1000));
  }
  log(`lights ${effect} ${effect === 'off' ? '' : q.color} on ${r.changed.map(c => c.id).join(', ') || 'nothing'}` +
      (r.failed.length ? `; failed ${r.failed.map(f => f.id + ': ' + f.reason).join(', ')}` : ''));
  setTimeout(tick, 300);
  return [r.changed.length ? 200 : 502, { ok: r.changed.length > 0, changed: r.changed, failed: r.failed,
    ...(q.duration_s ? { restore_after_s: q.duration_s } : {}) }];
}

// Only these shapes are accepted; anything else is a 400.
function validateLights(q) {
  if (!q || typeof q !== 'object' || Array.isArray(q)) return 'Send a JSON object.';
  const extra = Object.keys(q).filter(k => !['target', 'color', 'effect', 'duration_s'].includes(k));
  if (extra.length) return `Unknown field ${extra[0]}.`;
  if (typeof q.target !== 'string' || !/^[a-z0-9:_-]{1,80}$/.test(q.target)) return 'target must be "all", a kind or a light id.';
  if (q.effect !== undefined && !EFFECTS.includes(q.effect)) return `effect must be one of ${EFFECTS.join(', ')}.`;
  if (q.color !== undefined && (typeof q.color !== 'string' || !/^#[0-9a-fA-F]{6}$/.test(q.color))) return 'color must look like #rrggbb.';
  if (['static', 'breathe', 'pulse', undefined].includes(q.effect) && q.color === undefined) return 'A color is needed for that effect.';
  if (q.duration_s !== undefined && !(Number.isFinite(q.duration_s) && q.duration_s >= 1 && q.duration_s <= 86400)) return 'duration_s must be 1 to 86400.';
  return null;
}

// ---------------------------------------------------------------- the VM

// Lights go back to their vendors once no VM is running; with --exit-with-vm
// the service also leaves (after a minute), like the Claude bridge.
let vmMissing = 0;
setInterval(() => execFile('tasklist', ['/FI', 'IMAGENAME eq qemu-system-x86_64.exe', '/NH'], { windowsHide: true }, async (e, out) => {
  vmMissing = /qemu-system/i.test(out || '') ? 0 : vmMissing + 1;
  if (vmMissing >= 2 && heldByUs.size) { log('no VM is running; giving the lights back'); await restoreLights(); }
  if (exitWithVm && vmMissing >= 4) { log('the VM is gone; stopping'); await shutdown(); }
}), 15000);

async function shutdown() {
  if (stopping) return;
  stopping = true;
  try { if (heldByUs.size || lastRead?.holding) await restoreLights(); } catch {}
  if (helper) { helper.stdin.end(); await new Promise(r => { const t = setTimeout(r, 4000); helper?.once('exit', () => { clearTimeout(t); r(); }); }); }
  process.exit(0);
}
for (const sig of ['SIGINT', 'SIGTERM', 'SIGBREAK', 'SIGHUP']) process.on(sig, shutdown);

// ---------------------------------------------------------------- HTTP

function send(res, status, body) {
  const s = JSON.stringify(body);
  res.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(s), 'Cache-Control': 'no-store' });
  res.end(s);
}

// Only the VM (10.0.2.2) and this PC by name: a web page that tricks the
// browser into calling 127.0.0.1 carries another Host and is turned away.
const okHosts = new Set(['127.0.0.1', 'localhost', '10.0.2.2'].flatMap(h => [h, `${h}:${port}`]));
const posts = [];

const server = http.createServer((req, res) => {
  const host = String(req.headers.host || '').toLowerCase();
  if (host && !okHosts.has(host)) return send(res, 403, { error: 'Not for this host.' });
  const path = req.url.split('?')[0];
  if (req.method === 'GET' && path === '/health') return send(res, 200, { ok: true });
  if (req.method === 'GET' && path === '/senses') return send(res, 200, snapshot);
  if (req.method !== 'POST' || !['/lights', '/lights/restore'].includes(path)) return send(res, 404, { error: 'Not here.' });
  if (!/^application\/json\b/i.test(req.headers['content-type'] || '') && path === '/lights') return send(res, 415, { error: 'Send application/json.' });
  const now = Date.now();
  while (posts.length && now - posts[0] > 1000) posts.shift();
  if (posts.length >= 10) return send(res, 429, { error: 'Too many changes at once.' });
  posts.push(now);
  let body = '', size = 0;
  req.setEncoding('utf8');
  req.on('data', d => { size += d.length; if (size > 4096) { send(res, 413, { error: 'Too big.' }); req.destroy(); } else body += d; });
  req.on('end', async () => {
    if (res.writableEnded) return;
    try {
      if (path === '/lights/restore') {
        const r = await restoreLights();
        return send(res, r.error ? 502 : 200, { ok: !r.error, restored: 'all', error: r.error });
      }
      let q;
      try { q = JSON.parse(body); } catch { return send(res, 400, { error: 'That was not JSON.' }); }
      const bad = validateLights(q);
      if (bad) return send(res, 400, { error: bad });
      const [status, out] = await setLights(q);
      send(res, status, out);
    } catch (e) {
      log('request failed:', e.message);
      send(res, 500, { error: 'Something went wrong.' });
    }
  });
});

server.on('error', e => { log('cannot listen:', e.message); process.exit(1); });
server.listen(port, '127.0.0.1', () => {
  log(`host senses on 127.0.0.1:${port} (the VM: http://10.0.2.2:${port}/senses)`);
  startHelper();
  nodeCpuLoad();
  setTimeout(tick, 1200);
  setInterval(tick, 2000);
});
