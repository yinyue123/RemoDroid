// Test doubles: a protocol-faithful adbd over TCP (with real RSA auth checks)
// and a fake phone server that speaks the tunnel protocol (plan §4.2, A.9).
import crypto from 'node:crypto';
import fs from 'node:fs';
import net from 'node:net';

const CMD = {
  CNXN: 0x4e584e43, OPEN: 0x4e45504f, OKAY: 0x59414b4f, CLSE: 0x45534c43,
  WRTE: 0x45545257, AUTH: 0x48545541,
};
const NAME = Object.fromEntries(Object.entries(CMD).map(([k, v]) => [v, k]));

function packet(cmd, a0, a1, data = Buffer.alloc(0)) {
  const h = Buffer.alloc(24);
  h.writeUInt32LE(cmd, 0); h.writeUInt32LE(a0, 4); h.writeUInt32LE(a1, 8);
  h.writeUInt32LE(data.length, 12); h.writeUInt32LE(0, 16); h.writeUInt32LE((cmd ^ 0xffffffff) >>> 0, 20);
  return Buffer.concat([h, data]);
}

// ---- tunnel frames: u16 len | u8 type | u8 sock | u8 af | addr | u16 port | payload
export const T = { UDP_OPEN: 1, UDP_SEND: 2, UDP_RECV: 3, UDP_CLOSE: 4, PING: 0x20, PONG: 0x21, CTRL: 0x30 };
export function tunBuild(type, sock = 0, af = 0, addr = null, port = 0, payload = Buffer.alloc(0)) {
  const al = af === 4 ? 4 : af === 6 ? 16 : 0;
  const hdr = 3 + al + (al ? 2 : 0);
  const b = Buffer.alloc(2 + hdr);
  b.writeUInt16BE(hdr + payload.length, 0);
  b[2] = type; b[3] = sock; b[4] = af;
  if (al) { Buffer.from(addr).copy(b, 5); b.writeUInt16BE(port, 5 + al); }
  return Buffer.concat([b, payload]);
}
export function tunParse(buf) {
  const out = [];
  let off = 0;
  while (buf.length - off >= 2) {
    const len = buf.readUInt16BE(off);
    if (buf.length - off < 2 + len) break;
    const p = buf.subarray(off + 2, off + 2 + len);
    const af = p[2], al = af === 4 ? 4 : af === 6 ? 16 : 0, hdr = 3 + al + (al ? 2 : 0);
    out.push({ type: p[0], sock: p[1], af, addr: p.subarray(3, 3 + al), port: al ? p.readUInt16BE(3 + al) : 0,
      payload: p.subarray(hdr) });
    off += 2 + len;
  }
  return { frames: out, rest: buf.subarray(off) };
}

// Checks the Android RSAPublicKey blob against the agent's PEM key.
export function checkAndroidPubkey(str, pemPath) {
  const b64 = str.replace(/\0$/, '').split(' ')[0];
  const blob = Buffer.from(b64, 'base64');
  if (blob.length !== 524) throw new Error('pubkey blob length ' + blob.length);
  if (blob.readUInt32LE(0) !== 64) throw new Error('modulus word count');
  const key = crypto.createPublicKey(fs.readFileSync(pemPath));
  const jwk = key.export({ format: 'jwk' });
  const nBE = Buffer.from(jwk.n, 'base64url');
  const nLE = Buffer.from(blob.subarray(8, 264));
  if (!Buffer.from(nBE).reverse().equals(nLE)) throw new Error('modulus mismatch');
  const n0 = BigInt(nLE.readUInt32LE(0)), n0inv = BigInt(blob.readUInt32LE(4));
  if ((n0 * n0inv) % (1n << 32n) !== (1n << 32n) - 1n) throw new Error('n0inv wrong');
  const N = BigInt('0x' + nBE.toString('hex'));
  const rr = BigInt('0x' + Buffer.from(blob.subarray(264, 520)).reverse().toString('hex'));
  if (rr !== (1n << 4096n) % N) throw new Error('rr wrong');
  if (blob.readUInt32LE(520) !== 65537) throw new Error('exponent');
  return true;
}

