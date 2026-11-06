// Performance fixes: the slow spots found, each fix's state, and the measurements that judge them, run as the
// project's own test scripts (tests/*_win.py, which print "PERF name=value" lines). One run at a time: each boots
// a VM of its own.

const PYTHON = 'C:\\Users\\Valerie\\AppData\\Local\\Programs\\Python\\Python312\\python.exe'

// What can be measured, and how long it takes.
const MEASUREMENTS = {
  wake: { name: 'Wake-up delay', script: 'tests/wake_latency_win.py', minutes: 1 },
  idle: { name: 'Idle desktop', script: 'tests/perf_idle_vm_win.py', minutes: 3 },
  idle_before: { name: 'Idle desktop, drawn as before', script: 'tests/perf_idle_vm_win.py', minutes: 3, env: { ALWAYS_DRAW: '1' }, records: 'before' },
  firefox: { name: 'Firefox install and launch', script: 'tests/browser_vm_win.py', minutes: 7 },
  net: { name: 'Network, one download', script: 'tests/net_speed_win.py', minutes: 1, env: { NIC: 'e1000e' } },
  net2: { name: 'Network, two at once', script: 'tests/net_speed_win.py', minutes: 1, env: { MANY: '2', NIC: 'e1000e' } },
  net_e1000: { name: 'Network, the older e1000 card', script: 'tests/net_speed_win.py', minutes: 1, env: { NIC: 'e1000' } },
  disk: { name: 'Disk', script: 'tests/disk_speed_win.py', minutes: 1 },
  resize: { name: 'Moving and resizing a window', script: 'tests/perf_resize_vm_win.py', minutes: 2 }
}

// The numbers. better: which way is good.
const METRICS = {
  poll_round_trip_us: { name: 'Message round trip, poll', unit: 'µs', better: 'lower', by: 'wake' },
  epoll_round_trip_us: { name: 'Message round trip, epoll', unit: 'µs', better: 'lower', by: 'wake' },
  sleep_1ms_us: { name: 'A 1 ms sleep takes', unit: 'µs', better: 'lower', by: 'wake' },
  firefox_first_page_s: { name: 'Firefox first launch', unit: 's', better: 'lower', by: 'firefox' },
  firefox_again_page_s: { name: 'Firefox next launch', unit: 's', better: 'lower', by: 'firefox' },
  firefox_first_window_s: { name: 'Firefox first window', unit: 's', better: 'lower', by: 'firefox' },
  firefox_install_s: { name: 'Firefox install', unit: 's', better: 'lower', by: 'firefox' },
  idle_vm_host_cpu_pct: { name: 'The idle VM, on this PC', unit: '% of a core', better: 'lower', by: 'idle' },
  idle_desktop_cpu_pct: { name: 'Desktop CPU, idle', unit: '% of a core', better: 'lower', by: 'idle' },
  idle_machine_busy_pct: { name: 'Machine busy, idle', unit: '%', better: 'lower', by: 'idle' },
  idle_kernel_pct: { name: 'Kernel share, idle', unit: '%', better: 'lower', by: 'idle' },
  largest_kept_file_mb: { name: 'Largest file kept', unit: 'MB', better: 'higher', by: null },
  home_limit_mb: { name: 'Home size limit', unit: 'MB', better: 'higher', by: null },
  net_single_mbs: { name: 'One download', unit: 'MB/s', better: 'higher', by: 'net' },
  net_e1000_mbs: { name: 'One download, older e1000', unit: 'MB/s', better: 'higher', by: 'net_e1000' },
  net_together_mbs: { name: 'Two downloads together', unit: 'MB/s', better: 'higher', by: 'net2' },
  tcp_far_mbs: { name: 'Download from 30 ms away', unit: 'MB/s', better: 'higher', by: null },
  resize_frame_ms: { name: 'Frame work, resizing', unit: 'ms', better: 'lower', by: 'resize' },
  resize_interval_ms: { name: 'Frame interval, resizing', unit: 'ms', better: 'lower', by: 'resize' },
  move_frame_ms: { name: 'Frame work, moving', unit: 'ms', better: 'lower', by: 'resize' },
  getrusage_works: { name: 'getrusage works', unit: 'yes/no', better: 'higher', by: 'wake' },
  disk_big_write_mbs: { name: '200 MB file write', unit: 'MB/s', better: 'higher', by: 'disk' },
  disk_small_synced_per_s: { name: 'Synced small files', unit: 'a second', better: 'higher', by: 'disk' },
  clock_error_pct: { name: 'Clock speed error, worst boot', unit: '%', better: 'lower', by: null },
  clock_error_test_pct: { name: 'Clock speed error, test VMs', unit: '%', better: 'lower', by: null },
  max_descriptors: { name: 'Files and sockets a program may open', unit: '', better: 'higher', by: null },
  model_read_at_boot_s: { name: 'Built-in model read at boot (mind elsewhere)', unit: 's', better: 'lower', by: null },
  model_memory_at_boot_mb: { name: 'Memory it held (mind elsewhere)', unit: 'MB', better: 'lower', by: null }
}

