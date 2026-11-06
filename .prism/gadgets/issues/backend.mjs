// Issues kept on this computer: one Markdown file each in <project>/.prism/issues, a git repository of its own with no
// remote. Every change (a new issue, a note, labels, resolving or reopening) is a commit there, so the history is
// git's and nothing is ever pushed anywhere. The project's own repository ignores the folder (.prism/* in .gitignore).
import { execFile } from 'node:child_process'
import { promises as fs } from 'node:fs'
import path from 'node:path'

let ROOT = ''
let DIR = ''

function git(args, cwd = DIR) {
  return new Promise((resolve) => {
    execFile('git', args, { cwd, maxBuffer: 8 << 20, windowsHide: true }, (err, stdout, stderr) =>
      resolve({ ok: !err, out: String(stdout || ''), err: String(stderr || (err && err.message) || '') }))
  })
}

async function ensure() {
  await fs.mkdir(DIR, { recursive: true })
  try {
    await fs.access(path.join(DIR, '.git'))
  } catch {
    await git(['init', '-q'])
    await git(['config', 'user.name', 'Issues'])
    await git(['config', 'user.email', 'issues@localhost'])
    await fs.writeFile(path.join(DIR, 'README.md'), '# Issues\n\nThis project\'s issues, one file each, kept on this computer only (a git repository with no remote: never pushed).\nThe Issues gadget in Prism reads and writes them.\n')
    await commit('The issue list starts', 'Issues')
  }
}

async function commit(message, by) {
  await git(['add', '-A'])
  const who = (by || 'Someone').replace(/[<>]/g, '')
  return git(['-c', `user.name=${who}`, '-c', 'user.email=issues@localhost', 'commit', '-q', '-m', message])
}

const slug = (s) => s.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 50) || 'issue'
const now = () => new Date().toISOString().replace(/\.\d+Z$/, 'Z')

