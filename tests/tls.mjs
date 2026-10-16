// HTTPS / wss / ACME test for remodroid-server and remodroid-agent.
//   node tests/tls.mjs build/host
//   PEBBLE_DIR=/path/to/pebble node tests/tls.mjs build/host     (also runs the ACME part)
// PEBBLE_DIR holds the pebble and pebble-challtestsrv binaries (github.com/letsencrypt/pebble).
// Test certificates are made with the openssl command-line tool.
import { execFileSync, spawn } from 'node:child_process';
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import https from 'node:https';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import tls from 'node:tls';

const BIN = path.resolve(process.argv[2] || 'build/host');
const PEBBLE = process.env.PEBBLE_DIR;
const VERBOSE = !!process.env.VERBOSE;
const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'remodroid-tls-'));
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
    const v = await fn().catch(e => { if (process.env.VERBOSE) console.log('    (' + what + ': ' + e.message + ')'); return null; });
    if (v) return v;
    if (Date.now() > end) throw new Error('timeout: ' + what);
    await sleep(100);
  }
}
function freePort() {
  return new Promise(r => { const s = net.createServer(); s.listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => r(p)); }); });
}
// Output goes to a file (dir/<name>.log); p.log reads it back.
function start(name, cmd, args, env) {
  const file = path.join(dir, `${name}.log`), fd = fs.openSync(file, 'w');
  const p = spawn(cmd, args, { stdio: ['ignore', fd, fd], env: { ...process.env, ...env } });
  fs.closeSync(fd);
  p.on('error', e => fs.appendFileSync(file, `spawn failed: ${e.message}\n`)); /* reported via the readiness timeout */
  Object.defineProperty(p, 'log', { get: () => fs.readFileSync(file, 'utf8') });
  p.name = name;
  procs.push(p);
  return p;
}
const ossl = (...a) => execFileSync('openssl', a, { cwd: dir, stdio: ['ignore', 'pipe', 'pipe'] });

// CA + certificate for localhost / 127.0.0.1 (prefix: file names)
function makeCert(prefix, cn = 'localhost') {
  ossl('req', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes', '-keyout', `${prefix}.key`,
    '-out', `${prefix}.csr`, '-subj', `/CN=${cn}`);
  fs.writeFileSync(path.join(dir, `${prefix}.ext`), 'subjectAltName=DNS:localhost,DNS:pebble,IP:127.0.0.1\n');
  ossl('x509', '-req', '-in', `${prefix}.csr`, '-CA', 'ca.pem', '-CAkey', 'ca.key', '-CAcreateserial',
    '-out', `${prefix}.pem`, '-days', '2', '-extfile', `${prefix}.ext`);
}

function get(url, opts = {}) {
  const mod = url.startsWith('https:') ? https : http;
  return new Promise((res, rej) => {
    const headers = { 'User-Agent': 'remodroid-test', ...opts.headers }; /* ACME servers refuse requests without one */
    const req = mod.request(url, { ca: fs.readFileSync(path.join(dir, 'ca.pem')), ...opts, headers }, r => {
      const chunks = [];
      r.on('data', c => chunks.push(c));
      r.on('end', () => res({ status: r.statusCode, headers: r.headers, body: Buffer.concat(chunks) }));
    });
    req.on('error', rej);
    req.end();
  });
}

// Peer certificate of a TLS port (no verification: just to see what is served)
function peerCert(port, servername) {
  return new Promise((res, rej) => {
    const s = tls.connect({ port, host: '127.0.0.1', servername, rejectUnauthorized: false }, () => {
      const c = s.getPeerCertificate(); const proto = s.getProtocol(); s.end(); res({ ...c, proto });
    });
    s.on('error', rej);
  });
}