// The slow spots, in the order they are being fixed.
const FIXES = [
  { id: 'waits', title: 'Waiting programs wake up late',
    what: 'poll, select and epoll looked again every 4 ms instead of waking the moment something arrives, and every sleep was rounded to 4 ms. Every message between programs (D-Bus, Wayland apps, Firefox, sound) could wait up to 4 ms at each step.',
    plan: 'Sleep on the files\' wait queues and wake at once; precise timers for sleeps.',
    metrics: ['poll_round_trip_us', 'epoll_round_trip_us', 'sleep_1ms_us', 'firefox_first_page_s', 'firefox_again_page_s'] },
  { id: 'idle', title: 'The desktop redraws when nothing changes',
    what: 'The whole screen was drawn and copied to the display 30 times a second, moving or not: half a core, all the time.',
    plan: 'No frame when nothing changed; then only the parts that changed.',
    metrics: ['idle_vm_host_cpu_pct', 'idle_desktop_cpu_pct', 'idle_machine_busy_pct', 'idle_kernel_pct'] },
  { id: 'rusage', title: 'Programs can\'t see their own CPU time',
    what: 'getrusage always answered zero, so `time`, build tools and programs measuring themselves saw nothing.',
    plan: 'Answer from the kernel\'s own per-thread accounting.',
    metrics: ['getrusage_works'] },
  { id: 'nic', title: 'The network card is looked at every 4 ms',
    what: 'The e1000 card was polled once a tick with 64 receive slots: downloads top out at about 24 MB/s, a fifth of gigabit.',
    plan: 'Interrupts, bigger rings, one register write per batch.',
    metrics: ['net_single_mbs', 'net_together_mbs', 'net_e1000_mbs'] },
  { id: 'tcp', title: 'TCP can\'t fill a line far away',
    what: 'No window scaling (64 KB in flight) and out-of-order segments thrown away. Hidden in the VM; on a real PC a download from 30 ms away is held to about 2 MB/s, under half of a 40 Mbit line.',
    plan: 'Window scaling, bigger buffers, an out-of-order queue, SACK, delayed ACKs.',
    metrics: ['tcp_far_mbs', 'firefox_install_s'] },
  { id: 'resize', title: 'Resizing windows is slow',
    what: 'Earlier runs measured 100-150 ms of work per frame while resizing (7-10 frames a second), against 7-10 ms while moving.',
    plan: 'Measure on this PC first, then keep what doesn\'t change while the size does.',
    metrics: ['resize_frame_ms', 'resize_interval_ms', 'move_frame_ms'] },
  { id: 'simd', title: 'Drawing uses 2003-era instructions',
    what: 'Everything is built for the first 64-bit processors (no AVX2), so blending, blurs and scaling do a quarter of the work per instruction they could.',
    plan: 'Build the drawing code for modern processors too, chosen when it starts.',
    metrics: ['resize_frame_ms', 'idle_desktop_cpu_pct'] },
  { id: 'home', title: 'Your files live in memory',
    what: 'Your home is kept in RAM and the whole of it is written to the disk as one archive at every save, checked every 2 s on the desktop\'s own thread. At most 128 MB, and files over 32 MB aren\'t kept at all.',
    plan: 'The home on the disk itself (ext2, with a journal so a power cut can\'t break it).',
    metrics: ['largest_kept_file_mb', 'home_limit_mb'] },
  { id: 'clock', title: 'The clock can run at half speed',
    what: 'Found in Valerie\'s own session: the boot measured the timer against one 10 ms window of the PIT, and one slow port read in the VM stretched it. That boot ran every clock at 0.465 of real time (a second lasted two; Firefox\'s start took 14 minutes; 122 s behind by the time the network set it).',
    plan: 'Measure the counter over 50 ms with both ends pinned between two readings, again until sure to 0.1%; the timer against the counter.',
    metrics: ['clock_error_pct', 'clock_error_test_pct'] },
  { id: 'fds', title: 'A program can open only 256 files',
    what: 'Found in the same session: a fixed table of 256 descriptors per program, and setrlimit ignored. Firefox needs more, stalled, and its next socket() freed the socket twice: a kernel panic.',
    plan: 'A table that grows to Linux\'s limits (1024, raised by the program up to 65536); EMFILE, never a double free.',
    metrics: ['max_descriptors'] },
  { id: 'bootmodel', title: 'The built-in model is read at every boot',
    what: '5 GB from the disc into memory at every start (22 s on a test VM, about 3 minutes on Valerie\'s slow boot), even with the companion thinking with Claude.',
    plan: 'Left on the disc unless it is the mind; read when chosen, or when it stands in (asked first) for a mind that can\'t be reached.',
    metrics: ['model_read_at_boot_s', 'model_memory_at_boot_mb'] }
]

