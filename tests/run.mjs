// End-to-end test: real remodroid-server + remodroid-agent, a fake adbd/phone,
// and a viewer speaking the WebSocket + REST API (plan Appendix A).
//   node tests/run.mjs build/host
import { execFile, execFileSync, spawn } from 'node:child_process';
import crypto from 'node:crypto';
import dgram from 'node:dgram';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { FakePhone, T, tunBuild } from './fakes.mjs';

const BIN = path.resolve(process.argv[2] || 'build/host');
const VERBOSE = !!process.env.VERBOSE;
// QEMU=/path/to/qemu-mipsel runs cross-built binaries under user-mode emulation.
const PRE = process.env.QEMU ? [process.env.QEMU] : [];
const exe = (name) => [...PRE, path.join(BIN, name)];
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'remodroid-test-'));
const procs = [];
let failures = 0, passes = 0;

function ok(cond, name) {
  if (cond) { passes++; console.log('  ✓ ' + name); }
  else { failures++; console.log('  ✗ ' + name); }
}
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(fn, ms = 5000, what = 'condition') {
  const end = Date.now() + ms;
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() > end) throw new Error('timeout: ' + what);
    await sleep(50);
  }
}
function freePort() {
  return new Promise(r => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); });
}
function start(name, args) {
  const p = spawn(args[0], args.slice(1), { stdio: ['ignore', 'pipe', 'pipe'] });
  p.log = '';
  const onData = d => { p.log += d; if (VERBOSE) process.stderr.write(`[${name}] ${d}`); };
  p.stdout.on('data', onData); p.stderr.on('data', onData);
  procs.push(p);
  return p;
}

