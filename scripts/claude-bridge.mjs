// Lets the VM think with Claude through this PC's own Claude Code sign-in
// (the OAuth session), with no API key anywhere.
//
// It answers OpenAI-style requests on 127.0.0.1:4570 (the VM reaches it as
// http://10.0.2.2:4570/v1) and hands each one to `claude -p` with every
// Claude Code tool turned off, so Claude cannot run anything on this PC.
// An app's own tools (OpenAI function calling: Prism's agent) work: Claude
// asks for them, and they run in the app, in the VM (see "tools" below).
// Customisations (CLAUDE.md, hooks, MCP servers) are skipped too.
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

// ---- tools ----
// The app's tools (OpenAI function calling) run where the app is, in the VM: never here. Claude Code's own
// tools stay off (--tools ''), so Claude can't run anything on this PC; it is told the app's tools and asks
// for one with a <tool_call> block, which goes back to the app as an OpenAI tool call. The app's results come
// back in the next request as role "tool" messages, and ride along in the transcript.
function toolPrompt(tools, choice) {
  const list = tools.map(t => t.function || t).map(f =>
    `- ${f.name}: ${f.description || ''}\n  parameters (JSON schema): ${JSON.stringify(f.parameters || {})}`).join('\n');
  let must = '';
  if (choice === 'required') must = '\nYou must use a tool in this answer.';
  else if (choice && typeof choice === 'object' && choice.function?.name) must = `\nYou must use the tool ${choice.function.name} in this answer.`;
  return `You can use tools. They run in the app that asked (on the person's computer), not here, and their results come back to you.
To use one, end your answer with one or more blocks, each exactly:
<tool_call>{"name": "TOOL_NAME", "arguments": {ARGUMENTS AS JSON}}</tool_call>
Write nothing after them. Each result comes back to you in a <tool_result> block, and then you go on. When you need no tool, answer normally, with no such block.${must}
The tools:
${list}`;
}

let callNo = 0;
// Claude's answer, its <tool_call> blocks taken out as OpenAI tool calls.
function toolCalls(text) {
  const calls = [];
  const rest = text.replace(/<tool_call>\s*([\s\S]*?)\s*<\/tool_call>/g, (all, json) => {
    try {
      const c = JSON.parse(json);
      if (c && typeof c.name === 'string') {
        calls.push({ id: `call_${Date.now().toString(36)}_${++callNo}`, type: 'function',
          function: { name: c.name, arguments: JSON.stringify(c.arguments ?? {}) } });
        return '';
      }
    } catch { /* not a call after all: left in the text */ }
    return all;
  }).trim();
  return { calls, rest };
}

// One message in the transcript: its words, the tools it asked for, or a tool's result.
function line(m) {
  if (m.role === 'tool')
    return `<tool_result id="${m.tool_call_id || ''}"${m.name ? ` name="${m.name}"` : ''}>\n${textOf(m.content)}\n</tool_result>`;
  const who = m.role === 'assistant' ? 'you' : 'them';
  const calls = (m.tool_calls || []).map(c =>
    `<tool_call>${JSON.stringify({ name: c.function?.name, arguments: (() => { try { return JSON.parse(c.function?.arguments || '{}'); } catch { return c.function?.arguments; } })() })}</tool_call>`);
  return `<${who}>\n${[textOf(m.content), ...calls].filter(Boolean).join('\n')}\n</${who}>`;
}

// `claude -p` takes one new user turn, so earlier turns ride along in it as a
// transcript, and the last user message (with any pictures) closes it; what
// came after it (the tools it used for that message, and their results) follows.
function turn(messages, tools, choice) {
  let system = messages.filter(m => m.role === 'system').map(m => textOf(m.content)).join('\n\n');
  if (tools?.length && choice !== 'none') system = (system ? system + '\n\n' : '') + toolPrompt(tools, choice);
  const talk = messages.filter(m => m.role !== 'system');
  let lastUser = talk.length - 1;
  while (lastUser >= 0 && talk[lastUser].role !== 'user') lastUser--;
  const before = talk.slice(0, Math.max(lastUser, 0)), after = lastUser >= 0 ? talk.slice(lastUser + 1) : talk;
  const content = [];
  if (before.length)
    content.push({ type: 'text', text: `The conversation so far:\n${before.map(line).join('\n')}\n\nTheir new message follows. Answer it as your next turn.` });
  content.push(...(lastUser >= 0 ? blocks(talk[lastUser].content) : [{ type: 'text', text: '(no message)' }]));
  if (after.length)
    content.push({ type: 'text', text: `\nYour work on it so far:\n${after.map(line).join('\n')}\n\nGo on from here.` });
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
    const tools = Array.isArray(q.tools) && q.tools.length && q.tool_choice !== 'none' ? q.tools : null;
    const { system, content } = turn(q.messages || [], tools, q.tool_choice);
    const t0 = Date.now();
    if (tools) {
      // With tools the whole answer is waited for (its tool calls are found in it), then sent: streamed, as
      // OpenAI streams tool calls, or whole.
      try {
        const r = await ask(model, system, content);
        const { calls, rest } = toolCalls(r.result || '');
        const finish = calls.length ? 'tool_calls' : r.stop_reason === 'max_tokens' ? 'length' : 'stop';
        log(`${model}: ${Date.now() - t0} ms, ${r.usage?.output_tokens ?? '?'} tokens out, ${calls.length} tool call(s)`);
        if (q.stream) {
          res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache' });
          const chunk = (delta, f = null) => res.write('data: ' + JSON.stringify({
            id: 'bridge-' + t0, object: 'chat.completion.chunk', created: Math.floor(t0 / 1000), model,
            choices: [{ index: 0, delta, finish_reason: f }],
          }) + '\n\n');
          chunk({ role: 'assistant', content: rest || '' });
          calls.forEach((c, index) => chunk({ tool_calls: [{ index, ...c }] }));
          chunk({}, finish);
          return res.end('data: [DONE]\n\n');
        }
        return send(res, 200, {
          id: 'bridge-' + t0, object: 'chat.completion', created: Math.floor(t0 / 1000), model,
          choices: [{ index: 0, message: { role: 'assistant', content: rest || null, ...(calls.length ? { tool_calls: calls } : {}) },
            finish_reason: finish }],
          usage: { prompt_tokens: r.usage?.input_tokens ?? 0, completion_tokens: r.usage?.output_tokens ?? 0 },
        });
      } catch (e) {
        log(`${model}: failed: ${e.message}`);
        return send(res, 502, { error: { message: 'Claude on this PC: ' + e.message } });
      }
    }
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