// What was measured before any of this, 2026-11-03 (by hand, in the VM, before this panel existed).
const SEED = {
  idle_desktop_cpu_pct: { value: 53, note: 'os-shell, 10 s of an idle desktop' },
  idle_machine_busy_pct: { value: 12.3, note: '8 CPUs, 10 s' },
  idle_kernel_pct: { value: 5.9, note: '8 CPUs, 10 s' },
  net_single_mbs: { value: 21.9, note: '200 MB from this PC (e1000, polled every tick)' },
  net_e1000_mbs: { value: 21.9, note: '200 MB from this PC (polled every tick)' },
  net_together_mbs: { value: 32.8, note: '21.5 + 11.3 MB/s' },
  tcp_far_mbs: { value: 2.2, note: 'worked out: 64 KB / 30 ms (no window scaling)' },
  firefox_install_s: { value: 162, note: 'after the install fix; it was 20 minutes' },
  largest_kept_file_mb: { value: 32, note: 'store.c MAX_FILE' },
  home_limit_mb: { value: 128, note: 'store.c SLOT_SIZE' },
  getrusage_works: { value: 0, note: 'returns all zeros' },
  disk_big_write_mbs: { value: 540, note: 'disk-speed' },
  disk_small_synced_per_s: { value: 482, note: 'disk-speed' }
}

let api
let run = null // { id, started, proc, lines, perf }

function now() { return Date.now() }

async function load() {
  const st = (await api.state.get('data', null)) || { metrics: {}, fixes: {}, runs: [] }
  for (const [k, s] of Object.entries(SEED)) {
    st.metrics[k] = st.metrics[k] || { history: [] }
    if (!st.metrics[k].before) st.metrics[k].before = { value: s.value, at: Date.parse('2026-11-03T22:30:00Z'), note: s.note }
  }
  for (const f of FIXES) st.fixes[f.id] = st.fixes[f.id] || { status: 'found', note: '', commit: '' }
  return st
}

async function save(st) { await api.state.set('data', st) }

function fmt(v, m) {
  if (v === undefined || v === null) return '—'
  if (m && m.unit === 'yes/no') return v ? 'yes' : 'no'
  const n = Number(v)
  return (Math.abs(n) >= 100 ? n.toFixed(0) : Math.abs(n) >= 10 ? n.toFixed(1) : n.toFixed(2)).replace(/\.0+$|(\.\d*[1-9])0+$/, '$1')
}