// ---- minimal WebSocket client (so the test needs nothing but Node) ----
function wsConnect(port, pathname, headers = {}) {
  return new Promise((resolve, reject) => {
    const sock = net.connect(port, '127.0.0.1');
    const key = crypto.randomBytes(16).toString('base64');
    let buf = Buffer.alloc(0), open = false;
    const ws = { msgs: [], waiters: [], closed: false,
      send(obj) {
        const p = Buffer.from(JSON.stringify(obj));
        const mask = crypto.randomBytes(4);
        let h;
        if (p.length < 126) { h = Buffer.from([0x81, 0x80 | p.length]); }
        else { h = Buffer.alloc(4); h[0] = 0x81; h[1] = 0x80 | 126; h.writeUInt16BE(p.length, 2); }
        const m = Buffer.from(p.map((b, i) => b ^ mask[i & 3]));
        sock.write(Buffer.concat([h, mask, m]));
      },
      wait(pred, ms = 5000, what = 'message') {
        const hit = ws.msgs.find(pred);
        if (hit) return Promise.resolve(hit);
        return new Promise((res, rej) => {
          const w = { pred, res };
          ws.waiters.push(w);
          setTimeout(() => rej(new Error('timeout waiting for ' + what)), ms);
        });
      },
      close() { sock.end(); },
    };
    sock.on('connect', () => {
      const extra = Object.entries(headers).map(([k, v]) => `${k}: ${v}\r\n`).join('');
      sock.write(`GET ${pathname} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
        `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n${extra}\r\n`);
    });
    sock.on('data', d => {
      buf = Buffer.concat([buf, d]);
      if (!open) {
        const i = buf.indexOf('\r\n\r\n');
        if (i < 0) return;
        const head = buf.subarray(0, i).toString();
        buf = buf.subarray(i + 4);
        const status = parseInt(head.split(' ')[1], 10);
        if (status !== 101) { sock.destroy(); reject(Object.assign(new Error('HTTP ' + status), { status })); return; }
        open = true;
        resolve(ws);
      }
      while (buf.length >= 2) {
        let len = buf[1] & 127, off = 2;
        if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
        if (buf.length < off + len) return;
        const op = buf[0] & 15, payload = buf.subarray(off, off + len);
        buf = buf.subarray(off + len);
        if (op === 1) {
          const m = JSON.parse(payload.toString());
          ws.msgs.push(m);
          for (const w of [...ws.waiters]) if (w.pred(m)) { w.res(m); ws.waiters.splice(ws.waiters.indexOf(w), 1); }
        } else if (op === 9) {
          const mask = crypto.randomBytes(4);
          sock.write(Buffer.concat([Buffer.from([0x8a, 0x80 | payload.length]), mask, Buffer.from(payload.map((b, i) => b ^ mask[i & 3]))]));
        }
      }
    });
    sock.on('close', () => { ws.closed = true; });
    sock.on('error', e => { if (!open) reject(e); });
  });
}

async function main() {
  const httpPort = await freePort();
  const hash = pw => { const [cmd, ...pre] = exe('remodroid-server'); return execFileSync(cmd, [...pre, 'hashpw', pw]).toString().trim(); };
  fs.mkdirSync(path.join(dir, 'web'));
  fs.writeFileSync(path.join(dir, 'web', 'index.html'), '<!doctype html><title>RemoDroid</title>');
  fs.writeFileSync(path.join(dir, 'server.ini'), `[http]
listen = 127.0.0.1:${httpPort}
web_root = ${dir}/web
[auth]
session_days = 30
max_failed_logins = 5
lockout_minutes = 1
[users]
me = ${hash('pw-user')}
[devices]
router1 = ${hash('pw-device')}
[device_names]
router1 = Living room phone
[turn]
urls = turn:127.0.0.1:3478?transport=udp
shared_secret = turnsecret
credential_ttl_minutes = 60
[keepalive]
ping_seconds = 2
timeout_seconds = 6
heartbeat_seconds = 2
`);

  // Files the agent pushes to the phone.
  fs.mkdirSync(path.join(dir, 'files'));
  fs.writeFileSync(path.join(dir, 'files', 'remodroid-server.jar'), crypto.randomBytes(300 * 1024));
  fs.writeFileSync(path.join(dir, 'files', 'libremodroid.so'), crypto.randomBytes(60 * 1024)); // fallback build
  fs.mkdirSync(path.join(dir, 'files', 'arm64-v8a'));
  fs.writeFileSync(path.join(dir, 'files', 'arm64-v8a', 'libremodroid.so'), crypto.randomBytes(70 * 1024));

  const keyPath = path.join(dir, 'adbkey');
  const phone = new FakePhone({ keyPath, log: s => VERBOSE && console.log('[phone] ' + s) });
  const adbPort = await phone.listen();
  const agentIni = path.join(dir, 'agent.ini');
  fs.writeFileSync(agentIni, `[server]
url = ws://127.0.0.1:${httpPort}/ws/agent
device_id = router1
device_secret = pw-device

[keepalive]
ping_seconds = 2
timeout_seconds = 6
heartbeat_seconds = 2

[agent]
files_dir = ${dir}/files
adbkey = ${keyPath}
socket = ${dir}/agent.sock
log_level = ${VERBOSE ? 'debug' : 'info'}

[phone]
adb = tcp:127.0.0.1:${adbPort}
screen_off_on_connect = true   ; turn the panel off
lock_grace_seconds = 10

[video]
codec = auto
`);

  start('server', [...exe('remodroid-server'), '-c', path.join(dir, 'server.ini')]);
  await until(() => fetch(`http://127.0.0.1:${httpPort}/`).then(() => true, () => false), 5000, 'server up');
  const base = `http://127.0.0.1:${httpPort}`;

  console.log('server: auth and REST');
  let r = await fetch(base + '/');
  ok(r.status === 401 && /Basic realm/.test(r.headers.get('www-authenticate')), 'unauthenticated page gets a Basic challenge');
  r = await fetch(base + '/', { headers: { Authorization: 'Basic ' + Buffer.from('me:pw-user').toString('base64') } });
  const setCookie = r.headers.get('set-cookie') || '';
  const cookie = setCookie.split(';')[0];
  ok(r.status === 200 && /HttpOnly/.test(setCookie) && /SameSite=Strict/.test(setCookie) && /Max-Age=2592000/.test(setCookie),
    'Basic login sets an HttpOnly, SameSite=Strict, 30-day cookie');
  r = await fetch(base + '/api/v1/devices');
  ok(r.status === 401, 'API without cookie is 401');
  r = await fetch(base + '/api/v1/me', { headers: { Cookie: cookie } });
  const me = await r.json();
  ok(me.user === 'me' && me.session_expires > Date.now() + 29 * 864e5, '/me returns the user and a 30-day expiry');
  let devs = await (await fetch(base + '/api/v1/devices', { headers: { Cookie: cookie } })).json();
  ok(devs.devices[0].id === 'router1' && devs.devices[0].name === 'Living room phone' && !devs.devices[0].online,
    'device list shows router1 offline');
  r = await fetch(base + '/api/v1/devices/router1/cmd', { method: 'POST', headers: { Cookie: cookie }, body: '{"cmd":"phone.wake"}' });
  ok(r.status === 409 && (await r.json()).error === 'offline', 'command to an offline device is 409 offline');
  await wsConnect(httpPort, '/ws/agent', { Authorization: 'Basic ' + Buffer.from('router1:wrong').toString('base64') })
    .then(() => ok(false, 'bad device secret rejected'), e => ok(e.status === 401, 'bad device secret rejected with 401'));
  await wsConnect(httpPort, '/ws/viewer', { Cookie: cookie, Origin: 'http://evil.example' })
    .then(() => ok(false, 'foreign Origin rejected'), e => ok(e.status === 403, 'viewer WebSocket from a foreign Origin is 403'));

  const viewer = await wsConnect(httpPort, '/ws/viewer', { Cookie: cookie, Origin: `http://127.0.0.1:${httpPort}` });
  const snap = await viewer.wait(m => m.t === 'devices');
  ok(snap.d.devices.length === 1, 'viewer gets a devices snapshot');

  console.log('agent: ADB auth, push, server start, tunnel');
  start('agent', [...exe('remodroid-agent'), '-c', agentIni]);
  await phone.waitFor(e => e.kind === 'auth.pubkey', 30000, 'public key (first run generates the RSA key)');
  ok(phone.events.find(e => e.kind === 'auth.signature')?.data === true, 'agent signs the auth token correctly (RSA PKCS#1 v1.5, SHA-1)');
  ok(phone.events.find(e => e.kind === 'auth.pubkey')?.data === true, 'agent sends a valid Android RSAPublicKey (n0inv, R², modulus)');
  await viewer.wait(m => m.t === 'device.event' && m.d.kind === 'adb.unauthorized', 5000, 'adb.unauthorized event');
  ok(true, 'viewer sees adb.unauthorized while the phone shows the prompt');
  await viewer.wait(m => m.t === 'device.event' && m.d.kind === 'adb.authorized', 5000, 'adb.authorized');
  ok(true, 'viewer sees adb.authorized after "Allow"');
  await phone.waitFor(e => e.kind === 'server.start', 5000, 'server start');
  const jar = phone.files.get('/data/local/tmp/remodroid/remodroid-server.jar');
  ok(jar && jar.data.equals(fs.readFileSync(path.join(dir, 'files', 'remodroid-server.jar'))), 'jar pushed byte-for-byte (300 KB over sync:)');
  ok(phone.files.get('/data/local/tmp/remodroid/libremodroid.so')?.mode === 0o100755, 'native library pushed with mode 0755');
  ok(phone.files.get('/data/local/tmp/remodroid/libremodroid.so')?.data.length === 70 * 1024, 'the library matching the phone ABI (arm64-v8a) is pushed');
  ok(/CLASSPATH=\/data\/local\/tmp\/remodroid\/remodroid-server.jar exec app_process/.test(phone.serverCmd), 'server started with app_process');
  await phone.waitFor(e => e.kind === 'tunnel.open', 5000, 'tunnel');
  const cfg = await phone.waitFor(e => e.kind === 'ctrl' && e.data.t === 'config', 5000, 'config');
  ok(cfg.data.d.phone.screen_off_on_connect === true && cfg.data.d.phone.lock_grace_seconds === 10,
    'phone receives typed config from agent.ini');
  const upd = await viewer.wait(m => m.t === 'device.update' && m.d.patch.online && m.d.patch.phone.server === 'running'
    && m.d.patch.phone.battery, 6000, 'device running');
  ok(upd.d.patch.phone.model === 'FakePhone' && upd.d.patch.phone.android === '14', 'device shows model and Android version');
  ok(upd.d.patch.phone.screen === 'off' && upd.d.patch.phone.battery.level === 82, 'phone status (screen, battery) reaches the viewer');

  r = await fetch(base + '/api/v1/devices/router1/adb', { headers: { Cookie: cookie } });
  const adbst = await r.json();
  ok(adbst.auth === 'authorized' && adbst.features.includes('shell_v2') && adbst.max_payload === 262144, 'REST /adb shows ADB status');

  console.log('session: signaling, UDP relay');
  viewer.send({ t: 'session.request', id: 1, d: { device: 'router1', quality: { mode: 'auto', codec: 'auto' } } });
  const ack = await viewer.wait(m => m.t === 'ack' && m.id === 1);
  const sid = ack.d.sid;
  ok(/^s_[0-9a-f]{16}$/.test(sid), 'session.request returns a sid');
  const ice = await viewer.wait(m => m.t === 'session.ice');
  const turn = ice.d.ice_servers[0];
  const expectCred = crypto.createHmac('sha1', 'turnsecret').update(turn.username).digest('base64');
  ok(turn.credential === expectCred && /^\d+:me$/.test(turn.username), 'TURN credentials use the coturn REST format');
  const st = await phone.waitFor(e => e.kind === 'ctrl' && e.data.t === 'session.start');
  ok(st.data.sid === sid && st.data.d.quality.mode === 'auto', 'phone receives session.start with quality');
  const offer = await viewer.wait(m => m.t === 'signal' && m.d.type === 'offer');
  ok(offer.sid === sid && offer.d.sdp === 'v=0 fake-offer', 'phone offer reaches the viewer');
  viewer.send({ t: 'signal', d: { type: 'answer', sdp: 'v=0 fake-answer' } });
  const ans = await phone.waitFor(e => e.kind === 'ctrl' && e.data.t === 'signal' && e.data.d.type === 'answer');
  ok(ans.data.sid === sid, 'viewer answer reaches the phone');
  const second = await wsConnect(httpPort, '/ws/viewer', { Cookie: cookie });
  second.send({ t: 'session.request', id: 3, d: { device: 'router1' } });
  const busy = await second.wait(m => m.t === 'error' && m.id === 3);
  ok(busy.d.code === 'busy', 'a second viewer gets busy');
  second.close();

  // UDP relay: phone -> router -> echo server -> router -> phone
  const echo = dgram.createSocket('udp4');
  echo.on('message', (msg, rinfo) => echo.send(Buffer.concat([Buffer.from('echo:'), msg]), rinfo.port, rinfo.address));
  await new Promise(res => echo.bind(0, '127.0.0.1', res));
  phone.tunnel.send(tunBuild(T.UDP_OPEN, 7, 4, [0, 0, 0, 0], 0)); // af 4 + bind address 0.0.0.0:0
  await sleep(100);
  phone.tunnel.send(tunBuild(T.UDP_SEND, 7, 4, [127, 0, 0, 1], echo.address().port, Buffer.from('rtp-packet')));
  const udp = await phone.waitFor(e => e.kind === 'udp.recv', 3000, 'udp echo');
  ok(udp.data.payload === 'echo:rtp-packet' && udp.data.sock === 7 && udp.data.port === echo.address().port,
    'UDP relay round-trip through the agent');
  echo.close();

  console.log('commands, config, diag');
  r = await fetch(base + '/api/v1/devices/router1/cmd', { method: 'POST', headers: { Cookie: cookie }, body: '{"cmd":"phone.wake"}' });
  ok(r.status === 200, 'REST phone.wake is acknowledged');
  await phone.waitFor(e => e.kind === 'ctrl' && e.data.t === 'cmd' && e.data.d.cmd === 'phone.wake');
  ok(true, 'phone receives the wake command');
  r = await fetch(base + '/api/v1/devices/router1/cmd', { method: 'POST', headers: { Cookie: cookie },
    body: JSON.stringify({ cmd: 'agent.config', args: { screenOff: false, grace: 25, 'files.upload_dir': '/sdcard/Pictures/RD' } }) });
  ok(r.status === 200, 'agent.config is acknowledged');
  const ini = fs.readFileSync(agentIni, 'utf8');
  ok(/screen_off_on_connect = false   ; turn the panel off/.test(ini), 'agent.ini value replaced, inline comment kept');
  ok(/lock_grace_seconds = 25/.test(ini) && /\[files\]\nupload_dir = \/sdcard\/Pictures\/RD/.test(ini), 'new keys and sections added');
  await phone.waitFor(e => e.kind === 'ctrl' && e.data.t === 'config' && e.data.d.phone.lock_grace_seconds === 25);
  ok(true, 'phone gets the updated config');
  r = await fetch(base + '/api/v1/devices/router1/cmd', { method: 'POST', headers: { Cookie: cookie }, body: '{"cmd":"rm -rf"}' });
  ok(r.status === 400, 'unknown command is rejected');
  r = await fetch(base + '/api/v1/devices/router1/diag?lines=50', { headers: { Cookie: cookie } });
  const diag = await r.json();
  ok(diag.result.agent_log.some(l => /connected to the server/.test(l)) && diag.result.server_stderr.includes('remodroid server starting'),
    'diag returns agent log and phone server output');

  console.log('CLI');
  // Async on purpose: the fake phone lives in this process and must keep answering.
  const cli = (...a) => new Promise(res => execFile(exe('remodroid-agent')[0], [...exe('remodroid-agent').slice(1), '-c', agentIni, ...a], { timeout: 20000 },
    (e, out, err) => res({ out: out + (e ? err : ''), code: e ? e.code : 0 })));
  let c = await cli('shell', 'echo', 'hello phone');
  ok(c.code === 0 && c.out === 'hello phone\n', 'CLI shell prints output');
  c = await cli('shell', 'exit', '3');
  ok(c.code === 3, 'CLI shell returns the exit code (shell v2)');
  fs.writeFileSync(path.join(dir, 'note.txt'), 'hi from router');
  c = await cli('push', path.join(dir, 'note.txt'), '/sdcard/Download/');
  ok(c.code === 0 && phone.files.get('/sdcard/Download/note.txt')?.data.toString() === 'hi from router', 'CLI push into a directory');
  c = await cli('pull', '/sdcard/Download/note.txt', path.join(dir, 'back.txt'));
  ok(c.code === 0 && fs.readFileSync(path.join(dir, 'back.txt'), 'utf8') === 'hi from router', 'CLI pull');
  c = await cli('status');
  ok(c.code === 0 && JSON.parse(c.out).server_connected === true, 'CLI status');

  console.log('lifecycle');
  viewer.close();
  const end = await phone.waitFor(e => e.kind === 'ctrl' && e.data.t === 'session.end', 3000, 'session.end');
  ok(end.data.d.reason === 'viewer_left', 'closing the viewer ends the session on the phone');
  r = await fetch(base + '/api/v1/devices/router1/cmd', { method: 'POST', headers: { Cookie: cookie }, body: '{"cmd":"server.restart"}' });
  ok(r.status === 200, 'server.restart acknowledged');
  await phone.waitFor(e => e.kind === 'server.stop', 3000, 'old server stopped');
  phone.events = phone.events.filter(e => e.kind !== 'server.start' && e.kind !== 'tunnel.open');
  phone.readySent = false;
  await phone.waitFor(e => e.kind === 'tunnel.open', 6000, 'tunnel reopened');
  ok(true, 'phone server restarts and the tunnel reopens');
  phone.dropConnection();
  const v2 = await wsConnect(httpPort, '/ws/viewer', { Cookie: cookie });
  await v2.wait(m => m.t === 'device.update' && m.d.patch.phone.attached === false, 5000, 'detached');
  ok(true, 'unplugging the phone is reported (phone.attached = false)');
  await phone.waitFor(e => e.kind === 'adb.closed');
  phone.events = phone.events.filter(e => !['server.start', 'tunnel.open'].includes(e.kind));
  phone.readySent = false;
  await phone.waitFor(e => e.kind === 'tunnel.open', 10000, 'reconnect');
  ok(true, 'agent reconnects to the phone and restarts the server');
  r = await fetch(base + '/api/v1/logout', { method: 'POST', headers: { Cookie: cookie } });
  ok(r.status === 204, 'logout');
  r = await fetch(base + '/api/v1/me', { headers: { Cookie: cookie } });
  ok(r.status === 401, 'cookie is invalid after logout');
  v2.close();
}

const timer = setTimeout(() => { console.log('  ✗ overall timeout'); failures++; finish(); }, 90000);
function finish() {
  clearTimeout(timer);
  for (const p of procs) p.kill('SIGTERM');
  if (failures) for (const p of procs) console.log('---- log ----\n' + p.log.slice(-4000));
  console.log(`\n${passes} passed, ${failures} failed`);
  if (!failures) fs.rmSync(dir, { recursive: true, force: true });
  process.exit(failures ? 1 : 0);
}
main().then(finish, e => { console.log('  ✗ ' + e.message); failures++; finish(); });
