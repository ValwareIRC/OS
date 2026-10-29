// Lets the VM think with Claude through this PC's own Claude Code sign-in
// (the OAuth session), with no API key anywhere.
//
// It answers OpenAI-style requests on 127.0.0.1:4570 (the VM reaches it as
// http://10.0.2.2:4570/v1) and hands each one to `claude -p` with every
// tool turned off, so Claude can only talk: it cannot run anything on this
// PC. Customisations (CLAUDE.md, hooks, MCP servers) are skipped too.
//
//   node scripts/claude-bridge.mjs [--port 4570]
//
// Models: "opus", "sonnet", "haiku", "fable" or a full claude-* id; anything
// else gets the default (CLAUDE_BRIDGE_MODEL, else sonnet).
import http from 'node:http';
import { spawn } from 'node:child_process';
import { existsSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { homedir, tmpdir } from 'node:os';
import { join } from 'node:path';

// The installed executable itself (no shell in between to mangle arguments).
const claudeExe = [join(homedir(), '.local', 'bin', 'claude.exe'), join(homedir(), '.local', 'bin', 'claude')]
  .find(existsSync) || 'claude';

const port = Number(process.argv[process.argv.indexOf('--port') + 1]) || 4570;
const fallbackModel = process.env.CLAUDE_BRIDGE_MODEL || 'sonnet';
const MODELS = ['opus', 'sonnet', 'haiku', 'fable'];
// An empty folder to run in, so no project's files or settings are picked up.
const quietDir = mkdtempSync(join(tmpdir(), 'claude-bridge-'));
const log = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);

function pickModel(m) {
  if (typeof m !== 'string') return fallbackModel;
  const s = m.toLowerCase();
  if (s.startsWith('claude-')) return m;
  return MODELS.find(x => s.includes(x)) || fallbackModel;
}

// OpenAI content (a string, or parts with text and image_url) as Claude blocks.
function blocks(content) {
  if (typeof content === 'string') return [{ type: 'text', text: content }];
  const out = [];
  for (const p of content || []) {
    if (p.type === 'text') out.push({ type: 'text', text: p.text });
    else if (p.type === 'image_url') {
      const url = typeof p.image_url === 'string' ? p.image_url : p.image_url?.url;
      const m = /^data:(image\/[a-z]+);base64,(.*)$/s.exec(url || '');
      if (m) out.push({ type: 'image', source: { type: 'base64', media_type: m[1], data: m[2] } });
    }
  }
  return out;
}

const textOf = c => blocks(c).filter(b => b.type === 'text').map(b => b.text).join('\n');

// `claude -p` takes one new user turn, so earlier turns ride along in it as a
// transcript, and the last user message (with any pictures) closes it.
function turn(messages) {
  const system = messages.filter(m => m.role === 'system').map(m => textOf(m.content)).join('\n\n');
  const talk = messages.filter(m => m.role !== 'system');
  let lastUser = talk.length - 1;
  while (lastUser >= 0 && talk[lastUser].role !== 'user') lastUser--;
  const before = talk.slice(0, Math.max(lastUser, 0));
  const content = [];
  if (before.length) {
    const lines = before.map(m => `<${m.role === 'assistant' ? 'you' : 'them'}>\n${textOf(m.content)}\n</${m.role === 'assistant' ? 'you' : 'them'}>`);
    content.push({ type: 'text', text: `The conversation so far:\n${lines.join('\n')}\n\nTheir new message follows. Answer it as your next turn.` });
  }
  content.push(...(lastUser >= 0 ? blocks(talk[lastUser].content) : [{ type: 'text', text: '(no message)' }]));
  return { system, content };
}