// A result for a metric: kept in its history; the first ever is its "before"; once its fix is being worked on, the
// newest is its "after".
function record(st, key, value, source, as) {
  const m = (st.metrics[key] = st.metrics[key] || { history: [] })
  const entry = { value: Number(value), at: now(), source }
  m.history.push(entry)
  m.history = m.history.slice(-30)
  const owners = FIXES.filter((f) => f.metrics.includes(key))
  const working = owners.some((f) => ['working', 'fixed', 'verified'].includes(st.fixes[f.id].status))
  if (as === 'before' || !m.before) m.before = { ...entry, note: source }
  else if (working) m.after = entry
}

async function snapshot() {
  const st = await load()
  return {
    fixes: FIXES.map((f) => ({ ...f, ...st.fixes[f.id],
      metrics: f.metrics.map((k) => ({ key: k, ...METRICS[k], before: st.metrics[k]?.before, after: st.metrics[k]?.after,
        history: st.metrics[k]?.history || [] })) })),
    measurements: Object.entries(MEASUREMENTS).map(([id, m]) => ({ id, ...m })),
    run: run && { id: run.id, name: MEASUREMENTS[run.id].name, started: run.started, lines: run.lines.slice(-14), perf: run.perf, done: run.done, code: run.code },
    runs: st.runs.slice(-12).reverse()
  }
}

async function push() { api.emit('state', await snapshot()) }

async function startRun(id) {
  const m = MEASUREMENTS[id]
  if (!m) throw new Error('no measurement called ' + id + ' (there are: ' + Object.keys(MEASUREMENTS).join(', ') + ')')
  if (run && !run.done) throw new Error(MEASUREMENTS[run.id].name + ' is still running: one at a time (each boots a VM)')
  const env = { ...process.env, ISO: api.root + '\\build\\dev.iso', PYTHONUNBUFFERED: '1', ...(m.env || {}) }
  if (!m.env || !m.env.MANY) env.MANY = ''
  run = { id, started: now(), lines: [], perf: {}, done: false, code: null, buf: '' }
  const mine = run
  let finish
  mine.finished = new Promise((r) => (finish = r))
  mine.proc = api.spawn(PYTHON, [m.script], {
    cwd: api.root, env,
    onData(chunk) {
      mine.buf += chunk.toString()
      const parts = mine.buf.split(/\r?\n/)
      mine.buf = parts.pop()
      for (const line of parts) {
        if (!line.trim()) continue
        mine.lines.push(line.slice(0, 220))
        mine.lines = mine.lines.slice(-200)
        const p = line.match(/^PERF (\w+)=([-\d.]+)/)
        if (p) mine.perf[id === 'net_e1000' && p[1] === 'net_single_mbs' ? 'net_e1000_mbs' : p[1]] = Number(p[2])
      }
      push()
    },
    async onExit(code) {
      mine.done = true
      mine.code = code
      const st = await load()
      for (const [k, v] of Object.entries(mine.perf)) if (v !== 999) record(st, k, v, m.name, m.records) // (999: not seen within its time)
      st.runs.push({ id, name: m.name, at: mine.started, secs: Math.round((now() - mine.started) / 1000), code,
        perf: mine.perf, stopped: !!mine.stopped })
      st.runs = st.runs.slice(-40)
      await save(st)
      api.log(`${m.name}: exit ${code}, ${JSON.stringify(mine.perf)}`)
      if (Object.keys(mine.perf).length) api.ui.notify('Measured: ' + m.name, Object.entries(mine.perf).map(([k, v]) => `${METRICS[k]?.name || k}: ${fmt(v, METRICS[k])}`).join('; ').slice(0, 300))
      await push()
      finish()
    }
  })
  await push()
  return mine
}

function summaryText(s) {
  const out = []
  for (const f of s.fixes) {
    out.push(`${f.id} [${f.status}] ${f.title}${f.commit ? ' (' + f.commit + ')' : ''}${f.note ? ' — ' + f.note : ''}`)
    for (const m of f.metrics) {
      const b = m.before ? fmt(m.before.value, m) : '—', a = m.after ? fmt(m.after.value, m) : '—'
      out.push(`    ${m.key}: before ${b}, after ${a} ${m.unit}`)
    }
  }
  if (s.run) out.push(`run: ${s.run.name} ${s.run.done ? 'done (exit ' + s.run.code + ')' : 'running for ' + Math.round((now() - s.run.started) / 1000) + ' s'}`)
  return out.join('\n')
}