function verifySig(pemPath, token, sig) {
  const key = crypto.createPublicKey(fs.readFileSync(pemPath));
  const dec = crypto.publicDecrypt({ key, padding: crypto.constants.RSA_PKCS1_PADDING }, sig);
  const prefix = Buffer.from('3021300906052b0e03021a05000414', 'hex'); // DigestInfo(SHA-1)
  return dec.equals(Buffer.concat([prefix, token]));
}

// A fake phone: adbd + the phone server behind localabstract:remodroid.
export class FakePhone {
  constructor({ keyPath, rejectFirstKey = true, log = () => {} }) {
    Object.assign(this, { keyPath, rejectFirstKey, log });
    this.files = new Map();
    this.events = [];
    this.ctrl = [];          // CTRL messages received from the agent
    this.shells = [];        // commands run
    this.serverRunning = false;
    this.tunnel = null;      // { send(buf) }
    this.waiters = [];
    this.apiRequests = [];   // AI API requests on localabstract:remodroid-api
    this.apiHandler = req => this.defaultApi(req);
  }

  // A deterministic "JPEG" whose size follows max_edge (large ones exceed the server's 1 MB relay limit).
  shot(maxEdge) {
    const n = maxEdge >= 3000 ? 2 * 1024 * 1024 : 150 * 1024;
    const b = Buffer.alloc(n);
    for (let i = 0; i < n; i++) b[i] = (i * 7 + maxEdge) & 0xff;
    b[0] = 0xff; b[1] = 0xd8; b[2] = 0xff;
    return b;
  }

  defaultApi({ op, args }) {
    if (op === 'capabilities') return { body: { schema: 1, ui: { backend: 'uiautomation' }, screenshot: { encoder: 'c' } } };
    if (op === 'ui') return { body: { schema: 1, ui_id: 1, package: 'com.example', display: { w: 1080, h: 2400, rotation: 0, epoch: 0 },
      nodes: [{ id: 0, rid: 'com.example:id/search', cls: 'EditText', desc: 'Search', b: [40, 120, 1040, 220], f: ['click', 'edit', 'enabled'] }] } };
    if (op === 'screenshot') {
      const edge = Number(args.max_edge || 1280);
      return { type: 'image/jpeg', body: this.shot(edge), headers: { 'X-RD-Width': Math.round(1080 * edge / 2400), 'X-RD-Height': edge, 'X-RD-Epoch': 0, 'X-Evil': 'dropped' } };
    }
    if (op === 'action' && args.action === 'tap' && args.viewer) return { status: 409, body: { error: 'viewer_active', msg: 'a person is using the phone', retryable: true } };
    if (op === 'action') return { body: { ok: true, action: args.action, injected: true } };
    return { status: 404, body: { error: 'not_found', msg: 'unknown op ' + op } };
  }

  emit(kind, data) {
    this.events.push({ kind, data });
    for (const w of [...this.waiters]) if (w.pred({ kind, data })) { w.res({ kind, data }); this.waiters.splice(this.waiters.indexOf(w), 1); }
  }
  waitFor(pred, ms = 5000, what = 'event') {
    const hit = this.events.find(pred);
    if (hit) return Promise.resolve(hit);
    return new Promise((res, rej) => {
      const w = { pred, res };
      this.waiters.push(w);
      setTimeout(() => rej(new Error('timeout waiting for ' + what)), ms);
    });
  }

  listen() {
    this.server = net.createServer(sock => this.onConn(sock));
    return new Promise(r => this.server.listen(0, '127.0.0.1', () => r(this.server.address().port)));
  }

