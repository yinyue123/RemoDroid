// End-to-end test of the Cloudflare Worker version (worker/) under `wrangler dev`, with the
// real remodroid-agent: setup wizard API, login, enrollment, agent online, viewer WebSocket,
// signaling to the agent, commands, rename and remove.
//   (cd worker && npm install) && node tests/worker.mjs build/host
import { spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = path.resolve(process.argv[2] || 'build/host');
const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const WRANGLER = process.env.WRANGLER || path.join(ROOT, 'worker/node_modules/.bin/wrangler');
const VERBOSE = !!process.env.VERBOSE;
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'remodroid-worker-'));
const procs = [];
let failures = 0, passes = 0;
const ok = (c, name) => { if (c) { passes++; console.log('  ✓ ' + name); } else { failures++; console.log('  ✗ ' + name); } };
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(fn, ms = 8000, what = 'condition') {
  const end = Date.now() + ms;
  for (;;) {
    const v = await fn().catch(() => null);
    if (v) return v;
    if (Date.now() > end) throw new Error('timeout: ' + what);
    await sleep(150);
  }
}
function freePort() { return new Promise(r => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); }); }
function start(name, cmd, args, opts = {}) {
  const file = path.join(dir, name + '.log'), fd = fs.openSync(file, 'w');
  const p = spawn(cmd, args, { stdio: ['ignore', fd, fd], ...opts });
  fs.closeSync(fd);
  Object.defineProperty(p, 'log', { get: () => fs.readFileSync(file, 'utf8') });
  p.name = name;
  procs.push(p);
  return p;
}
function run(cmd, args) {
  return new Promise(res => { const p = spawn(cmd, args); let out = ''; p.stdout.on('data', d => out += d); p.stderr.on('data', d => out += d); p.on('close', code => res({ code, out })); });
}

