// First-run wizard and router enrollment on remodroid-server, with the real agent:
// setup mode and its token, saving (server restarts on the new port), login, codes,
// `remodroid-agent enroll`, settings through the panel, rename and remove.
//   node tests/setup.mjs build/host
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';

const BIN = path.resolve(process.argv[2] || 'build/host');
const ROOT = path.resolve(path.dirname(new URL(import.meta.url).pathname), '..');
const VERBOSE = !!process.env.VERBOSE;
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'remodroid-setup-'));
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
function start(name, cmd, args) {
  const file = path.join(dir, name + '.log'), fd = fs.openSync(file, 'w');
  const p = spawn(cmd, args, { stdio: ['ignore', fd, fd] });
  fs.closeSync(fd);
  Object.defineProperty(p, 'log', { get: () => fs.readFileSync(file, 'utf8') });
  p.name = name;
  procs.push(p);
  return p;
}
function run(cmd, args) {
  return new Promise(res => { const p = spawn(cmd, args); let out = ''; p.stdout.on('data', d => out += d); p.stderr.on('data', d => out += d); p.on('close', code => res({ code, out })); });
}

async function main() {
  const p1 = await freePort(), p2 = await freePort();
  const ini = path.join(dir, 'server.ini');
  fs.writeFileSync(ini, `; RemoDroid server (test)
[server]
state_dir = ${dir}/state     ; setup token
[http]
listen = 127.0.0.1:${p1}
web_root = ${ROOT}/web
`);
  const server = start('server', path.join(BIN, 'remodroid-server'), ['-c', ini]);
  const A = `http://127.0.0.1:${p1}`, B = `http://127.0.0.1:${p2}`;
  await until(() => fetch(A + '/wizard.html').then(r => r.ok), 5000, 'server up');

  console.log('setup mode');
  const token = fs.readFileSync(path.join(dir, 'state/setup-token'), 'utf8').trim();
  ok(/^[a-z0-9]{12}$/.test(token) && (fs.statSync(path.join(dir, 'state/setup-token')).mode & 0o777) === 0o600, 'a setup token is written (mode 0600)');
  ok(server.log.includes(`/wizard.html?token=${token}`), 'the log says where to open the wizard');
  let r = await fetch(A + '/?token=' + token, { redirect: 'manual' });
  ok(r.status === 302 && r.headers.get('location') === '/wizard.html?token=' + token, '/ goes to the wizard, keeping the token');
  r = await fetch(A + '/api/v1/devices');
  ok(r.status === 503, 'the API is closed until setup is done');
  r = await fetch(A + '/api/v1/setup');
  ok(r.status === 403, 'the wizard API needs the setup token');
  const T = { 'X-Setup-Token': token };
  let j = await (await fetch(A + '/api/v1/setup', { headers: T })).json();
  ok(j.platform === 'server' && j.configured === false && j.config_file === ini, 'the wizard reads the current settings');
  r = await fetch(A + '/api/v1/setup', { method: 'POST', headers: T, body: JSON.stringify({ http_port: p2, username: 'admin', password: 'correct-horse', turn_enabled: true, turn_port: 3478, public_ip: 'not-an-ip' }) });
  ok(r.status === 400 && /public IPv4/.test((await r.json()).msg), 'bad input is explained');
  r = await fetch(A + '/api/v1/setup', { method: 'POST', headers: T, body: JSON.stringify({
    http_port: p2, username: 'admin', password: 'correct-horse', turn_enabled: false,
    ice_extra: 'stun:stun.cloudflare.com:3478, turn:turn.example.com:3478?transport=udp alice s3cret' }) });
  ok(r.status === 200, 'saving the wizard is accepted');
  await until(() => fetch(B + '/', { redirect: 'manual' }).then(r2 => r2.status === 302), 6000, 'restart on the new port');
  ok(true, 'the server restarts itself on the chosen port');
  const conf = fs.readFileSync(ini, 'utf8');
  ok(/^; RemoDroid server \(test\)/.test(conf) && /\[users\]\nadmin = \$2b\$10\$/.test(conf) && new RegExp(`listen = 0\\.0\\.0\\.0:${p2}`).test(conf),
    'server.ini gets the admin (bcrypt) and port, comments kept');
  ok(!fs.existsSync(path.join(dir, 'state/setup-token')), 'the setup token is gone');

  console.log('after setup');
  r = await fetch(B + '/api/v1/setup', { headers: T });
  ok(r.status === 401, 'the wizard now needs a login, not the token');
  const login = pw => fetch(B + '/login', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ user: 'admin', password: pw }) });
  r = await login('correct-horse');
  const cookie = (r.headers.get('set-cookie') || '').split(';')[0];
  ok(r.status === 200 && cookie, 'the login page logs the new admin in');
  const H = { Cookie: cookie };
  j = await (await fetch(B + '/api/v1/setup', { headers: H })).json();
  ok(j.configured && j.username === 'admin' && j.turn_enabled === false && /alice/.test(j.ice_extra), 'settings can be read back');

  console.log('enroll');
  const code = (await (await fetch(B + '/api/v1/enroll/codes', { method: 'POST', headers: H, body: '{"name":"Living room"}' })).json()).code;
  ok(/^[2-9A-Z]{4}-[2-9A-Z]{4}$/.test(code), 'a one-time code');
  const aini = path.join(dir, 'agent.ini');
  fs.writeFileSync(aini, `[agent]\nadbkey = ${dir}/adbkey\nsocket = ${dir}/agent.sock\n[phone]\nadb = tcp:127.0.0.1:1\n`);
  const agent = start('agent', path.join(BIN, 'remodroid-agent'), ['-c', aini]);
  await until(async () => fs.existsSync(path.join(dir, 'agent.sock')), 10000, 'agent socket'); /* after its RSA key is made */
  const t0 = Date.now();
  const en = await run(path.join(BIN, 'remodroid-agent'), ['-c', aini, 'enroll', '--panel', B + '/', '--code', code]);
  ok(en.code === 0 && /bound — this router is online/.test(en.out) && Date.now() - t0 < 8000, 'remodroid enroll binds the router within seconds');
  if (en.code !== 0 || !/bound/.test(en.out)) console.log(en.out.replace(/^/gm, '      '));
  ok(/device_id = living-room/.test(fs.readFileSync(aini, 'utf8')) && /\[devices\]\nliving-room = \$2b\$/.test(fs.readFileSync(ini, 'utf8')),
    'both config files have the router');
  const fresh = path.join(dir, 'fresh.ini');
  const code2 = (await (await fetch(B + '/api/v1/enroll/codes', { method: 'POST', headers: H, body: '{}' })).json()).code;
  const en2 = await run(path.join(BIN, 'remodroid-agent'), ['-c', fresh, 'enroll', '--panel', B, '--code', code2]);
  const realAgent = fs.existsSync('/var/run/remodroid.sock') || fs.existsSync('/run/remodroid.sock'); /* a real install on this machine answers instead */
  ok(en2.code === 0 && (fs.statSync(fresh).mode & 0o777) === 0o600 && (realAgent || /isn't running/.test(en2.out)), 'enroll creates a missing agent.ini (0600) and says to start the agent');
  j = await (await fetch(B + `/api/v1/enroll/codes/${code}`, { headers: H })).json();
  ok(j.state === 'used' && j.device === 'living-room' && j.online, 'the page sees the router online');
  const wrong = await run(path.join(BIN, 'remodroid-agent'), ['-c', path.join(dir, 'x.ini'), 'enroll', '--panel', B, '--code', 'ZZZZ-ZZZZ']);
  ok(wrong.code === 1 && /wrong, used or expired/.test(wrong.out), 'a wrong code is refused');

  console.log('management');
  r = await fetch(B + '/api/v1/devices/living-room/cmd', { method: 'POST', headers: H, body: '{"cmd":"agent.config.get"}' });
  j = await r.json();
  ok(r.status === 200 && typeof j.result === 'object', 'agent.config.get returns the router settings');
  r = await fetch(B + '/api/v1/devices/living-room/cmd', { method: 'POST', headers: H, body: JSON.stringify({ cmd: 'agent.config', args: { screenOff: false, codec: 'h264' } }) });
  const a = fs.readFileSync(aini, 'utf8');
  ok(r.status === 200 && /screen_off_on_connect = false/.test(a) && /codec = h264/.test(a), 'settings saved from the panel land in agent.ini');
  r = await fetch(B + '/api/v1/devices/living-room', { method: 'PATCH', headers: H, body: '{"name":"Bedroom"}' });
  ok(r.status === 200 && /living-room = Bedroom/.test(fs.readFileSync(ini, 'utf8')), 'rename is saved');
  r = await fetch(B + '/api/v1/devices/living-room', { method: 'DELETE', headers: H });
  ok(r.status === 204 && !/living-room =/.test(fs.readFileSync(ini, 'utf8')), 'remove takes it out of server.ini');
  await until(async () => /unauthorized|HTTP 40[13]|rejected/.test(agent.log), 10000, 'agent refused');
  ok(true, 'the removed router can no longer log in');

  console.log('settings (the panel\'s Server tab)');
  r = await fetch(B + '/api/v1/settings', { headers: H });
  let txt = await r.text();
  j = JSON.parse(txt);
  ok(r.status === 200 && j.ice_servers.length === 2 && j.ice_servers[1].username === 'alice' && j.ice_servers[1].has_credential && !/s3cret/.test(txt),
    'the STUN/TURN list comes back without its passwords');
  const PATCH = body => fetch(B + '/api/v1/settings', { method: 'PATCH', headers: H, body: JSON.stringify(body) });
  r = await PATCH({ ice_servers: [{ url: 'stun:stun.cloudflare.com:3478' },
    { url: 'turn:turn.example.com:3479?transport=udp', username: 'alice', was: 'turn:turn.example.com:3478?transport=udp' },
    { url: 'stun:203.0.113.10:3478' }] });
  j = await r.json();
  ok(r.status === 200 && j.restart_ms === 0 && /extra = stun:stun\.cloudflare\.com:3478, turn:turn\.example\.com:3479\?transport=udp alice s3cret, stun:203\.0\.113\.10:3478/.test(fs.readFileSync(ini, 'utf8')),
    'editing one server keeps its password; adding one needs just the address; no restart');
  r = await PATCH({ ice_servers: [{ url: 'http://example.com' }] });
  ok(r.status === 400, 'a malformed server is refused');
  r = await PATCH({ password: 'new-password-1' });
  ok(r.status === 403, 'a new password needs the current one');
  r = await PATCH({ current_password: 'correct-horse', password: 'new-password-1' });
  j = await r.json();
  ok(r.status === 200 && j.restart_ms === 0 && (await login('new-password-1')).status === 200 && (await login('correct-horse')).status === 401,
    'the password changes on its own, nothing else does');
  const tport = 20000 + Math.floor(Math.random() * 20000);
  r = await PATCH({ turn_enabled: true, public_ip: '203.0.113.7', turn_port: tport });
  j = await r.json();
  ok(r.status === 200 && j.restart_ms > 0 && new RegExp(`\\[turn\\][^[]*listen = 0\\.0\\.0\\.0:${tport}`).test(fs.readFileSync(ini, 'utf8')),
    'turning on the built-in TURN restarts the server');
  await sleep(j.restart_ms + 300);
  await until(() => fetch(B + '/api/v1/settings', { headers: H }).then(x => x.ok, () => false), 8000, 'back after restart');
  j = await (await fetch(B + '/api/v1/settings', { headers: H })).json();
  ok(j.turn_enabled && j.turn_port === tport && j.public_ip === '203.0.113.7', 'the server is back with TURN on');
}

const timer = setTimeout(() => { console.log('  ✗ overall timeout'); failures++; finish(); }, 90000);
function finish() {
  clearTimeout(timer);
  for (const p of procs) p.kill('SIGTERM');
  if (failures || VERBOSE) for (const p of procs) console.log(`---- ${p.name} ----\n` + p.log.slice(-3000));
  console.log(`\n${passes} passed, ${failures} failed`);
  if (!failures) fs.rmSync(dir, { recursive: true, force: true });
  process.exit(failures ? 1 : 0);
}
main().then(finish, e => { console.log('  ✗ ' + e.message); failures++; finish(); });
