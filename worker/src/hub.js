// The Hub: one Durable Object that is the whole RemoDroid panel on Cloudflare.
//
// It mirrors remodroid-server (server/*.c) on the wire, so web/ and remodroid-agent
// work unchanged:
//   login      /login page (routers: Basic auth) → rd_session cookie (30 days), per-IP lockout   (auth.c)
//   setup      /api/v1/setup: admin account, Cloudflare TURN / other ICE  (setup.c)
//   routers    one-time codes, /api/v1/enroll, rename, remove            (enroll.c, api.c)
//   relay      /ws/viewer and /ws/agent: devices, session.*, signal, cmd (hub.c)
// Data lives in the Durable Object's SQLite; WebSockets use the hibernation API, so
// per-socket facts are kept in their attachments and device state in SQLite.
import { DurableObject } from 'cloudflare:workers';

const PROTO = 1;
const VERSION = '0.3.0';
const CODE_TTL = 10 * 60 * 1000;
const CODE_ABC = '23456789ABCDEFGHJKMNPQRSTUVWXYZ';
const PBKDF2_ITER = 20000;   // keeps a login inside the Workers CPU budget; cookies make logins rare
const MAX_FAILED = 5, LOCKOUT_MS = 15 * 60 * 1000;
const enc = new TextEncoder();

const json = (body, status = 200, headers = {}) =>
  new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json', 'cache-control': 'no-store', ...headers } });
const err = (status, code, msg) => json({ error: code, msg }, status);
const hex = buf => [...new Uint8Array(buf)].map(b => b.toString(16).padStart(2, '0')).join('');
const b64 = buf => btoa(String.fromCharCode(...new Uint8Array(buf)));
const b64url = buf => b64(buf).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
const rand = n => crypto.getRandomValues(new Uint8Array(n));
const now = () => Date.now();
const msg = (t, d = {}, extra = {}) => JSON.stringify({ t, ...extra, d });

function timingSafeEqual(a, b) {
  const x = enc.encode(a), y = enc.encode(b);
  let diff = x.length ^ y.length;
  for (let i = 0; i < Math.max(x.length, y.length); i++) diff |= (x[i] || 0) ^ (y[i] || 0);
  return diff === 0;
}

async function pbkdf2(password, salt, iter) {
  const key = await crypto.subtle.importKey('raw', enc.encode(password), 'PBKDF2', false, ['deriveBits']);
  return crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt, iterations: iter }, key, 256);
}
async function hashPassword(pw) {
  const salt = rand(16);
  return `pbkdf2$${PBKDF2_ITER}$${b64(salt)}$${b64(await pbkdf2(pw, salt, PBKDF2_ITER))}`;
}
async function checkPassword(stored, pw) {
  const [kind, iter, salt, hash] = String(stored || '').split('$');
  if (kind !== 'pbkdf2') return false;
  const s = Uint8Array.from(atob(salt), c => c.charCodeAt(0));
  return timingSafeEqual(b64(await pbkdf2(pw, s, +iter)), hash);
}
const sha256 = async s => hex(await crypto.subtle.digest('SHA-256', enc.encode(s)));

function normalizeCode(s) {
  const c = String(s || '').toUpperCase().replace(/[\s-]/g, '');
  if (c.length !== 8 || [...c].some(ch => !CODE_ABC.includes(ch))) return null;
  return c.slice(0, 4) + '-' + c.slice(4);
}

/* [ice] extra format, shared with remodroid-server: "url [user credential], url, …" */
function parseIce(line) {
  const out = [];
  for (const entry of String(line || '').split(',')) {
    const [url, username, credential] = entry.trim().split(/\s+/);
    if (!/^(stun|turns?):/.test(url || '')) continue;
    const s = { urls: [url] };
    if (username && credential) Object.assign(s, { username, credential });
    out.push(s);
  }
  return out;
}

