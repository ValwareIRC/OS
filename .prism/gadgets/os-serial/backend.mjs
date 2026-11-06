import fs from 'node:fs'
import path from 'node:path'

// Follows build/os-vm-serial.log (written by QEMU when "Operating System.cmd" runs) and
// sorts every line into a kind, so a boot can be watched without grep.

const MAX_LINES = 3000
let api
let file = ''
let offset = 0
let partial = ''
let timer = null
let boots = []          // { n, started, lines: [{ t, kind, text }] }
let filter = 'all'
let waiters = []        // { re, resolve, timer }

const KINDS = [
  ['error', /\bpanic\b|page fault|\bexception\b|ai-error|sent no text|empty reply|ui problem|design markup|\bfailed\b|\berror\b/i],
  ['timeout', /timed? ?out|timeout/i],
  ['ai', /^shell: (ai |mind |design|ui command|ui action)|^ai: /],
  ['screen', /^shell: screen |OS BOOT OK|(Operating System|Lumen) kernel starting|net: eth0|boot parameters|preloaded/],
]

function kindOf(text) {
  // what the AI said is never itself an error, whatever words it uses
  if (/^shell: (ai |mind |ui command|ui action)/.test(text) || /^shell: ai$/.test(text.trim())) return 'ai'
  for (const [k, re] of KINDS) if (re.test(text)) return k
  return 'other'
}

function current() {
  if (!boots.length) boots.push({ n: 1, started: Date.now(), lines: [] })
  return boots[boots.length - 1]
}

function newBoot() {
  const n = boots.length ? boots[boots.length - 1].n + 1 : 1
  boots.push({ n, started: Date.now(), lines: [] })
  if (boots.length > 6) boots.shift()
  api.emit('boot', summary())
}

function addLine(raw) {
  const text = raw.replace(/\r$/, '')
  if (!text.trim()) return
  if (/^(Operating System|Lumen) kernel starting/.test(text) && current().lines.length > 5) newBoot()
  const line = { t: Date.now(), kind: kindOf(text), text }
  const b = current()
  b.lines.push(line)
  if (b.lines.length > MAX_LINES) b.lines.splice(0, b.lines.length - MAX_LINES)
  api.emit('line', line)
  for (const w of [...waiters]) if (w.re.test(text)) { clearTimeout(w.timer); waiters = waiters.filter(x => x !== w); w.resolve(text) }
}

function poll() {
  let st
  try { st = fs.statSync(file) } catch { return }
  if (st.size < offset) { // QEMU recreated the file: a new boot
    offset = 0
    partial = ''
    newBoot()
  }
  if (st.size === offset) return
  const fd = fs.openSync(file, 'r')
  try {
    const len = Math.min(st.size - offset, 4 * 1024 * 1024)
    const buf = Buffer.alloc(len)
    fs.readSync(fd, buf, 0, len, offset)
    offset += len
    const chunk = partial + buf.toString('utf8')
    const parts = chunk.split('\n')
    partial = parts.pop()
    for (const p of parts) addLine(p)
  } finally { fs.closeSync(fd) }
}

function summary() {
  const b = current()
  const count = k => b.lines.filter(l => l.kind === k).length
  const screens = b.lines.filter(l => /^shell: screen /.test(l.text)).map(l => l.text.slice(14))
  const replies = b.lines.filter(l => /^shell: ai /.test(l.text))
  const empty = replies.filter(l => l.text.trim() === 'shell: ai').length
  const errors = b.lines.filter(l => l.kind === 'error')
  let size = 0
  try { size = fs.statSync(file).size } catch {}
  return {
    file, exists: fs.existsSync(file), size, boot: b.n, started: b.started, lines: b.lines.length,
    counts: { ai: count('ai'), error: count('error'), timeout: count('timeout'), screen: count('screen') },
    screens, replies: replies.length, empty, lastError: errors.length ? errors[errors.length - 1].text : '',
    lastScreen: screens.length ? screens[screens.length - 1] : '', filter,
  }
}

function pick(kind, n, grep) {
  let lines = current().lines
  if (kind && kind !== 'all') lines = lines.filter(l => l.kind === kind)
  if (grep) { const re = new RegExp(grep, 'i'); lines = lines.filter(l => re.test(l.text)) }
  return lines.slice(-n)
}

function start(path_) {
  file = path_
  offset = 0
  partial = ''
  boots = []
  poll()
}

async function restartVm() {
  const root = api.root || 'C:\\Users\\Valerie\\OS'
  await api.shell('powershell -NoProfile -Command "Stop-Process -Name qemu-system-x86_64 -Force -ErrorAction SilentlyContinue"', { timeoutMs: 15000 })
  await api.shell(`powershell -NoProfile -Command "Start-Process -FilePath '${path.join(root, 'Operating System.cmd')}' -WorkingDirectory '${root}'"`, { timeoutMs: 15000 })
  return 'restarting'
}

export default {
  async start(a) {
    api = a
    filter = (await api.state.get('filter', 'all')) || 'all'
    const saved = await api.state.get('file', '')
    start(saved || path.join(api.root || 'C:/Users/Valerie/OS', 'build', 'os-vm-serial.log'))
    timer = setInterval(poll, 400)
  },
  async stop() { clearInterval(timer) },
  handlers: {
    async state() { return { ...summary(), recent: pick('all', 800) } },
    async setFilter({ filter: f }) { filter = f || 'all'; await api.state.set('filter', filter); api.emit('filter', filter); return filter },
    async setFile({ file: f }) { await api.state.set('file', f); start(f); api.emit('boot', summary()); return summary() },
    async clear() { boots = [{ n: (current().n || 0) + 1, started: Date.now(), lines: [] }]; api.emit('boot', summary()); return true },
    async restart() { return restartVm() },
  },
  tools: [
    {
      name: 'serial_summary',
      description: 'What happened in the current boot: screens reached, AI replies (and how many were empty), counts of errors/timeouts, the last error.',
      parameters: { type: 'object', properties: {} },
      async execute() { return JSON.stringify(summary(), null, 2) },
    },
    {
      name: 'serial_tail',
      description: 'Recent lines of the current boot. kind: all | ai | error | timeout | screen | other. grep: optional regex.',
      parameters: { type: 'object', properties: { kind: { type: 'string' }, n: { type: 'number' }, grep: { type: 'string' } } },
      async execute({ kind = 'all', n = 60, grep = '' }) {
        const lines = pick(kind, Math.min(n, 500), grep)
        return lines.length ? lines.map(l => `[${l.kind}] ${l.text}`).join('\n') : '(no matching lines)'
      },
    },
    {
      name: 'serial_wait',
      description: 'Wait until a new serial line matches a regex (e.g. "shell: ai" for the next AI reply), up to timeoutSec. Returns the line or says it timed out.',
      parameters: { type: 'object', properties: { pattern: { type: 'string' }, timeoutSec: { type: 'number' } }, required: ['pattern'] },
      async execute({ pattern, timeoutSec = 120 }) {
        const re = new RegExp(pattern)
        const line = await new Promise(resolve => {
          const w = { re, resolve, timer: setTimeout(() => { waiters = waiters.filter(x => x !== w); resolve(null) }, Math.min(timeoutSec, 900) * 1000) }
          waiters.push(w)
        })
        return line ? line : `No line matched /${pattern}/ within ${timeoutSec}s.`
      },
    },
    {
      name: 'serial_restart',
      description: 'Stop the VM and start it again with Operating System.cmd (a new boot section begins).',
      parameters: { type: 'object', properties: {} },
      async execute() { return restartVm() },
    },
  ],
}
