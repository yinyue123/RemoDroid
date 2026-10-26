// remodroid-server on Windows (CI: windows-2022 and windows-11-arm): the console program under its
// supervisor, the wizard's restart on a new port, static files byte for byte, login, WebSocket,
// TURN, `settings` / `configure`, the supervisor stopping the server when it goes, and the Windows
// service (install, restart, stop, uninstall; needs an administrator, as CI is).
// Runs on Linux too, without the service part.
//   node tests/server_windows.mjs build/win-x86_64
import { spawn, spawnSync } from 'node:child_process';
import crypto from 'node:crypto';
import dgram from 'node:dgram';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const BIN = path.resolve(process.argv[2] || 'build/host');
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const WIN = process.platform === 'win32';
const EXE = path.join(BIN, 'remodroid-server' + (WIN ? '.exe' : ''));
const VERBOSE = !!process.env.VERBOSE;
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'remodroid-win-'));
const procs = [];
let failures = 0, passes = 0;
const ok = (c, name) => { if (c) { passes++; console.log('  ✓ ' + name); } else { failures++; console.log('  ✗ ' + name); } };
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(fn, ms = 10000, what = 'condition') {
  const end = Date.now() + ms;
  for (;;) {
    const v = await fn().catch(() => null);
    if (v) return v;
    if (Date.now() > end) throw new Error('timeout: ' + what);
    await sleep(150);
  }
}
const up = url => until(() => fetch(url, { redirect: 'manual' }).then(r => r.status > 0), 10000, url + ' up');
const down = url => until(() => fetch(url).then(() => false, () => true), 10000, url + ' down');
function freePort() { return new Promise(r => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); }); }
function start(name, args) {
  const file = path.join(dir, name + '.log'), fd = fs.openSync(file, 'w');
  const p = spawn(EXE, args, { stdio: ['pipe', fd, fd] }); /* stdin: a console's would be the terminal */
  fs.closeSync(fd);
  Object.defineProperty(p, 'log', { get: () => fs.readFileSync(file, 'utf8') });
  procs.push(p);
  return p;
}
const run = (args, input) => spawnSync(EXE, args, { input: input || '', encoding: 'utf8' });