let asked = 0;
// onText, when given, receives each piece of the reply as Claude writes it.
function ask(model, system, content, onText) {
  return new Promise((resolve, reject) => {
    const args = ['-p', '--input-format', 'stream-json', '--output-format', 'stream-json', '--verbose',
      '--tools', '', '--strict-mcp-config', '--safe-mode', '--no-session-persistence', '--model', model];
    if (onText) args.push('--include-partial-messages');
    // Long prompts go in a file: no command line is long enough for them.
    const promptFile = join(quietDir, `system-${process.pid}-${++asked}.txt`);
    if (system) {
      writeFileSync(promptFile, system);
      args.push('--system-prompt-file', promptFile);
    }
    // Long thinking made each new design take five or six minutes; a modest
    // budget keeps them to a minute or two (CLAUDE_BRIDGE_THINKING overrides).
    const env = { ...process.env, MAX_THINKING_TOKENS: process.env.CLAUDE_BRIDGE_THINKING || '3000' };
    const child = spawn(claudeExe, args, { cwd: quietDir, windowsHide: true, env });
    let out = '', err = '', pending = '';
    child.stdout.on('data', d => {
      out += d;
      if (!onText) return;
      pending += d;
      const lines = pending.split('\n');
      pending = lines.pop();
      for (const l of lines) {
        let e; try { e = JSON.parse(l); } catch { continue; }
        const ev = e?.type === 'stream_event' ? e.event : null;
        if (ev?.type === 'content_block_delta' && ev.delta?.type === 'text_delta' && ev.delta.text) onText(ev.delta.text);
      }
    });
    child.stderr.on('data', d => { err += d; });
    child.on('error', reject);
    child.on('close', code => {
      rmSync(promptFile, { force: true });
      const result = out.split('\n').filter(Boolean).map(l => { try { return JSON.parse(l); } catch { return null; } })
        .find(e => e && e.type === 'result');
      if (result && !result.is_error) resolve(result);
      else reject(new Error(result?.result || err.trim().slice(-400) || `claude exited with ${code}`));
    });
    child.stdin.end(JSON.stringify({ type: 'user', message: { role: 'user', content } }) + '\n');
  });
}

function send(res, status, body) {
  const s = JSON.stringify(body);
  res.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(s) });
  res.end(s);
}

// --exit-with-vm: leave once the Windows QEMU has been gone a minute (the
// launcher starts the bridge alongside the VM; nothing should linger after).
if (process.argv.includes('--exit-with-vm')) {
  const { execFile } = await import('node:child_process');
  let missing = 0;
  setInterval(() => execFile('tasklist', ['/FI', 'IMAGENAME eq qemu-system-x86_64.exe', '/NH'], (e, out) => {
    missing = /qemu-system/i.test(out || '') ? 0 : missing + 1;
    if (missing >= 4) { log('the VM is gone; stopping'); process.exit(0); }
  }), 15000);
}

http.createServer((req, res) => {
  if (req.method === 'GET' && /\/models$/.test(req.url)) {
    return send(res, 200, { object: 'list', data: MODELS.map(id => ({ id, object: 'model', owned_by: 'anthropic' })) });
  }
  if (req.method !== 'POST' || !/\/chat\/completions$/.test(req.url)) return send(res, 404, { error: { message: 'Not here.' } });
  let body = '';
  req.on('data', d => { body += d; });
  req.on('end', async () => {
    let q;
    try { q = JSON.parse(body); } catch { return send(res, 400, { error: { message: 'That was not JSON.' } }); }
    const model = pickModel(q.model);
    const { system, content } = turn(q.messages || []);
    const t0 = Date.now();
    if (q.stream) {
      // Server-sent events, as OpenAI streams them: the VM shows designs
      // being written instead of waiting minutes for the whole reply.
      res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
      const chunk = (delta, finish = null) => res.write('data: ' + JSON.stringify({
        id: 'bridge-' + t0, object: 'chat.completion.chunk', created: Math.floor(t0 / 1000), model,
        choices: [{ index: 0, delta, finish_reason: finish }],
      }) + '\n\n');
      chunk({ role: 'assistant', content: '' });
      try {
        const r = await ask(model, system, content, text => chunk({ content: text }));
        log(`${model}: ${Date.now() - t0} ms, ${r.usage?.output_tokens ?? '?'} tokens out (streamed)`);
        chunk({}, r.stop_reason === 'max_tokens' ? 'length' : 'stop');
      } catch (e) {
        log(`${model}: failed: ${e.message}`);
        res.write('data: ' + JSON.stringify({ error: { message: 'Claude on this PC: ' + e.message } }) + '\n\n');
      }
      res.end('data: [DONE]\n\n');
      return;
    }
    try {
      const r = await ask(model, system, content);
      log(`${model}: ${Date.now() - t0} ms, ${r.usage?.output_tokens ?? '?'} tokens out`);
      send(res, 200, {
        id: 'bridge-' + t0, object: 'chat.completion', created: Math.floor(t0 / 1000), model,
        choices: [{ index: 0, message: { role: 'assistant', content: r.result }, finish_reason: r.stop_reason === 'max_tokens' ? 'length' : 'stop' }],
        usage: { prompt_tokens: r.usage?.input_tokens ?? 0, completion_tokens: r.usage?.output_tokens ?? 0 },
      });
    } catch (e) {
      log(`${model}: failed: ${e.message}`);
      send(res, 502, { error: { message: 'Claude on this PC: ' + e.message } });
    }
  });
}).listen(port, '127.0.0.1', () => log(`Claude bridge on 127.0.0.1:${port} (the VM: http://10.0.2.2:${port}/v1)`));