// Minimal WebSocket client over TLS
function wssConnect(port, pathname, headers) {
  return new Promise((resolve, reject) => {
    const sock = tls.connect({ port, host: '127.0.0.1', servername: 'localhost', ca: fs.readFileSync(path.join(dir, 'ca.pem')) });
    const key = crypto.randomBytes(16).toString('base64');
    let buf = Buffer.alloc(0), open = false;
    const ws = { msgs: [], close() { sock.end(); } };
    ws.wait = (pred, ms = 8000) => until(async () => ws.msgs.find(pred), ms, 'ws message');
    sock.on('secureConnect', () => {
      const extra = Object.entries(headers).map(([k, v]) => `${k}: ${v}\r\n`).join('');
      sock.write(`GET ${pathname} HTTP/1.1\r\nHost: localhost:${port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n` +
        `Sec-WebSocket-Key: ${key}\r\nSec-WebSocket-Version: 13\r\n${extra}\r\n`);
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
    sock.on('error', e => { if (!open) reject(e); });
  });
}

function agentIni(name, url, extra = '') {
  const f = path.join(dir, `agent-${name}.ini`);
  fs.writeFileSync(f, `[server]
url = ${url}
device_id = router1
device_secret = pw-device
${extra}
[keepalive]
reconnect_max_seconds = 2
[agent]
adbkey = ${dir}/adbkey
socket = ${dir}/agent-${name}.sock
log_level = debug
[phone]
adb = tcp:127.0.0.1:1
`);
  return f;
}

async function main() {
  const server = path.join(BIN, 'remodroid-server'), agent = path.join(BIN, 'remodroid-agent');
  const hash = pw => execFileSync(server, ['hashpw', pw]).toString().trim();
  ossl('req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256', '-nodes', '-keyout', 'ca.key',
    '-out', 'ca.pem', '-days', '2', '-subj', '/CN=RemoDroid Test CA');
  makeCert('srv');
  fs.mkdirSync(path.join(dir, 'web'));
  fs.writeFileSync(path.join(dir, 'web', 'index.html'), '<!doctype html><title>RemoDroid</title>');
  const big = crypto.randomBytes(3 * 1024 * 1024 + 123);
  fs.writeFileSync(path.join(dir, 'web', 'big.bin'), big);
  const httpPort = await freePort(), tlsPort = await freePort();
  const users = `[users]\nme = ${hash('pw-user')}\n[devices]\nrouter1 = ${hash('pw-device')}\n`;
  fs.writeFileSync(path.join(dir, 'server.ini'), `[http]
listen = 127.0.0.1:${httpPort}
web_root = ${dir}/web
[tls]
listen = 127.0.0.1:${tlsPort}
cert = ${dir}/srv.pem
key = ${dir}/srv.key
${users}`);
  const srv = start('server', server, ['-c', path.join(dir, 'server.ini'), '-v']);
  await until(() => get(`https://localhost:${tlsPort}/`).then(() => true), 5000, 'server up');
  const basic = { Authorization: 'Basic ' + Buffer.from('me:pw-user').toString('base64') };

  console.log('HTTPS');
  let r = await get(`http://127.0.0.1:${httpPort}/page?x=1`);
  ok(r.status === 308 && r.headers.location === `https://127.0.0.1:${tlsPort}/page?x=1`, 'plain HTTP redirects to HTTPS (308, same path and query)');
  r = await get(`http://127.0.0.1:${httpPort}/.well-known/acme-challenge/none`);
  ok(r.status === 404, 'ACME challenge path is answered on plain HTTP, not redirected');
  r = await get(`https://localhost:${tlsPort}/`, { headers: basic });
  const setCookie = r.headers['set-cookie']?.[0] || '';
  ok(r.status === 200 && /; Secure/.test(setCookie) && /HttpOnly/.test(setCookie), 'HTTPS login sets a Secure, HttpOnly cookie');
  const cookie = setCookie.split(';')[0];
  const pc = await peerCert(tlsPort, 'localhost');
  ok(pc.proto === 'TLSv1.2' && pc.subject.CN === 'localhost', 'TLS 1.2 with the configured certificate');
  const files = await Promise.all([1, 2, 3, 4].map(() => get(`https://localhost:${tlsPort}/big.bin`, { headers: { Cookie: cookie } })));
  ok(files.every(f => f.status === 200 && f.body.equals(big)), '4 parallel 3 MB downloads over TLS are byte-exact');
  r = await get(`https://localhost:${tlsPort}/api/v1/me`, { headers: { Cookie: cookie } });
  ok(r.status === 200 && JSON.parse(r.body).user === 'me', 'REST API over HTTPS');

  console.log('wss');
  const viewer = await wssConnect(tlsPort, '/ws/viewer', { Cookie: cookie, Origin: `https://localhost:${tlsPort}` });
  ok((await viewer.wait(m => m.t === 'devices')).d.devices[0].id === 'router1', 'viewer WebSocket over TLS');
  start('agent-good', agent, ['-c', agentIni('good', `wss://localhost:${tlsPort}/ws/agent`, `ca_file = ${dir}/ca.pem`)]);
  await viewer.wait(m => m.t === 'device.update' && m.d.patch.online === true, 10000);
  ok(true, 'agent connects over wss:// and the viewer sees it online');
  procs.find(p => p.name === 'agent-good').kill('SIGTERM');
  const bad = start('agent-bad', agent, ['-c', agentIni('bad', `wss://localhost:${tlsPort}/ws/agent`, `ca_file = ${dir}/srv.pem`)]);
  await until(async () => /not trusted/.test(bad.log), 8000, 'untrusted');
  ok(true, 'agent refuses a server certificate its CA file does not vouch for');
  bad.kill('SIGTERM');
  const plain = start('agent-plain', agent, ['-c', agentIni('plain', `ws://127.0.0.1:${httpPort}/ws/agent`)]);
  await until(async () => /HTTP 308/.test(plain.log), 8000, 'plain refused');
  ok(true, 'plain ws:// agents are redirected (and refused) when plain_http = redirect');
  plain.kill('SIGTERM');

  console.log('certificate reload');
  const before = (await peerCert(tlsPort, 'localhost')).fingerprint256;
  makeCert('srv2', 'renewed.localhost');
  fs.copyFileSync(path.join(dir, 'srv2.pem'), path.join(dir, 'srv.pem'));
  fs.copyFileSync(path.join(dir, 'srv2.key'), path.join(dir, 'srv.key'));
  srv.kill('SIGHUP');
  const after = await until(async () => { const c = await peerCert(tlsPort, 'localhost'); return c.fingerprint256 !== before && c; }, 5000, 'reload');
  ok(after.subject.CN === 'renewed.localhost', 'SIGHUP loads a replaced certificate for new connections');
  viewer.close();
  srv.kill('SIGTERM');

  if (!PEBBLE) {
    console.log('ACME: skipped (set PEBBLE_DIR to the pebble binaries to run it)');
    return;
  }

  console.log('ACME (Pebble)');
  const dnsPort = await freePort(), acmePort = await freePort(), mgmtPort = await freePort(), chPort = await freePort();
  const http2 = await freePort(), tls2 = await freePort();
  const find = name => [path.join(PEBBLE, name), path.join(PEBBLE, `${name}-linux-amd64/linux/amd64/${name}`)].find(f => fs.existsSync(f));
  makeCert('pebble');
  fs.writeFileSync(path.join(dir, 'pebble.json'), JSON.stringify({ pebble: {
    listenAddress: `127.0.0.1:${acmePort}`, managementListenAddress: `127.0.0.1:${mgmtPort}`,
    certificate: `${dir}/pebble.pem`, privateKey: `${dir}/pebble.key`, httpPort: http2, tlsPort: 5001,
    ocspResponderURL: '', externalAccountBindingRequired: false, domainBlocklist: [], retryAfter: { authz: 1, order: 1 },
    keyAlgorithm: 'ecdsa',
    profiles: { default: { description: 'default', validityPeriod: 7776000 }, short: { description: 'short', validityPeriod: 30 } },
  } }));
  start('challtestsrv', find('pebble-challtestsrv'), ['-defaultIPv4', '127.0.0.1', '-defaultIPv6', '', '-dnsserver', `127.0.0.1:${dnsPort}`,
    '-http01', '', '-https01', '', '-tlsalpn01', '', '-doh', '', '-management', `127.0.0.1:${chPort}`]);
  // PEBBLE_WFE_NONCEREJECT stays at its default: 5% of nonces are refused, which exercises badNonce retries
  start('pebble', find('pebble'), ['-config', path.join(dir, 'pebble.json'), '-dnsserver', `127.0.0.1:${dnsPort}`], { PEBBLE_VA_NOSLEEP: '1' });
  await until(() => get(`https://127.0.0.1:${acmePort}/dir`).then(r => r.status === 200), 10000, 'pebble up');
  const acmeIni = profile => fs.writeFileSync(path.join(dir, 'acme.ini'), `[http]
listen = 127.0.0.1:${http2}
web_root = ${dir}/web
[tls]
listen = 127.0.0.1:${tls2}
[acme]
domains = remodroid.test, www.remodroid.test
directory = https://127.0.0.1:${acmePort}/dir
dir = ${dir}/state/acme
ca_file = ${dir}/ca.pem
${profile ? 'profile = ' + profile : ''}
${users}`);
  acmeIni('');
  let s2 = start('acme-server', server, ['-c', path.join(dir, 'acme.ini')]);
  const issued = await until(async () => { const c = await peerCert(tls2, 'remodroid.test'); return c.subjectaltname && c; }, 30000, 'certificate issued');
  ok(/DNS:remodroid\.test/.test(issued.subjectaltname) && /DNS:www\.remodroid\.test/.test(issued.subjectaltname) && /Pebble/.test(issued.issuer.CN),
    'certificate issued by the ACME CA for both domains (http-01 answered on plain HTTP)');
  ok(/answered the http-01 challenge/.test(s2.log), 'http-01 challenges were served by remodroid-server');
  const mode = fs.statSync(path.join(dir, 'state/acme/key.pem')).mode & 0o777;
  ok(mode === 0o600 && fs.existsSync(path.join(dir, 'state/acme/account.key')), 'keys are stored with mode 0600');
  s2.kill('SIGTERM');
  await sleep(300);
  s2 = start('acme-server-2', server, ['-c', path.join(dir, 'acme.ini'), '-v']);
  await until(async () => /certificate is current/.test(s2.log), 5000, 'reuse');
  ok(!/requesting a certificate/.test(s2.log) && (await peerCert(tls2, 'remodroid.test')).serialNumber === issued.serialNumber,
    'a restart reuses the stored certificate without contacting the CA');
  s2.kill('SIGTERM');
  await sleep(300);

  fs.rmSync(path.join(dir, 'state'), { recursive: true });
  acmeIni('short'); // 30 s certificates: renewal is due after 20 s
  s2 = start('acme-server-3', server, ['-c', path.join(dir, 'acme.ini')]);
  const first = await until(async () => { const c = await peerCert(tls2, 'remodroid.test'); return c.subjectaltname && c; }, 30000, 'short certificate');
  const second = await until(async () => { const c = await peerCert(tls2, 'remodroid.test'); return c.serialNumber !== first.serialNumber && c; }, 40000, 'renewal');
  ok(second && (s2.log.match(/certificate issued/g) || []).length >= 2, 'the certificate is renewed automatically before it expires');
  s2.kill('SIGTERM');
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
