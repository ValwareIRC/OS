// Reproduces "the VM dies when the cursor is over its window while it boots"
// on the Windows QEMU (WHPX), without touching the real mouse: QEMU is started
// as scripts\os-win.ps1 starts it, and mouse movement is injected through its
// QMP socket for the whole boot. Reports how far the boot got.
//   node tests/win_mouse_boot.mjs [ISO] [abs|rel|none] [SECONDS]
import { spawn } from 'node:child_process'
import net from 'node:net'
import fs from 'node:fs'
import path from 'node:path'

const root = path.resolve(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), '..')
const iso = process.argv[2] || path.join(root, 'build', 'os.iso')
const mode = process.argv[3] || 'abs'
const seconds = Number(process.argv[4] || 90)
const log = path.join(root, 'build', `win-mouse-${mode}.log`)
fs.rmSync(log, { force: true })
const port = 4590
const qemu = 'C:\\Program Files\\qemu\\qemu-system-x86_64.exe'
const args = ['-name', 'OS mouse test', '-M', 'q35,vmport=on', '-m', '8G', '-smp', '4',
  '-accel', 'whpx,kernel-irqchip=on', '-accel', 'tcg,thread=multi', '-cpu', 'Skylake-Client-noTSX-IBRS',
  '-vga', 'std', '-display', 'none', '-netdev', 'user,id=n0', '-device', 'e1000,netdev=n0',
  '-serial', 'file:' + log.replace(/\\/g, '/'), '-qmp', `tcp:127.0.0.1:${port},server=on,wait=off`,
  '-cdrom', iso]
const vm = spawn(qemu, args, { stdio: 'ignore' })
let exited = null
vm.on('exit', (code) => { exited = code })

const sleep = (ms) => new Promise((r) => setTimeout(r, ms))
await sleep(1500)
let sock = null
for (let i = 0; i < 20 && !sock; i++) {
  try {
    sock = await new Promise((res, rej) => { const s = net.connect(port, '127.0.0.1', () => res(s)); s.on('error', rej) })
  } catch { await sleep(500) }
}
if (!sock) { console.log('no QMP'); vm.kill(); process.exit(1) }
sock.on('error', () => {})
sock.write(JSON.stringify({ execute: 'qmp_capabilities' }) + '\n')

const t0 = Date.now()
let n = 0, x = 0
const stats = { lastLine: '', input: null, bootOk: null }
const timer = setInterval(() => {
  if (mode === 'none' || exited !== null) return
  x = (x + 1) % 400
  const ev = mode === 'abs'
    ? [{ type: 'abs', data: { axis: 'x', value: 4000 + x * 50 } }, { type: 'abs', data: { axis: 'y', value: 8000 + x * 30 } }]
    : [{ type: 'rel', data: { axis: 'x', value: x % 2 ? 3 : -3 } }, { type: 'rel', data: { axis: 'y', value: 2 } }]
  sock.write(JSON.stringify({ execute: 'input-send-event', arguments: { events: ev } }) + '\n')
  n++
}, 4)

while (Date.now() - t0 < seconds * 1000 && exited === null) {
  await sleep(1000)
  const text = fs.existsSync(log) ? fs.readFileSync(log, 'utf8') : ''
  const lines = text.trim().split('\n')
  stats.lastLine = lines[lines.length - 1] || ''
  const t = ((Date.now() - t0) / 1000).toFixed(0)
  if (!stats.input && text.includes('input: ')) stats.input = t
  if (!stats.bootOk && text.includes('OS BOOT OK')) stats.bootOk = t
  if (stats.bootOk) break
}
clearInterval(timer)
console.log(`mode ${mode}: ${n} mouse events; input set up at ${stats.input ?? 'NEVER'} s; boot ok at ${stats.bootOk ?? 'NEVER'} s;` +
  ` qemu ${exited === null ? 'still running' : 'exited ' + exited}; last line: ${stats.lastLine.slice(0, 100)}`)
try { sock.write(JSON.stringify({ execute: 'quit' }) + '\n') } catch {}
await sleep(1000)
try { vm.kill() } catch {}
process.exit(0)