/* the response's first line to a WebSocket upgrade */
function upgrade(port, pathname, headers) {
  return new Promise((resolve, reject) => {
    const s = net.connect(port, '127.0.0.1', () => {
      const h = Object.entries(headers).map(([k, v]) => `${k}: ${v}\r\n`).join('');
      s.write(`GET ${pathname} HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
        `Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ${crypto.randomBytes(16).toString('base64')}\r\n${h}\r\n`);
    });
    s.once('data', d => { resolve(d.toString().split('\r\n')[0]); s.destroy(); });
    s.on('error', reject);
  });
}

/* one TURN Allocate with long-term credentials (the 401 challenge, then the signed request): 200 or the error */
function turnAllocate(port, user, pass) {
  const sock = dgram.createSocket('udp4');
  const attr = (t, v) => { const h = Buffer.alloc(4); h.writeUInt16BE(t, 0); h.writeUInt16BE(v.length, 2); return Buffer.concat([h, v, Buffer.alloc((4 - v.length % 4) % 4)]); };
  const msg = (attrs, key) => {
    const tid = crypto.randomBytes(12), body = Buffer.concat(attrs);
    const head = len => { const h = Buffer.alloc(20); h.writeUInt16BE(0x0003, 0); h.writeUInt16BE(len, 2); h.writeUInt32BE(0x2112A442, 4); tid.copy(h, 8); return h; };
    if (!key) return Buffer.concat([head(body.length), body]);
    const mac = crypto.createHmac('sha1', key).update(Buffer.concat([head(body.length + 24), body])).digest();
    return Buffer.concat([head(body.length + 24), body, attr(0x0008, mac)]);
  };
  const read = b => { const out = {}; for (let o = 20; o + 4 <= b.length;) { const t = b.readUInt16BE(o), l = b.readUInt16BE(o + 2); out[t] = b.subarray(o + 4, o + 4 + l); o += 4 + l + ((4 - l % 4) % 4); } return { type: b.readUInt16BE(0), a: out }; };
  const code = r => r.type === 0x0103 ? 200 : r.a[0x0009] ? r.a[0x0009][2] * 100 + r.a[0x0009][3] : 0;
  const transport = attr(0x0019, Buffer.from([17, 0, 0, 0]));
  const ask = b => new Promise((res, rej) => { const t = setTimeout(() => rej(new Error('no TURN answer')), 3000); sock.once('message', m => { clearTimeout(t); res(read(m)); }); sock.send(b, port, '127.0.0.1'); });
  return ask(msg([transport])).then(r => {
    const realm = r.a[0x0014], nonce = r.a[0x0015];
    if (!realm || !nonce) return code(r);
    const key = crypto.createHash('md5').update(`${user}:${realm}:${pass}`).digest();
    return ask(msg([transport, attr(0x0006, Buffer.from(user)), attr(0x0014, realm), attr(0x0015, nonce)], key)).then(code);
  }).finally(() => sock.close());
}

async function main() {
  const p1 = await freePort(), p2 = await freePort(), tp = await freePort();
  const ini = path.join(dir, 'server.ini');
  fs.writeFileSync(ini, `[server]\nstate_dir = ${dir}/state\n[http]\nlisten = 127.0.0.1:${p1}\nweb_root = ${ROOT}/web\n`);
  const A = `http://127.0.0.1:${p1}`, B = `http://127.0.0.1:${p2}`;

  console.log('setup mode, under the supervisor');
  let server = start('server', ['-c', ini]);
  await up(A + '/wizard.html');
  const big = fs.readdirSync(path.join(ROOT, 'web')).map(f => path.join(ROOT, 'web', f))
    .filter(f => fs.statSync(f).isFile()).sort((a, b) => fs.statSync(b).size - fs.statSync(a).size)[0];
  let r = await fetch(A + '/' + path.basename(big));
  const body = Buffer.from(await r.arrayBuffer());
  ok(r.status === 200 && body.equals(fs.readFileSync(big)), `the largest web file comes through byte for byte (${path.basename(big)}, ${body.length} bytes)`);
  r = await fetch(A + '/..%5cserver.ini');
  ok(r.status === 400, 'paths with backslashes or .. are refused');
  r = await fetch(A + '/wizard.html::$DATA');
  ok(r.status === 400, 'paths with a colon (Windows streams, drives) are refused');

  const token = fs.readFileSync(path.join(dir, 'state', 'setup-token'), 'utf8').trim();
  r = await fetch(A + '/api/v1/setup', { method: 'POST', headers: { 'X-Setup-Token': token }, body: JSON.stringify({
    http_port: p2, username: 'admin', password: 'correct-horse', turn_enabled: true, turn_port: tp, public_ip: '127.0.0.1' }) });
  ok(r.status === 200, 'the wizard saves');
  await up(B + '/');
  ok(true, 'the server comes back on the new port (restarted by its supervisor on Windows)');
  ok(server.exitCode === null, 'the supervisor keeps running across the restart');

  console.log('the configured server');
  const login = (user, password) => fetch(B + '/login', { method: 'POST', headers: { 'Content-Type': 'application/json', Origin: B },
    body: JSON.stringify({ user, password }) });
  r = await login('admin', 'correct-horse');
  const cookie = (r.headers.get('set-cookie') || '').split(';')[0];
  ok(r.status === 200 && cookie, 'the admin logs in');
  ok((await upgrade(p2, '/ws/viewer', { Cookie: cookie, Origin: B })) === 'HTTP/1.1 101 Switching Protocols', 'a viewer WebSocket opens');
  ok(/^HTTP\/1\.1 403/.test(await upgrade(p2, '/ws/viewer', { Cookie: cookie, Origin: 'http://evil.example' })), 'one from another site does not');
  const secret = /shared_secret = (\S+)/.exec(fs.readFileSync(ini, 'utf8'))[1];
  const tuser = `${Math.floor(Date.now() / 1000) + 600}:admin`;
  ok(await turnAllocate(tp, tuser, crypto.createHmac('sha1', secret).update(tuser).digest('base64')) === 200, 'TURN allocates with the server\'s credentials');
  ok(await turnAllocate(tp, tuser, 'wrong') === 401, 'and refuses a wrong password');

  console.log('settings and configure');
  let out = run(['-c', ini, 'settings']);
  let j = JSON.parse(out.stdout);
  ok(out.status === 0 && j.configured && j.username === 'admin' && j.http_port === p2 && j.turn_enabled, '`settings` prints the wizard\'s settings');
  out = run(['-c', ini, 'configure'], JSON.stringify({ ...j, password: 'short' }));
  ok(out.status === 1 && /8 characters/.test(JSON.parse(out.stdout).error), '`configure` explains what is wrong');
  out = run(['-c', ini, 'configure'], JSON.stringify({ ...j, password: 'battery-staple' }));
  ok(out.status === 0 && JSON.parse(out.stdout).ok, '`configure` saves the form');

  console.log('stopping');
  server.kill(); /* Windows: ends the supervisor only; the server sees its stdin close */
  await down(B + '/');
  ok(true, 'the server stops when its supervisor goes');
  server = start('server2', ['-c', ini]);
  await up(B + '/');
  ok((await login('admin', 'battery-staple')).status === 200, 'after a restart the configured password works');
  server.kill();
  await down(B + '/');

  if (WIN) {
    console.log('Windows service');
    const data = path.join(process.env.ProgramData || 'C:\\ProgramData', 'RemoDroid');
    out = run(['-c', ini, 'install']);
    ok(out.status === 0, '`install` installs and starts the service' + (out.status ? `: ${out.stdout}${out.stderr}` : ''));
    await up(B + '/');
    ok((await login('admin', 'battery-staple')).status === 200, 'the service serves the panel');
    const log = () => fs.readFileSync(path.join(data, 'server.log'), 'utf8');
    ok(/service: started/.test(log()), 'the service logs to server.log in the data folder');
    const fw = spawnSync('netsh', ['advfirewall', 'firewall', 'show', 'rule', 'name=RemoDroid Server', 'verbose'], { encoding: 'utf8' });
    ok(fw.status === 0 && fw.stdout.includes('remodroid-server'), 'a firewall rule lets the server in');
    const starts = () => (log().match(/remodroid-server \S+: \d+ device/g) || []).length;
    const before = starts();
    ok(run(['restart']).status === 0, '`restart` asks the service');
    await until(async () => starts() > before, 10000, 'restarted');
    await up(B + '/');
    ok(/restarting with the new settings/.test(log()), 'the server restarts gracefully (sessions kept)');
    ok(run(['stop']).status === 0, '`stop` stops it');
    await down(B + '/');
    ok(run(['start']).status === 0, '`start` starts it');
    await up(B + '/');
    out = run(['uninstall']);
    ok(out.status === 0, '`uninstall` removes it');
    await down(B + '/');
    ok(spawnSync('sc', ['query', 'RemoDroidServer']).status !== 0, 'the service is gone');
    if (VERBOSE) console.log(log());
  }
}

main().catch(e => { failures++; console.log('  ✗ ' + e.message); }).finally(() => {
  for (const p of procs) p.kill();
  if (failures || VERBOSE)
    for (const f of fs.readdirSync(dir).filter(f => f.endsWith('.log')))
      console.log(`--- ${f}\n${fs.readFileSync(path.join(dir, f), 'utf8').slice(-4000)}`);
  console.log(`\n${passes} passed, ${failures} failed`);
  process.exit(failures ? 1 : 0);
});