  onConn(sock) {
    let buf = Buffer.alloc(0), authed = false, sentToken = 0, token = null;
    const streams = new Map(); // remote(agent) id -> stream
    let nextLocal = 100;
    const send = (cmd, a0, a1, data) => sock.write(packet(cmd, a0, a1, data));
    const sendToken = () => { token = crypto.randomBytes(20); sentToken++; send(CMD.AUTH, 1, 0, token); };

    const openStream = (agentId, service) => {
      const local = nextLocal++;
      const st = { local, agentId, service, inbuf: Buffer.alloc(0), queue: [], waiting: false, closed: false };
      st.write = (data) => {
        if (st.closed) return;
        for (let i = 0; i < data.length; i += 4096) st.queue.push(data.subarray(i, i + 4096));
        st.pump();
      };
      st.pump = () => {
        if (st.waiting || st.closed) return;
        if (!st.queue.length) { if (st.ending) st.close(); return; }
        st.waiting = true;
        send(CMD.WRTE, local, agentId, st.queue.shift());
      };
      st.end = () => { st.ending = true; st.pump(); }; // close once everything is sent
      st.close = () => { if (!st.closed) { st.closed = true; send(CMD.CLSE, local, agentId); streams.delete(agentId); st.onClose?.(); } };
      streams.set(agentId, st);
      return st;
    };

    const v2 = (id, data) => { const h = Buffer.alloc(5); h[0] = id; h.writeUInt32LE(data.length, 1); return Buffer.concat([h, data]); };

    const handleOpen = (agentId, service) => {
      this.emit('open', service);
      if (service.startsWith('shell,v2,raw:') || service.startsWith('shell:')) {
        const cmd = service.replace(/^shell(,v2,raw)?:/, '');
        this.shells.push(cmd);
        const st = openStream(agentId, service);
        send(CMD.OKAY, st.local, agentId);
        const isV2 = service.startsWith('shell,v2');
        const finish = (out, code = 0) => {
          st.write(isV2 ? Buffer.concat([v2(1, Buffer.from(out)), v2(3, Buffer.from([code]))]) : Buffer.from(out));
          setTimeout(() => st.close(), 20);
        };
        if (cmd.startsWith('getprop')) finish('FakePhone\n14\n34\narm64-v8a\n');
        else if (cmd.includes('app_process')) {
          this.serverRunning = true;
          this.serverCmd = cmd;
          this.emit('server.start', cmd);
          st.write(isV2 ? v2(1, Buffer.from('remodroid server starting\n')) : Buffer.from('starting\n'));
          st.onClose = () => { this.serverRunning = false; this.emit('server.stop'); };
          this.serverStream = st;
        } else if (cmd.startsWith('echo ')) finish(cmd.slice(5) + '\n');
        else if (cmd.startsWith('exit ')) finish('', parseInt(cmd.slice(5), 10));
        else finish('');
        return;
      }
      if (service === 'sync:') {
        const st = openStream(agentId, service);
        send(CMD.OKAY, st.local, agentId);
        st.onData = (d) => this.handleSync(st, d);
        return;
      }
      if (service === 'localabstract:chrome_devtools_remote' || service === 'tcp:6000') { // echo; "bye\n" closes
        const st = openStream(agentId, service);
        send(CMD.OKAY, st.local, agentId);
        this.emit('sock.open', service);
        st.onData = (d) => { if (d.toString() === 'bye\n') st.end(); else st.write(d); };
        st.onClose = () => this.emit('sock.closed', service);
        return;
      }
      if (service === 'localabstract:firehose') { // 16 MB at once, paced only by the agent's OKAYs
        const st = openStream(agentId, service);
        send(CMD.OKAY, st.local, agentId);
        this.firehose = st;
        st.write(Buffer.alloc(16 << 20, 0x5a));
        st.onClose = () => this.emit('sock.closed', service);
        return;
      }
      if (service === 'localabstract:remodroid-api') { // one request line in, header line + body out
        if (!this.serverRunning) { send(CMD.CLSE, 0, agentId); return; }
        const st = openStream(agentId, service);
        send(CMD.OKAY, st.local, agentId);
        let rb = Buffer.alloc(0);
        st.onData = (d) => {
          rb = Buffer.concat([rb, d]);
          const nl = rb.indexOf(10);
          if (nl < 0 || st.answered) return;
          st.answered = true;
          const req = JSON.parse(rb.subarray(0, nl).toString());
          this.apiRequests.push(req);
          this.emit('api', req);
          const { status = 200, type = 'application/json', body, headers = {} } = this.apiHandler(req);
          const b = Buffer.isBuffer(body) ? body : Buffer.from(JSON.stringify(body));
          st.write(Buffer.concat([Buffer.from(JSON.stringify({ status, type, len: b.length, headers }) + '\n'), b]));
          st.end();
        };
        return;
      }
      if (service === 'localabstract:remodroid') {
        if (!this.serverRunning) { send(CMD.CLSE, 0, agentId); return; }
        const st = openStream(agentId, service);
        send(CMD.OKAY, st.local, agentId);
        let tb = Buffer.alloc(0);
        this.tunnel = { send: (b) => st.write(b) };
        st.onData = (d) => {
          tb = Buffer.concat([tb, d]);
          const { frames, rest } = tunParse(tb);
          tb = rest;
          for (const f of frames) this.onTunnelFrame(f);
        };
        st.onClose = () => { this.tunnel = null; this.emit('tunnel.closed'); };
        this.emit('tunnel.open');
        return;
      }
      send(CMD.CLSE, 0, agentId);
    };

    const onPacket = (cmd, a0, a1, data) => {
      if (cmd === CMD.CNXN) { sendToken(); return; }
      if (cmd === CMD.AUTH) {
        if (a0 === 2) {
          const ok = verifySig(this.keyPath, token, data);
          this.emit('auth.signature', ok);
          if (ok && !(this.rejectFirstKey && !this.keyAccepted)) { authed = true; send(CMD.CNXN, 0x01000001, 256 * 1024, Buffer.from('device::ro.product.model=FakePhone;features=shell_v2,cmd,stat_v2\0')); }
          else sendToken();
        } else if (a0 === 3) {
          const str = data.toString('latin1');
          let valid = false;
          try { valid = checkAndroidPubkey(str, this.keyPath); } catch (e) { this.log('pubkey check: ' + e.message); }
          this.emit('auth.pubkey', valid);
          // Simulate the user tapping "Allow" after a short delay.
          setTimeout(() => {
            this.keyAccepted = true;
            authed = true;
            send(CMD.CNXN, 0x01000001, 256 * 1024, Buffer.from('device::ro.product.model=FakePhone;features=shell_v2,cmd,stat_v2\0'));
          }, 150);
        }
        return;
      }
      if (!authed) return;
      if (cmd === CMD.OPEN) return handleOpen(a0, data.toString().replace(/\0$/, ''));
      const st = streams.get(a0);
      if (cmd === CMD.OKAY) { const s2 = [...streams.values()].find(s => s.local === a1); if (s2) { s2.waiting = false; s2.pump(); } return; }
      if (cmd === CMD.WRTE) {
        const s2 = [...streams.values()].find(s => s.local === a1);
        if (!s2) return;
        send(CMD.OKAY, s2.local, a0);
        s2.onData?.(data);
        return;
      }
      if (cmd === CMD.CLSE) {
        const s2 = [...streams.values()].find(s => s.local === a1);
        if (s2) { s2.closed = true; streams.delete(s2.agentId); s2.onClose?.(); }
      }
      void st;
    };

    sock.on('data', d => {
      buf = Buffer.concat([buf, d]);
      while (buf.length >= 24) {
        const len = buf.readUInt32LE(12);
        if (buf.length < 24 + len) break;
        const cmd = buf.readUInt32LE(0), a0 = buf.readUInt32LE(4), a1 = buf.readUInt32LE(8);
        if (buf.readUInt32LE(20) !== ((cmd ^ 0xffffffff) >>> 0)) { sock.destroy(); return; }
        const data = buf.subarray(24, 24 + len);
        buf = buf.subarray(24 + len);
        onPacket(cmd, a0, a1, data);
      }
    });
    sock.on('error', () => {});
    sock.on('close', () => { this.emit('adb.closed'); for (const s of streams.values()) s.onClose?.(); });
    this.dropConnection = () => sock.destroy();
  }