export default {
  async start(a) { api = a },
  async stop() { if (run && !run.done) { run.stopped = true; run.proc?.kill() } },
  handlers: {
    async state() { return snapshot() },
    async run({ id }) { await startRun(id); return snapshot() },
    async stop() {
      if (run && !run.done) { run.stopped = true; run.proc.kill() }
      return snapshot()
    },
    async setStatus({ id, status }) {
      const st = await load()
      if (!st.fixes[id]) throw new Error('no fix ' + id)
      st.fixes[id].status = status
      await save(st)
      await push()
      return snapshot()
    }
  },
  tools: [
    { name: 'perf_status', description: 'The slow spots, each fix\'s state, commit and note, and every metric\'s before and after.',
      parameters: { type: 'object', properties: {} },
      async execute() { return summaryText(await snapshot()) } },
    { name: 'perf_run', description: 'Run a measurement (boots a VM; one at a time): wake, idle, firefox, net, net2, disk, resize. With wait, returns its results when it ends.',
      parameters: { type: 'object', properties: { id: { type: 'string' }, wait: { type: 'boolean', description: 'wait for it to finish (up to 15 minutes)' } }, required: ['id'] },
      async execute({ id, wait }) {
        const r = await startRun(id)
        if (!wait) return `started ${MEASUREMENTS[id].name} (about ${MEASUREMENTS[id].minutes} min)`
        await Promise.race([r.finished, new Promise((res) => setTimeout(res, 15 * 60 * 1000))])
        return `${MEASUREMENTS[id].name}: ${r.done ? 'exit ' + r.code : 'still running'}\n` +
          (Object.entries(r.perf).map(([k, v]) => `${k}=${v}`).join('\n') || 'no PERF lines') + '\n--- last lines:\n' + r.lines.slice(-12).join('\n')
      } },
    { name: 'perf_wait', description: 'Wait for the measurement running now to finish and return its results.',
      parameters: { type: 'object', properties: {} },
      async execute() {
        if (!run) return 'nothing has run'
        if (!run.done) await Promise.race([run.finished, new Promise((res) => setTimeout(res, 15 * 60 * 1000))])
        return `${MEASUREMENTS[run.id].name}: ${run.done ? 'exit ' + run.code : 'still running'}\n` +
          (Object.entries(run.perf).map(([k, v]) => `${k}=${v}`).join('\n') || 'no PERF lines') + '\n--- last lines:\n' + run.lines.slice(-12).join('\n')
      } },
    { name: 'perf_fix', description: 'Set a fix\'s state (found, working, fixed, verified), its commit and a short note.',
      parameters: { type: 'object', properties: { id: { type: 'string' }, status: { type: 'string', enum: ['found', 'working', 'fixed', 'verified'] }, commit: { type: 'string' }, note: { type: 'string' } }, required: ['id'] },
      async execute({ id, status, commit, note }) {
        const st = await load()
        if (!st.fixes[id]) return 'no fix ' + id + '; there are: ' + FIXES.map((f) => f.id).join(', ')
        if (status) st.fixes[id].status = status
        if (commit !== undefined) st.fixes[id].commit = commit
        if (note !== undefined) st.fixes[id].note = note
        await save(st)
        await push()
        return `${id}: ${st.fixes[id].status}`
      } },
    { name: 'perf_record', description: 'Record a number by hand (measured another way): as its before or after.',
      parameters: { type: 'object', properties: { metric: { type: 'string' }, value: { type: 'number' }, as: { type: 'string', enum: ['before', 'after'] }, note: { type: 'string' } }, required: ['metric', 'value', 'as'] },
      async execute({ metric, value, as, note }) {
        if (!METRICS[metric]) return 'no metric ' + metric + '; there are: ' + Object.keys(METRICS).join(', ')
        const st = await load()
        const m = (st.metrics[metric] = st.metrics[metric] || { history: [] })
        const e = { value, at: now(), source: note || 'recorded by hand', note }
        m.history.push(e)
        m[as] = e
        await save(st)
        await push()
        return `${metric}: ${as} ${value}`
      } }
  ]
}