export class Hub extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.sql = ctx.storage.sql;
    this.pending = new Map(); // cmd id → { resolve, timer }
    this.nextCmd = 1;
    ctx.blockConcurrencyWhile(async () => {
      this.sql.exec(`
        CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT);
        CREATE TABLE IF NOT EXISTS users (name TEXT PRIMARY KEY, hash TEXT);
        CREATE TABLE IF NOT EXISTS devices (id TEXT PRIMARY KEY, name TEXT, hash TEXT, created INTEGER,
          last_seen INTEGER, public_ip TEXT, hello TEXT, status TEXT, online_since INTEGER);
        CREATE TABLE IF NOT EXISTS sessions (id TEXT PRIMARY KEY, name TEXT, device INTEGER, created INTEGER);
        CREATE TABLE IF NOT EXISTS codes (code TEXT PRIMARY KEY, name TEXT, expires INTEGER, device TEXT);
        CREATE TABLE IF NOT EXISTS fails (ip TEXT PRIMARY KEY, count INTEGER, until INTEGER);
        CREATE TABLE IF NOT EXISTS vsessions (sid TEXT PRIMARY KEY, device TEXT, viewer TEXT, since INTEGER);`);
    });
  }

  /* ---------------- small storage helpers ---------------- */
  one(q, ...a) { return this.sql.exec(q, ...a).toArray()[0] || null; }
  all(q, ...a) { return this.sql.exec(q, ...a).toArray(); }
  kv(k, d = '') { const r = this.one('SELECT v FROM kv WHERE k = ?', k); return r ? r.v : d; }
  setKv(k, v) { this.sql.exec('INSERT INTO kv (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v', k, String(v)); }
  configured() { return !!this.one('SELECT name FROM users LIMIT 1'); }
  sessionDays() { return +this.kv('session_days', '30') || 30; }

  /* ---------------- login (auth.c) ---------------- */
  ip(req) { return req.headers.get('cf-connecting-ip') || '0.0.0.0'; }
  locked(req) { const f = this.one('SELECT until FROM fails WHERE ip = ?', this.ip(req)); return f && f.until > now(); }
  fail(req) {
    const ip = this.ip(req), f = this.one('SELECT count FROM fails WHERE ip = ?', ip);
    const count = (f ? f.count : 0) + 1;
    if (count >= MAX_FAILED) this.sql.exec('INSERT OR REPLACE INTO fails VALUES (?, 0, ?)', ip, now() + LOCKOUT_MS);
    else this.sql.exec('INSERT OR REPLACE INTO fails VALUES (?, ?, 0)', ip, count);
  }
  cookieSession(req, device) {
    const m = (req.headers.get('cookie') || '').match(/(?:^|;\s*)rd_session=([0-9a-f]{32})/);
    if (!m) return null;
    const s = this.one('SELECT * FROM sessions WHERE id = ? AND device = ?', m[1], device ? 1 : 0);
    if (!s || now() > s.created + this.sessionDays() * 864e5) return null;
    return s;
  }
  async basic(req) { /* routers only: → name, null (no header), or false (wrong) */
    const h = req.headers.get('authorization') || '';
    if (!/^Basic /i.test(h)) return null;
    let user = '', pass = '';
    try { [user, ...pass] = atob(h.slice(6).trim()).split(':'); pass = pass.join(':'); } catch (_) { this.fail(req); return false; }
    return this.password(req, user, pass, true);
  }
  async password(req, user, pass, device) { /* → name, or false (wrong) */
    let ok = false;
    if (device) {
      const d = this.one('SELECT hash FROM devices WHERE id = ?', user);
      ok = !!d && timingSafeEqual(d.hash, 'sha256$' + await sha256(pass));
    } else {
      const u = this.one('SELECT hash FROM users WHERE name = ?', user);
      ok = !!u && await checkPassword(u.hash, pass);
    }
    if (!ok) { this.fail(req); return false; }
    this.sql.exec('DELETE FROM fails WHERE ip = ?', this.ip(req));
    return user;
  }
  async login(req) { /* POST /login {user, password} from the login page → the session cookie */
    const origin = req.headers.get('origin');
    if (origin && new URL(origin).host !== new URL(req.url).host) return err(403, 'forbidden', 'origin not allowed');
    const b = await req.json().catch(() => null);
    if (!b || typeof b.user !== 'string' || typeof b.password !== 'string' || !b.user) return err(400, 'bad_request', 'user and password required');
    if (this.locked(req)) return err(429, 'rate_limited', 'too many failed logins, try again later');
    if (!await this.password(req, b.user, b.password, false)) return err(401, 'unauthorized', 'wrong user name or password');
    return new Response(JSON.stringify({ user: b.user }), { status: 200, headers: { 'content-type': 'application/json', 'set-cookie': this.newSession(b.user, false) } });
  }
  newSession(name, device) {
    const id = hex(rand(16));
    this.sql.exec('INSERT INTO sessions VALUES (?, ?, ?, ?)', id, name, device ? 1 : 0, now());
    return `rd_session=${id}; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=${this.sessionDays() * 86400}`;
  }

  /* ---------------- entry ---------------- */
  async fetch(req) {
    const url = new URL(req.url), p = url.pathname, m = req.method;
    if (m === 'POST' && p === '/api/v1/enroll') return this.enroll(req);

    if (p === '/ws/agent') {
      let name, cookie = null;
      const s = this.cookieSession(req, true);
      if (s) name = s.name;
      else {
        if (this.locked(req)) return err(429, 'rate_limited', 'too many failed logins');
        name = await this.basic(req);
        if (!name) return err(401, 'unauthorized', 'device credentials required');
        cookie = this.newSession(name, true);
      }
      if (!this.one('SELECT id FROM devices WHERE id = ?', name)) return err(403, 'forbidden', 'unknown device');
      return this.upgrade(req, { role: 'agent', id: name }, ['agent', 'agent:' + name], cookie);
    }

    if (!this.configured()) { /* setup mode: only the wizard */
      if (p === '/api/v1/setup') {
        if (this.env.SETUP_TOKEN && !timingSafeEqual(req.headers.get('x-setup-token') || '', this.env.SETUP_TOKEN)) {
          this.fail(req);
          return err(403, 'forbidden', 'wrong or missing setup code (the Worker\'s SETUP_TOKEN)');
        }
        return this.setup(req, null);
      }
      if (p === '/' || p === '/index.html' || p === '/login') return Response.redirect(new URL('/wizard.html' + url.search, url), 302);
      return err(503, 'setup_required', 'finish the setup wizard first (/wizard.html)');
    }

    const sess = this.cookieSession(req, false);
    const user = sess && sess.name;
    if (p === '/ws/viewer') {
      if (!user) return err(401, 'unauthorized', 'log in first');
      const origin = req.headers.get('origin');
      if (origin && new URL(origin).host !== url.host) return err(403, 'forbidden', 'origin not allowed');
      return this.upgrade(req, { role: 'viewer', user }, ['viewer'], null);
    }
    if (p.startsWith('/api/v1/')) {
      if (!user) return err(401, 'unauthorized', 'log in first');
      return this.api(req, url, user, sess);
    }
    /* pages: the login page (web/login.html), or a cookie, or off to the login page */
    if (p === '/login') {
      if (m === 'POST') return this.login(req);
      return new Response(null, { status: 200, headers: { 'x-rd-asset': '/login' } });
    }
    if (!user) {
      const back = p + url.search;
      return new Response(null, { status: 302, headers: { location: back === '/' ? '/login' : '/login?next=' + encodeURIComponent(back).replace(/%2F/g, '/') } });
    }
    return new Response(null, { status: 200, headers: { 'x-rd-asset': '/' } });
  }

  /* ---------------- REST (api.c) ---------------- */
  async api(req, url, user, sess) {
    const p = url.pathname, m = req.method;
    const body = async () => { try { return await req.json(); } catch (_) { return {}; } };
    if (p === '/api/v1/me') return json({ user, session_expires: sess.created + this.sessionDays() * 864e5, version: VERSION });
    if (p === '/api/v1/logout' && m === 'POST') {
      this.sql.exec('DELETE FROM sessions WHERE id = ?', sess.id);
      return new Response(null, { status: 204, headers: { 'set-cookie': 'rd_session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0' } });
    }
    if (p === '/api/v1/setup') return this.setup(req, user);
    if (p === '/api/v1/settings') return this.settings(req, user, sess);
    if (p === '/api/v1/devices' && m === 'GET') return json({ devices: this.all('SELECT * FROM devices ORDER BY created').map(d => this.deviceJson(d)) });
    if (p === '/api/v1/enroll/codes' && m === 'POST') {
      const b = await body();
      this.sql.exec('DELETE FROM codes WHERE expires < ?', now() - CODE_TTL);
      let code;
      do { code = normalizeCode([...rand(8)].map(x => CODE_ABC[x % CODE_ABC.length]).join('')); }
      while (this.one('SELECT code FROM codes WHERE code = ?', code));
      this.sql.exec('INSERT INTO codes VALUES (?, ?, ?, NULL)', code, String(b.name || '').slice(0, 60), now() + CODE_TTL);
      return json({ code, expires_in: CODE_TTL / 1000, panel: url.origin + '/' });
    }
    const cm = p.match(/^\/api\/v1\/enroll\/codes\/([^/]+)$/);
    if (cm && m === 'GET') {
      const k = this.one('SELECT * FROM codes WHERE code = ?', normalizeCode(decodeURIComponent(cm[1])) || '');
      if (!k || (!k.device && now() > k.expires)) return json({ state: 'expired' });
      if (k.device) return json({ state: 'used', device: k.device, online: this.online(k.device) });
      return json({ state: 'pending', expires_in: (k.expires - now()) / 1000 });
    }
    const dm = p.match(/^\/api\/v1\/devices\/([^/]+)(\/[a-z]+)?$/);
    if (!dm) return err(404, 'not_found', 'no such endpoint');
    const d = this.one('SELECT * FROM devices WHERE id = ?', decodeURIComponent(dm[1]));
    if (!d) return err(404, 'not_found', 'unknown device');
    const sub = dm[2] || '';
    if (!sub && m === 'GET') return json(this.deviceJson(d));
    if (!sub && m === 'PATCH') {
      const name = String((await body()).name || '').trim();
      if (!name || name.length > 60) return err(400, 'bad_request', 'a name of 1-60 characters');
      this.sql.exec('UPDATE devices SET name = ? WHERE id = ?', name, d.id);
      this.deviceChanged(d.id);
      return json(this.deviceJson(this.one('SELECT * FROM devices WHERE id = ?', d.id)));
    }
    if (!sub && m === 'DELETE') {
      this.endSessionsOf(d.id, 'device_removed');
      for (const ws of this.ctx.getWebSockets('agent:' + d.id)) ws.close(1000, 'removed');
      this.sql.exec('DELETE FROM devices WHERE id = ?', d.id);
      this.sql.exec('DELETE FROM sessions WHERE name = ? AND device = 1', d.id);
      this.broadcast(msg('device.removed', { id: d.id }));
      return new Response(null, { status: 204 });
    }
    if (sub === '/adb' && m === 'GET') {
      if (!this.online(d.id)) return err(409, 'offline', 'the router agent is offline');
      const st = JSON.parse(d.status || '{}');
      return json(st.adb || { usb: 'none' });
    }
    if (sub === '/cmd' && m === 'POST') {
      const b = await body();
      const allowed = ['server.restart', 'phone.wake', 'phone.sleep', 'phone.reboot', 'adb.reconnect', 'agent.reload', 'quality.default', 'agent.config', 'agent.config.get'];
      if (!allowed.includes(b.cmd)) return err(400, 'bad_request', 'unknown or missing "cmd"');
      return this.forward(d.id, 'cmd', { cmd: b.cmd, args: b.args && typeof b.args === 'object' ? b.args : {} });
    }
    if (sub === '/diag' && m === 'GET') {
      const lines = Math.max(1, Math.min(1000, +url.searchParams.get('lines') || 200));
      return this.forward(d.id, 'diag', { lines });
    }
    return err(404, 'not_found', 'no such endpoint');
  }

  /* REST → agent request, answered by its ack/error (hub_forward_cmd) */
  forward(id, type, d) {
    const ws = this.ctx.getWebSockets('agent:' + id)[0];
    if (!ws || !this.online(id)) return err(409, 'offline', 'the router agent is offline');
    const cid = this.nextCmd++;
    return new Promise(resolve => {
      const timer = setTimeout(() => { this.pending.delete(cid); resolve(err(504, 'timeout', 'the agent did not answer in time')); }, 15000);
      this.pending.set(cid, { resolve, timer });
      ws.send(JSON.stringify({ t: type, id: cid, d }));
    });
  }

  /* ---------------- setup (setup.c) ---------------- */
  async setup(req, user) {
    if (req.method === 'GET') {
      const turn = JSON.parse(this.kv('turn', '{}'));
      return json({
        platform: 'worker', version: VERSION, configured: this.configured(), username: user || '',
        turn: { cloudflare: turn.cloudflare !== false, key_id: turn.key_id || '', has_token: !!turn.api_token },
        ice_extra: this.kv('ice_extra'), host_hint: new URL(req.url).hostname,
      });
    }
    if (req.method !== 'POST') return err(404, 'not_found', 'no such endpoint');
    let b;
    try { b = await req.json(); } catch (_) { return err(400, 'bad_request', 'expected a JSON object'); }
    const uname = String(b.username || ''), pw = String(b.password || '');
    if (!/^[A-Za-z0-9._-]{1,32}$/.test(uname)) return err(400, 'bad_request', "the username may use letters, digits, '.', '_' and '-' (up to 32)");
    if ((!user || uname !== user) && !pw) return err(400, 'bad_request', 'choose a password');
    if (pw && pw.length < 8) return err(400, 'bad_request', 'the password needs at least 8 characters');
    const old = JSON.parse(this.kv('turn', '{}'));
    const t = b.turn || {};
    const turn = t.cloudflare ? { cloudflare: true, key_id: String(t.key_id || '').trim(), api_token: String(t.api_token || '').trim() || old.api_token || '' } : { cloudflare: false };
    if (turn.cloudflare && (!turn.key_id || !turn.api_token)) return err(400, 'bad_request', 'the Cloudflare TURN key ID and API token are both needed');
    const extra = String(b.ice_extra || '').replace(/[\r\n]+/g, ', ');
    if (!turn.cloudflare && !parseIce(extra).length) return err(400, 'bad_request', 'add at least one STUN or TURN server');
    if (turn.cloudflare) { /* check the key now rather than at the first session */
      const servers = await this.cloudflareIce(turn);
      if (!servers) return err(400, 'bad_request', 'Cloudflare refused the TURN key ID / API token');
    }
    if (pw) this.sql.exec('INSERT OR REPLACE INTO users VALUES (?, ?)', uname, await hashPassword(pw));
    if (user && uname !== user) {
      this.sql.exec('DELETE FROM users WHERE name = ?', user);
      this.sql.exec('DELETE FROM sessions WHERE name = ? AND device = 0', user);
    }
    this.setKv('turn', JSON.stringify(turn));
    this.setKv('ice_extra', extra);
    return json({ ok: true });
  }

  /* The panel's Server tab: one setting at a time (setup.c: settings_patch). */
  settingsJson(user) {
    const turn = JSON.parse(this.kv('turn', '{}'));
    return {
      platform: 'worker', version: VERSION, username: user,
      turn: { cloudflare: turn.cloudflare !== false && !!turn.api_token, key_id: turn.key_id || '', has_token: !!turn.api_token },
      ice_servers: parseIce(this.kv('ice_extra')).map(s => ({ url: s.urls[0], username: s.username || '', has_credential: !!s.credential })),
    };
  }
  async settings(req, user, sess) {
    if (req.method === 'GET') return json(this.settingsJson(user));
    if (req.method !== 'PATCH' && req.method !== 'POST') return err(404, 'not_found', 'no such endpoint');
    let b;
    try { b = await req.json(); } catch (_) { return err(400, 'bad_request', 'expected a JSON object'); }
    if (!b || typeof b !== 'object') return err(400, 'bad_request', 'expected a JSON object');
    const has = k => Object.prototype.hasOwnProperty.call(b, k);
    let relogin = false;
    if (has('username') || has('password')) { /* the login changes only with the current password */
      if (this.locked(req)) return err(429, 'rate_limited', 'too many wrong passwords, try again later');
      const row = this.one('SELECT hash FROM users WHERE name = ?', user);
      if (!row || !(await checkPassword(row.hash, String(b.current_password || '')))) { this.fail(req); return err(403, 'wrong_password', 'the current password is wrong'); }
      const uname = has('username') ? String(b.username) : user, pw = has('password') ? String(b.password) : '';
      if (!/^[A-Za-z0-9._-]{1,32}$/.test(uname)) return err(400, 'bad_request', "the username may use letters, digits, '.', '_' and '-' (up to 32)");
      if (has('password') && pw.length < 8) return err(400, 'bad_request', 'the password needs at least 8 characters');
      if (uname !== user && this.one('SELECT name FROM users WHERE name = ?', uname)) return err(409, 'bad_request', 'that username is taken');
      this.sql.exec('INSERT OR REPLACE INTO users VALUES (?, ?)', uname, pw ? await hashPassword(pw) : row.hash);
      if (uname !== user) {
        this.sql.exec('DELETE FROM users WHERE name = ?', user);
        this.sql.exec('DELETE FROM sessions WHERE name = ? AND device = 0', user);
        user = uname; relogin = true;
      }
    }
    const old = JSON.parse(this.kv('turn', '{}'));
    let turn = old, extra = this.kv('ice_extra');
    if (has('turn')) {
      const t = b.turn || {};
      turn = t.cloudflare ? { cloudflare: true, key_id: String(t.key_id ?? old.key_id ?? '').trim(), api_token: String(t.api_token || '').trim() || old.api_token || '' } : { cloudflare: false };
      if (turn.cloudflare && (!turn.key_id || !turn.api_token)) return err(400, 'bad_request', 'the Cloudflare TURN key ID and API token are both needed');
      if (turn.cloudflare && (turn.key_id !== old.key_id || turn.api_token !== old.api_token) && !(await this.cloudflareIce(turn)))
        return err(400, 'bad_request', 'Cloudflare refused the TURN key ID / API token');
    }
    if (has('ice_servers')) {
      if (!Array.isArray(b.ice_servers) || b.ice_servers.length > 32) return err(400, 'bad_request', 'ice_servers must be a list of up to 32 servers');
      const prev = parseIce(this.kv('ice_extra')), tok = s => /^[^\s,]*$/.test(s) && s.length <= 250;
      const out = [];
      for (const e of b.ice_servers) {
        const url = String(e.url || ''), u = String(e.username || '');
        let cred = String(e.credential || '');
        if (!/^(stun|turns?):./.test(url) || !tok(url) || !tok(u) || !tok(cred)) return err(400, 'bad_request', 'a STUN/TURN server looks wrong (e.g. stun:203.0.113.10:3478)');
        if (u && !cred) { const o = prev.find(x => x.urls[0] === String(e.was || url) && x.username === u); if (o) cred = o.credential; }
        out.push(u && cred ? `${url} ${u} ${cred}` : url);
      }
      extra = out.join(', ');
    }
    if (!turn.cloudflare && !parseIce(extra).length) return err(400, 'bad_request', 'keep at least one STUN or TURN server (or Cloudflare TURN)');
    this.setKv('turn', JSON.stringify(turn));
    this.setKv('ice_extra', extra);
    return json({ ...this.settingsJson(user), ok: true, restart_ms: 0, relogin });
  }

  /* Short-lived TURN credentials from Cloudflare Realtime (1,000 GB/month free). */
  async cloudflareIce(turn) {
    const base = `https://rtc.live.cloudflare.com/v1/turn/keys/${encodeURIComponent(turn.key_id)}/credentials`;
    const init = { method: 'POST', headers: { authorization: `Bearer ${turn.api_token}`, 'content-type': 'application/json' }, body: JSON.stringify({ ttl: 86400 }) };
    try {
      let r = await fetch(`${base}/generate-ice-servers`, init);
      if (r.status === 404) r = await fetch(`${base}/generate`, init); /* older API: one server object */
      if (!r.ok) return null;
      const s = (await r.json()).iceServers;
      return Array.isArray(s) ? s : s ? [s] : null;
    } catch (_) { return null; }
  }
  async iceServers() {
    const turn = JSON.parse(this.kv('turn', '{}'));
    const out = turn.cloudflare && turn.api_token ? (await this.cloudflareIce(turn)) || [] : [];
    return out.concat(parseIce(this.kv('ice_extra')));
  }

  /* ---------------- routers (enroll.c) ---------------- */
  async enroll(req) {
    if (!this.configured()) return err(503, 'setup_required', 'finish the setup wizard first');
    if (this.locked(req)) return err(429, 'rate_limited', 'too many wrong codes, try again later');
    let b = {};
    try { b = await req.json(); } catch (_) {}
    const code = normalizeCode(b.code);
    const k = code && this.one('SELECT * FROM codes WHERE code = ?', code);
    if (!k || k.device || now() > k.expires) {
      this.fail(req);
      return err(404, 'bad_code', 'this code is wrong, used or expired; make a new one on the panel (Agents → Add)');
    }
    const name = (k.name || String(b.name || '') || 'router').slice(0, 60);
    let base = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 32) || 'router', id = base;
    for (let i = 2; this.one('SELECT id FROM devices WHERE id = ?', id); i++) id = `${base.slice(0, 28)}-${i}`;
    const secret = b64url(rand(24));
    this.sql.exec('INSERT INTO devices (id, name, hash, created) VALUES (?, ?, ?, ?)', id, name, 'sha256$' + await sha256(secret), now());
    this.sql.exec('UPDATE codes SET device = ? WHERE code = ?', id, code);
    this.deviceChanged(id);
    return json({ device_id: id, device_secret: secret, name, ws_path: '/ws/agent' });
  }

  /* ---------------- devices and viewers (hub.c) ---------------- */
  online(id) {
    const d = this.one('SELECT hello FROM devices WHERE id = ?', id);
    return !!(d && d.hello && this.ctx.getWebSockets('agent:' + id).length);
  }
  deviceJson(d) {
    const online = this.online(d.id), hello = JSON.parse(d.hello || 'null'), status = JSON.parse(d.status || '{}');
    const vs = this.one('SELECT * FROM vsessions WHERE device = ?', d.id);
    return {
      id: d.id, name: d.name, online, last_seen: d.last_seen || 0,
      platform: (hello && hello.platform) || 'android', // "windows" / "macos" / "linux" for a computer
      agent: { ...(hello ? { version: hello.version || '', arch: hello.arch || '', ...(hello.os ? { os: hello.os } : {}) } : {}), ...(online ? { uptime_s: Math.round((now() - d.online_since) / 1000) } : {}), public_ip: d.public_ip || '' },
      phone: status.phone && typeof status.phone === 'object' ? status.phone : { attached: false },
      session: vs ? { active: true, sid: vs.sid, since: vs.since, viewer: vs.viewer } : { active: false },
    };
  }
  broadcast(text) { for (const ws of this.ctx.getWebSockets('viewer')) { try { ws.send(text); } catch (_) {} } }
  deviceChanged(id) {
    const d = this.one('SELECT * FROM devices WHERE id = ?', id);
    if (d) this.broadcast(msg('device.update', { id, patch: this.deviceJson(d) }));
  }
  event(id, kind, info) { this.broadcast(msg('device.event', { id, kind, ts: now(), info: info || {} })); }
  viewerOf(sid) { return this.ctx.getWebSockets('viewer').find(ws => (ws.deserializeAttachment() || {}).sid === sid) || null; }

  endSession(sid, reason, tellAgent, tellViewer) {
    const vs = this.one('SELECT * FROM vsessions WHERE sid = ?', sid);
    if (!vs) return;
    this.sql.exec('DELETE FROM vsessions WHERE sid = ?', sid);
    const agent = this.ctx.getWebSockets('agent:' + vs.device)[0];
    if (tellAgent && agent) agent.send(msg('session.end', { sid, reason }, { sid }));
    const viewer = this.viewerOf(sid);
    if (viewer) {
      if (tellViewer) viewer.send(msg('session.ended', { reason }, { sid }));
      viewer.serializeAttachment({ ...viewer.deserializeAttachment(), sid: null });
    }
    this.deviceChanged(vs.device);
  }
  endSessionsOf(device, reason) { for (const vs of this.all('SELECT sid FROM vsessions WHERE device = ?', device)) this.endSession(vs.sid, reason, true, true); }

  upgrade(req, att, tags, cookie) {
    if ((req.headers.get('upgrade') || '').toLowerCase() !== 'websocket') return err(400, 'bad_request', 'expected a WebSocket upgrade');
    const [client, server] = Object.values(new WebSocketPair());
    this.ctx.acceptWebSocket(server, tags);
    server.serializeAttachment({ ...att, ip: this.ip(req) });
    if (att.role === 'agent') { /* a reconnecting agent replaces its old socket */
      for (const old of this.ctx.getWebSockets('agent:' + att.id)) if (old !== server) old.close(1000, 'replaced');
    } else {
      server.send(msg('devices', { devices: this.all('SELECT * FROM devices ORDER BY created').map(d => this.deviceJson(d)) }));
    }
    return new Response(null, { status: 101, webSocket: client, headers: cookie ? { 'set-cookie': cookie } : {} });
  }

  async webSocketMessage(ws, data) {
    let m;
    try { m = JSON.parse(typeof data === 'string' ? data : new TextDecoder().decode(data)); } catch (_) { return; }
    if (!m || typeof m !== 'object') return;
    const a = ws.deserializeAttachment() || {};
    if (a.role === 'agent') return this.agentMessage(ws, a, m);
    if (a.role === 'viewer') return this.viewerMessage(ws, a, m);
  }

  async viewerMessage(ws, a, m) {
    const d = m.d || {}, id = m.id || 0;
    const reply = (t, payload) => ws.send(JSON.stringify({ t, id, d: payload }));
    if (m.t === 'session.request') {
      const dev = this.one('SELECT * FROM devices WHERE id = ?', String(d.device || ''));
      if (!dev) return reply('error', { code: 'not_found', msg: 'unknown device' });
      if (!this.online(dev.id)) return reply('error', { code: 'offline', msg: 'the router agent is offline' });
      const busy = this.one('SELECT * FROM vsessions WHERE device = ?', dev.id);
      if (busy && busy.sid !== a.sid) {
        if (!d.takeover) return reply('error', { code: 'busy', msg: 'device is already in a session' });
        this.endSession(busy.sid, 'taken_over', true, true); // "Take over": the other viewer is told why
      }
      if (a.sid) this.endSession(a.sid, 'replaced', true, false);
      const sid = 's_' + hex(rand(8));
      this.sql.exec('INSERT INTO vsessions VALUES (?, ?, ?, ?)', sid, dev.id, a.user, now());
      ws.serializeAttachment({ ...a, sid });
      const ice = await this.iceServers();
      const start = { sid, viewer: a.user, ice_servers: ice }; // a computer shows who is watching
      if (d.quality) start.quality = d.quality;
      if (d.force_relay) start.force_relay = true;
      this.ctx.getWebSockets('agent:' + dev.id)[0].send(msg('session.start', start, { sid }));
      reply('ack', { sid });
      ws.send(msg('session.ice', { ice_servers: ice }, { sid }));
      this.deviceChanged(dev.id);
    } else if (m.t === 'signal') {
      const vs = a.sid && this.one('SELECT * FROM vsessions WHERE sid = ?', a.sid);
      const agent = vs && this.ctx.getWebSockets('agent:' + vs.device)[0];
      if (!agent) return reply('error', { code: 'offline', msg: 'no active session' });
      agent.send(msg('signal', d, { sid: a.sid }));
    } else if (m.t === 'session.end') {
      if (a.sid) this.endSession(a.sid, 'viewer_left', true, false);
      if (id) reply('ack', {});
    } else if (m.t === 'ping') {
      ws.send(JSON.stringify({ t: 'pong', id, d: { ...d, server_ts: now() } }));
    } else {
      reply('error', { code: 'unsupported', msg: String(m.t) });
    }
  }

  agentMessage(ws, a, m) {
    const id = a.id, d = m.d || {};
    this.sql.exec('UPDATE devices SET last_seen = ? WHERE id = ?', now(), id);
    if (m.t === 'hello') {
      const was = this.online(id);
      this.sql.exec('UPDATE devices SET hello = ?, online_since = ?, public_ip = ? WHERE id = ?', JSON.stringify(d), now(), a.ip || '', id);
      if (d.phone && typeof d.phone === 'object') {
        const st = JSON.parse(this.one('SELECT status FROM devices WHERE id = ?', id).status || '{}');
        st.phone = d.phone;
        this.sql.exec('UPDATE devices SET status = ? WHERE id = ?', JSON.stringify(st), id);
      }
      ws.send(msg('welcome', { proto: PROTO, server_time: now(), ping_s: 20, timeout_s: 60, heartbeat_s: 30 }));
      if (!was) this.event(id, 'agent.online');
      this.deviceChanged(id);
      return;
    }
    if (!this.one('SELECT hello FROM devices WHERE id = ?', id).hello) {
      ws.send(JSON.stringify({ t: 'error', id: m.id || 0, d: { code: 'bad_request', msg: 'send hello first' } }));
      return;
    }
    if (m.t === 'status') {
      this.sql.exec('UPDATE devices SET status = ? WHERE id = ?', JSON.stringify(d), id);
      this.deviceChanged(id);
    } else if (m.t === 'event') {
      this.event(id, String(d.kind || 'unknown'), d.info);
    } else if (m.t === 'signal') {
      const vs = m.sid && this.one('SELECT * FROM vsessions WHERE sid = ? AND device = ?', m.sid, id);
      const viewer = vs && this.viewerOf(m.sid);
      if (viewer) viewer.send(msg('signal', d, { sid: m.sid }));
    } else if (m.t === 'session.ended') {
      const sid = m.sid || d.sid;
      if (sid && this.one('SELECT sid FROM vsessions WHERE sid = ? AND device = ?', sid, id)) this.endSession(sid, String(d.reason || 'phone_ended'), false, true);
    } else if (m.t === 'ack' || m.t === 'error') {
      const p = this.pending.get(m.id);
      if (!p) return;
      clearTimeout(p.timer);
      this.pending.delete(m.id);
      if (m.t === 'ack') p.resolve(json({ result: d }));
      else {
        const code = d.code || 'io_error';
        const status = { bad_request: 400, not_found: 404, offline: 409, busy: 409, adb_unauthorized: 409, too_large: 413, timeout: 504, unsupported: 501 }[code] || 500;
        p.resolve(err(status, code, d.msg || code));
      }
    }
  }

  async webSocketClose(ws, code, reason) { this.closed(ws); }
  async webSocketError(ws) { this.closed(ws); }
  closed(ws) {
    const a = ws.deserializeAttachment() || {};
    if (a.role === 'viewer') {
      if (a.sid) this.endSession(a.sid, 'viewer_left', true, false);
    } else if (a.role === 'agent') {
      if (this.ctx.getWebSockets('agent:' + a.id).some(w => w !== ws && w.readyState === 1)) return; /* replaced */
      const had = this.one('SELECT hello FROM devices WHERE id = ?', a.id);
      this.sql.exec('UPDATE devices SET hello = NULL WHERE id = ?', a.id);
      for (const vs of this.all('SELECT sid FROM vsessions WHERE device = ?', a.id)) this.endSession(vs.sid, 'agent_offline', false, true);
      if (had && had.hello) { this.event(a.id, 'agent.offline'); this.deviceChanged(a.id); }
    }
  }
}