  handleSync(st, d) {
    st.inbuf = Buffer.concat([st.inbuf, d]);
    for (;;) {
      if (st.recvLeft) {
        const take = Math.min(st.recvLeft, st.inbuf.length);
        if (!take) return;
        st.file.push(st.inbuf.subarray(0, take));
        st.inbuf = st.inbuf.subarray(take);
        st.recvLeft -= take;
        continue;
      }
      if (st.inbuf.length < 8) return;
      const id = st.inbuf.subarray(0, 4).toString(), len = st.inbuf.readUInt32LE(4);
      if (id === 'DATA') { st.recvLeft = len; st.inbuf = st.inbuf.subarray(8); continue; }
      if (id === 'DONE') {
        st.inbuf = st.inbuf.subarray(8);
        this.files.set(st.path, { data: Buffer.concat(st.file), mtime: len, mode: st.mode });
        this.emit('push', st.path);
        const r = Buffer.alloc(8); r.write('OKAY'); st.write(r);
        continue;
      }
      if (id === 'QUIT') { st.inbuf = st.inbuf.subarray(8); st.close(); return; }
      if (st.inbuf.length < 8 + len) return;
      const arg = st.inbuf.subarray(8, 8 + len).toString();
      st.inbuf = st.inbuf.subarray(8 + len);
      if (id === 'STAT') {
        const f = this.files.get(arg);
        const r = Buffer.alloc(16); r.write('STAT');
        if (f) { r.writeUInt32LE(0o100644, 4); r.writeUInt32LE(f.data.length, 8); r.writeUInt32LE(f.mtime, 12); }
        st.write(r);
      } else if (id === 'SEND') {
        const comma = arg.lastIndexOf(',');
        st.path = arg.slice(0, comma); st.mode = parseInt(arg.slice(comma + 1), 10); st.file = [];
      } else if (id === 'RECV') {
        const f = this.files.get(arg);
        if (!f) { const m = Buffer.from('No such file'); const r = Buffer.alloc(8); r.write('FAIL'); r.writeUInt32LE(m.length, 4); st.write(Buffer.concat([r, m])); continue; }
        const h = Buffer.alloc(8); h.write('DATA'); h.writeUInt32LE(f.data.length, 4);
        const d2 = Buffer.alloc(8); d2.write('DONE');
        st.write(Buffer.concat([h, f.data, d2]));
      }
    }
  }