function parse(text, file) {
  const m = /^---\n([\s\S]*?)\n---\n?([\s\S]*)$/.exec(text.replace(/\r\n/g, '\n'))
  const meta = {}
  let body = text
  if (m) {
    for (const line of m[1].split('\n')) {
      const i = line.indexOf(':')
      if (i > 0) meta[line.slice(0, i).trim()] = line.slice(i + 1).trim()
    }
    body = m[2]
  }
  const notesAt = body.indexOf('\n## Notes\n')
  const text_ = (notesAt >= 0 ? body.slice(0, notesAt) : body).trim()
  const notes = []
  if (notesAt >= 0) {
    const parts = body.slice(notesAt + 10).split(/\n### /)
    for (const p of parts) {
      const s = p.replace(/^### /, '')
      const nl = s.indexOf('\n')
      if (nl < 0 || !s.trim()) continue
      const head = s.slice(0, nl)
      const [when, who] = head.split(' · ')
      notes.push({ when: (when || '').trim(), by: (who || '').trim(), text: s.slice(nl + 1).trim() })
    }
  }
  return {
    id: Number(meta.id) || 0,
    file,
    title: meta.title || file,
    status: meta.status === 'resolved' ? 'resolved' : 'open',
    labels: (meta.labels || '').split(',').map((s) => s.trim()).filter(Boolean),
    created: meta.created || '',
    by: meta.by || '',
    resolved: meta.resolved || '',
    fixed_by: meta.fixed_by || '',
    text: text_,
    notes
  }
}

function render(i) {
  const meta = [
    `id: ${i.id}`,
    `title: ${i.title.replace(/\n/g, ' ')}`,
    `status: ${i.status}`,
    `labels: ${i.labels.join(', ')}`,
    `created: ${i.created}`,
    `by: ${i.by}`
  ]
  if (i.resolved) meta.push(`resolved: ${i.resolved}`)
  if (i.fixed_by) meta.push(`fixed_by: ${i.fixed_by}`)
  let s = `---\n${meta.join('\n')}\n---\n\n${i.text.trim()}\n`
  if (i.notes.length) {
    s += '\n## Notes\n'
    for (const n of i.notes) s += `\n### ${n.when} · ${n.by}\n${n.text.trim()}\n`
  }
  return s
}

async function all() {
  await ensure()
  const out = []
  for (const f of await fs.readdir(DIR)) {
    if (!/^\d{4}-.*\.md$/.test(f)) continue
    try {
      out.push(parse(await fs.readFile(path.join(DIR, f), 'utf8'), f))
    } catch {}
  }
  return out.sort((a, b) => b.id - a.id)
}

async function find(id) {
  const i = (await all()).find((x) => x.id === Number(id))
  if (!i) throw new Error(`There is no issue ${id}.`)
  return i
}

async function save(i, message, by) {
  await fs.writeFile(path.join(DIR, i.file), render(i))
  await commit(message, by)
}

let emitter = null
const changed = (id) => emitter && emitter.emit('changed', { id })

// ---- what the UI and the agent do ----

async function list({ status, label, q } = {}) {
  let xs = await all()
  if (status && status !== 'all') xs = xs.filter((i) => i.status === status)
  if (label) xs = xs.filter((i) => i.labels.includes(label))
  if (q) {
    const t = q.toLowerCase()
    xs = xs.filter((i) => (i.title + '\n' + i.text + '\n' + i.notes.map((n) => n.text).join('\n')).toLowerCase().includes(t))
  }
  return xs.map(({ text, notes, ...rest }) => ({ ...rest, notes: notes.length }))
}

async function create({ title, text, labels, by }) {
  if (!title || !title.trim()) throw new Error('An issue needs a title.')
  const xs = await all()
  const id = xs.reduce((m, i) => Math.max(m, i.id), 0) + 1
  const i = {
    id, file: `${String(id).padStart(4, '0')}-${slug(title)}.md`, title: title.trim(), status: 'open',
    labels: (Array.isArray(labels) ? labels : String(labels || '').split(',')).map((s) => String(s).trim()).filter(Boolean),
    created: now(), by: by || 'Valerie', resolved: '', fixed_by: '', text: text || '', notes: []
  }
  await save(i, `Issue ${id} filed: ${i.title}`, i.by)
  changed(id)
  return i
}

async function note({ id, text, by }) {
  if (!text || !text.trim()) throw new Error('A note needs some words.')
  const i = await find(id)
  i.notes.push({ when: now(), by: by || 'Valerie', text })
  await save(i, `Issue ${id}: a note`, by || 'Valerie')
  changed(i.id)
  return i
}

async function setLabels({ id, labels, by }) {
  const i = await find(id)
  i.labels = (Array.isArray(labels) ? labels : String(labels || '').split(',')).map((s) => String(s).trim()).filter(Boolean)
  await save(i, `Issue ${id}: labels ${i.labels.join(', ') || 'none'}`, by || 'Valerie')
  changed(i.id)
  return i
}

async function resolve({ id, commit: hash, text, by }) {
  const i = await find(id)
  i.status = 'resolved'
  i.resolved = now()
  if (hash) {
    const r = await git(['rev-parse', '--verify', '--short=10', `${hash}^{commit}`], ROOT)
    if (!r.ok) throw new Error(`There is no commit ${hash} in the project.`)
    i.fixed_by = r.out.trim()
  }
  if (text && text.trim()) i.notes.push({ when: now(), by: by || 'Valerie', text })
  await save(i, `Issue ${id} resolved${i.fixed_by ? ' by ' + i.fixed_by : ''}`, by || 'Valerie')
  changed(i.id)
  return i
}

async function reopen({ id, text, by }) {
  const i = await find(id)
  i.status = 'open'
  i.resolved = ''
  if (text && text.trim()) i.notes.push({ when: now(), by: by || 'Valerie', text })
  await save(i, `Issue ${id} reopened`, by || 'Valerie')
  changed(i.id)
  return i
}

// what happened to it: its own history, and the project's commits that name it or fixed it
async function details({ id }) {
  const i = await find(id)
  const h = await git(['log', '--format=%h\t%an\t%aI\t%s', '--', i.file])
  const history = h.out.trim() ? h.out.trim().split('\n').map((l) => { const [hash, by, when, what] = l.split('\t'); return { hash, by, when, what } }) : []
  const c = await git(['log', '-i', '-E', `--grep=issue #?${i.id}\b`, '--format=%h\t%aI\t%s', '-20'], ROOT)
  const commits = c.out.trim() ? c.out.trim().split('\n').map((l) => { const [hash, when, what] = l.split('\t'); return { hash, when, what } }) : []
  if (i.fixed_by) {
    const f = await git(['log', '-1', '--format=%h\t%aI\t%s', i.fixed_by], ROOT)
    if (f.ok && f.out.trim() && !commits.some((x) => i.fixed_by.startsWith(x.hash) || x.hash.startsWith(i.fixed_by))) {
      const [hash, when, what] = f.out.trim().split('\t')
      commits.unshift({ hash, when, what, fixed: true })
    } else {
      for (const x of commits) if (i.fixed_by.startsWith(x.hash) || x.hash.startsWith(i.fixed_by)) x.fixed = true
    }
  }
  return { ...i, history, commits }
}

async function recentCommits() {
  const r = await git(['log', '--format=%h\t%aI\t%s', '-15'], ROOT)
  return r.out.trim() ? r.out.trim().split('\n').map((l) => { const [hash, when, what] = l.split('\t'); return { hash, when, what } }) : []
}

async function labels() {
  const s = new Set(['bug', 'idea', 'kernel', 'drivers', 'desktop', 'companion', 'facet', 'docs', 'tests'])
  for (const i of await all()) i.labels.forEach((l) => s.add(l))
  return [...s].sort()
}

const text = (o) => JSON.stringify(o, null, 2)

export default {
  async start(api) {
    ROOT = api.root
    DIR = path.join(ROOT, '.prism', 'issues')
    emitter = api
    await ensure()
  },
  async stop() {},
  handlers: {
    list: (a) => list(a),
    details: (a) => details(a),
    create: (a) => create(a),
    note: (a) => note(a),
    setLabels: (a) => setLabels(a),
    resolve: (a) => resolve(a),
    reopen: (a) => reopen(a),
    recentCommits: () => recentCommits(),
    labels: () => labels()
  },
  tools: [
    { name: 'issue_list', description: 'The project\'s issues (local only): status open, resolved or all; optional label and search words.',
      parameters: { type: 'object', properties: { status: { type: 'string', enum: ['open', 'resolved', 'all'] }, label: { type: 'string' }, q: { type: 'string' } } },
      async execute(a) { return text(await list(a)) } },
    { name: 'issue_get', description: 'One issue in full: its text, notes, history and the project commits that name it or fixed it.',
      parameters: { type: 'object', properties: { id: { type: 'number' } }, required: ['id'] },
      async execute(a) { return text(await details(a)) } },
    { name: 'issue_create', description: 'File an issue. Give a plain title, and in text what goes wrong, how to see it, and the exact file:line where it is. labels: e.g. bug, kernel, desktop, companion, facet, docs, tests.',
      parameters: { type: 'object', properties: { title: { type: 'string' }, text: { type: 'string' }, labels: { type: 'array', items: { type: 'string' } }, by: { type: 'string', description: 'who files it, e.g. Claude' } }, required: ['title', 'text'] },
      async execute(a) { const i = await create({ by: 'Claude', ...a }); return `Issue ${i.id} filed: ${i.title}` } },
    { name: 'issue_note', description: 'Add a note to an issue (what you found, tried or changed).',
      parameters: { type: 'object', properties: { id: { type: 'number' }, text: { type: 'string' }, by: { type: 'string' } }, required: ['id', 'text'] },
      async execute(a) { await note({ by: 'Claude', ...a }); return `Note added to issue ${a.id}.` } },
    { name: 'issue_resolve', description: 'Mark an issue resolved, with the project commit that fixed it (its hash) and a short note.',
      parameters: { type: 'object', properties: { id: { type: 'number' }, commit: { type: 'string' }, text: { type: 'string' }, by: { type: 'string' } }, required: ['id'] },
      async execute(a) { const i = await resolve({ by: 'Claude', ...a }); return `Issue ${i.id} resolved${i.fixed_by ? ' by ' + i.fixed_by : ''}.` } },
    { name: 'issue_reopen', description: 'Open a resolved issue again, saying why.',
      parameters: { type: 'object', properties: { id: { type: 'number' }, text: { type: 'string' }, by: { type: 'string' } }, required: ['id'] },
      async execute(a) { await reopen({ by: 'Claude', ...a }); return `Issue ${a.id} is open again.` } }
  ]
}