function wsConnect(port, pathname, headers = {}) {
  return new Promise((resolve, reject) => {
    const sock = net.connect(port, '127.0.0.1');
    let buf = Buffer.alloc(0), open = false;
    const ws = { msgs: [], closed: false,
      send(obj) {
        const p = Buffer.from(JSON.stringify(obj)), mask = crypto.randomBytes(4);
        const h = p.length < 126 ? Buffer.from([0x81, 0x80 | p.length]) : Buffer.from([0x81, 0x80 | 126, p.length >> 8, p.length & 255]);
        sock.write(Buffer.concat([h, mask, Buffer.from(p.map((b, i) => b ^ mask[i & 3]))]));
      },
      wait(pred, ms = 8000, what = 'message') { return until(async () => ws.msgs.find(pred), ms, what); },
      close() { sock.end(); },
    };
    sock.on('connect', () => {
      const extra = Object.entries(headers).map(([k, v]) => `${k}: ${v}\r\n`).join('');
      sock.write(`GET ${pathname} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
        `Sec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\nSec-WebSocket-Version: 13\r\n${extra}\r\n`);
    });
    sock.on('data', d => {
      buf = Buffer.concat([buf, d]);
      if (!open) {
        const i = buf.indexOf('\r\n\r\n');
        if (i < 0) return;
        const status = parseInt(buf.subarray(0, i).toString().split(' ')[1], 10);
        buf = buf.subarray(i + 4);
        if (status !== 101) { sock.destroy(); reject(Object.assign(new Error('HTTP ' + status), { status })); return; }
        open = true;
        resolve(ws);
      }
      while (buf.length >= 2) {
        let len = buf[1] & 127, off = 2;
        if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
        if (buf.length < off + len) return;
        if ((buf[0] & 15) === 1) ws.msgs.push(JSON.parse(buf.subarray(off, off + len).toString()));
        buf = buf.subarray(off + len);
      }
    });
    sock.on('close', () => { ws.closed = true; });
    sock.on('error', e => { if (!open) reject(e); });
  });
}

async function main() {
  const port = await freePort();
  const B = `http://127.0.0.1:${port}`;
  const wr = start('wrangler', WRANGLER, ['dev', '--port', String(port), '--ip', '127.0.0.1', '--persist-to', path.join(dir, 'state')],
    { cwd: path.join(ROOT, 'worker'), env: { ...process.env, WRANGLER_SEND_METRICS: 'false', CI: '1' } });
  await until(() => fetch(B + '/wizard.html').then(r => r.status < 500), 60000, 'wrangler dev');

  console.log('setup');
  let r = await fetch(B + '/', { redirect: 'manual' });
  ok(r.status === 302 && /\/wizard\.html$/.test(r.headers.get('location')), 'a fresh Worker sends / to the setup wizard');
  r = await fetch(B + '/api/v1/devices');
  ok(r.status === 503, 'the API is closed until setup is done');
  let j = await (await fetch(B + '/api/v1/setup')).json();
  ok(j.platform === 'worker' && j.configured === false, 'GET /api/v1/setup reports a Worker that is not set up');
  r = await fetch(B + '/api/v1/setup', { method: 'POST', body: JSON.stringify({ username: 'admin', password: 'short', turn: { cloudflare: false }, ice_extra: 'stun:stun.cloudflare.com:3478' }) });
  ok(r.status === 400, 'a short password is refused');
  r = await fetch(B + '/api/v1/setup', { method: 'POST', body: JSON.stringify({ username: 'admin', password: 'correct-horse', turn: { cloudflare: false }, ice_extra: '' }) });
  ok(r.status === 400, 'no TURN at all is refused');
  r = await fetch(B + '/api/v1/setup', { method: 'POST', body: JSON.stringify({ username: 'admin', password: 'correct-horse', turn: { cloudflare: false },
    ice_extra: 'stun:stun.cloudflare.com:3478\nturn:turn.example.com:3478?transport=udp alice s3cret' }) });
  ok(r.status === 200, 'setup saves the admin account and ICE servers');

  console.log('login');
  r = await fetch(B + '/', { redirect: 'manual' });
  ok(r.status === 302 && r.headers.get('location') === '/login', 'pages send you to the login page after setup');
  r = await fetch(B + '/?device=r1', { redirect: 'manual' });
  ok(r.headers.get('location') === '/login?next=/%3Fdevice%3Dr1', 'the login page is told where to come back to');
  r = await fetch(B + '/login');
  ok(r.status === 200 && /id="password"/.test(await r.text()), 'the login page is served without a cookie');
  const login = pw => fetch(B + '/login', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ user: 'admin', password: pw }) });
  r = await login('correct-horse');
  const cookie = (r.headers.get('set-cookie') || '').split(';')[0];
  ok(r.status === 200 && /^rd_session=[0-9a-f]{32}$/.test(cookie), 'login sets the cookie');
  const H = { Cookie: cookie };
  r = await fetch(B + '/', { headers: H });
  ok(r.status === 200 && /<html/i.test(await r.text()), 'the panel page is served with the cookie');
  j = await (await fetch(B + '/api/v1/setup', { headers: H })).json();
  ok(j.configured && j.username === 'admin' && /alice/.test(j.ice_extra), 'settings can be read back after login');

  console.log('settings (the panel\'s Server tab)');
  r = await fetch(B + '/api/v1/settings', { headers: H });
  let txt = await r.text();
  j = JSON.parse(txt);
  ok(r.status === 200 && j.platform === 'worker' && j.ice_servers.length === 2 && j.ice_servers[1].has_credential && !/s3cret/.test(txt),
    'the STUN/TURN list comes back without its passwords');
  const PATCH = body => fetch(B + '/api/v1/settings', { method: 'PATCH', headers: H, body: JSON.stringify(body) });
  r = await PATCH({ ice_servers: [{ url: 'stun:stun.cloudflare.com:3478' },
    { url: 'turn:turn.example.com:3479?transport=udp', username: 'alice', was: 'turn:turn.example.com:3478?transport=udp' }, { url: 'stun:203.0.113.10:3478' }] });
  j = await (await fetch(B + '/api/v1/setup', { headers: H })).json();
  ok(r.status === 200 && j.ice_extra === 'stun:stun.cloudflare.com:3478, turn:turn.example.com:3479?transport=udp alice s3cret, stun:203.0.113.10:3478',
    'editing one server keeps its password; adding one needs just the address');
  r = await PATCH({ ice_servers: [] });
  ok(r.status === 400, 'the last STUN/TURN server can\'t go without Cloudflare TURN');
  r = await PATCH({ current_password: 'wrong-one', password: 'new-password-1' });
  ok(r.status === 403, 'a new password needs the current one');
  r = await PATCH({ current_password: 'correct-horse', password: 'new-password-1' });
  ok(r.status === 200 && (await login('new-password-1')).status === 200 && (await login('correct-horse')).status === 401, 'the password changes, the session stays');
  r = await PATCH({ current_password: 'new-password-1', password: 'correct-horse' });
  ok(r.status === 200, 'and back');

  console.log('enroll');
  const code = (await (await fetch(B + '/api/v1/enroll/codes', { method: 'POST', headers: H, body: '{"name":"Living room"}' })).json()).code;
  ok(/^[2-9A-Z]{4}-[2-9A-Z]{4}$/.test(code), 'the Agents tab gets a one-time code');
  const ini = path.join(dir, 'agent.ini');
  fs.writeFileSync(ini, `[agent]\nadbkey = ${dir}/adbkey\nsocket = ${dir}/agent.sock\nlog_level = debug\n[phone]\nadb = tcp:127.0.0.1:1\n`);
  const agent = start('agent', path.join(BIN, 'remodroid-agent'), ['-c', ini]);
  await sleep(800);
  const en = await run(path.join(BIN, 'remodroid-agent'), ['-c', ini, 'enroll', '--panel', B, '--code', code.toLowerCase()]);
  ok(en.code === 0 && /enrolled as "Living room" \(living-room\)/.test(en.out) && /bound — this router is online/.test(en.out), 'remodroid enroll binds the router and it comes online');
  const again = await run(path.join(BIN, 'remodroid-agent'), ['-c', path.join(dir, 'other.ini'), 'enroll', '--panel', B, '--code', code]);
  ok(again.code === 1 && /used or expired/.test(again.out), 'a code works only once');
  j = await (await fetch(B + `/api/v1/enroll/codes/${code}`, { headers: H })).json();
  ok(j.state === 'used' && j.device === 'living-room' && j.online === true, 'the page sees the code used and the router online');
  j = await (await fetch(B + '/api/v1/devices', { headers: H })).json();
  ok(j.devices.length === 1 && j.devices[0].online && j.devices[0].agent.arch, 'the router is listed online with its agent info');

  console.log('viewer and signaling');
  const viewer = await wsConnect(port, '/ws/viewer', { ...H, Origin: B });
  const snap = await viewer.wait(m => m.t === 'devices');
  ok(snap.d.devices[0].id === 'living-room', 'the viewer WebSocket gets the device list');
  await wsConnect(port, '/ws/viewer', { ...H, Origin: 'http://evil.example' }).then(() => ok(false, 'foreign origin refused'), e => ok(e.status === 403, 'a viewer WebSocket from another site is refused'));
  viewer.send({ t: 'session.request', id: 1, d: { device: 'living-room', quality: { mode: 'auto' } } });
  const ack = await viewer.wait(m => m.t === 'ack' && m.id === 1);
  const ice = await viewer.wait(m => m.t === 'session.ice');
  ok(/^s_[0-9a-f]{16}$/.test(ack.d.sid) && ice.d.ice_servers.some(s => s.username === 'alice' && s.credential === 's3cret'),
    'session.request gets a sid and the configured ICE servers');
  const ended = await viewer.wait(m => m.t === 'session.ended', 8000, 'session.ended');
  ok(ended.d.reason === 'phone_unavailable', 'session.start reaches the agent, which answers (no phone attached)');

  console.log('commands and management');
  r = await fetch(B + '/api/v1/devices/living-room/cmd', { method: 'POST', headers: H, body: '{"cmd":"agent.config.get"}' });
  j = await r.json();
  ok(r.status === 200 && j.result && typeof j.result === 'object', 'commands are forwarded to the agent and answered');
  r = await fetch(B + '/api/v1/devices/living-room/cmd', { method: 'POST', headers: H, body: JSON.stringify({ cmd: 'agent.config', args: { codec: 'h264' } }) });
  ok(r.status === 200 && /codec = h264/.test(fs.readFileSync(ini, 'utf8')), 'settings from the panel are written to agent.ini');
  r = await fetch(B + '/api/v1/devices/living-room', { method: 'PATCH', headers: H, body: '{"name":"Bedroom"}' });
  ok(r.status === 200 && (await r.json()).name === 'Bedroom', 'rename');
  r = await fetch(B + '/api/v1/devices/living-room', { method: 'DELETE', headers: H });
  ok(r.status === 204, 'remove');
  await viewer.wait(m => m.t === 'device.removed' && m.d.id === 'living-room', 5000, 'device.removed');
  ok(true, 'viewers hear about the removal');
  await until(async () => /unauthorized|rejected|HTTP 40[13]/.test(agent.log), 10000, 'agent refused');
  ok(true, 'the removed router can no longer log in');
  viewer.close();
}

const timer = setTimeout(() => { console.log('  ✗ overall timeout'); failures++; finish(); }, 150000);
function finish() {
  clearTimeout(timer);
  for (const p of procs) p.kill('SIGTERM');
  if (failures || VERBOSE) for (const p of procs) console.log(`---- ${p.name} ----\n` + p.log.slice(-3000));
  console.log(`\n${passes} passed, ${failures} failed`);
  if (!failures) fs.rmSync(dir, { recursive: true, force: true });
  process.exit(failures ? 1 : 0);
}
main().then(finish, e => { console.log('  ✗ ' + e.message); failures++; finish(); });