  // ---- fake phone server behaviour over the tunnel ----
  sendCtrl(obj) { this.tunnel?.send(tunBuild(T.CTRL, 0, 0, null, 0, Buffer.from(JSON.stringify(obj)))); }
  onTunnelFrame(f) {
    if (f.type === T.PING) { this.tunnel.send(tunBuild(T.PONG)); return; }
    if (f.type === T.UDP_RECV) { this.emit('udp.recv', { sock: f.sock, port: f.port, payload: f.payload.toString() }); return; }
    if (f.type === T.CTRL) {
      const m = JSON.parse(f.payload.toString());
      this.ctrl.push(m);
      this.emit('ctrl', m);
      if (m.t === 'config' && !this.readySent) {
        this.readySent = true;
        this.sendCtrl({ t: 'ready', d: { proto: 1, version: 'fake-0.1', android: '14', sdk: 34, model: 'FakePhone', codecs: ['h265', 'h264'], max_fps: 120 } });
        this.sendCtrl({ t: 'status', d: { battery: { level: 82, charging: true, temp_c: 31.5 }, screen: 'off', locked: false, path: 'tunnel' } });
      }
      if (m.t === 'session.start') this.sendCtrl({ t: 'signal', sid: m.sid, d: { type: 'offer', sdp: 'v=0 fake-offer' } });
    }
  }
}
