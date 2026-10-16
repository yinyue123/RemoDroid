# WebRTC guide: running RemoDroid without your own server

RemoDroid normally uses `remodroid-server` on a VPS for login, signaling and TURN. Most people
don't have a server. This guide shows how to replace it with:

- **a Cloudflare Worker** for signaling (the WebRTC "negotiation"), login and hosting the web
  client over **HTTPS**, and
- **a TURN server that you provide** to your users.

Video, audio and touch input never pass through Cloudflare. They go directly between the
iPhone and the phone, or through your TURN server when a direct path isn't possible.

---

## 1. What WebRTC needs, and who provides it

| Piece | What it does | Traffic | Who provides it here |
|---|---|---|---|
| **Signaling** | Delivers the SDP offer and answer and the ICE candidates between the phone and the browser | A few KB per connection | **Cloudflare Worker** |
| **STUN** | Tells each side its public address | A few packets | Your TURN server (TURN servers also answer STUN) |
| **TURN** | Relays the media when the two sides can't reach each other directly (carrier NAT, strict firewalls) | The whole stream, 2–20 Mbit/s | **You** |
| **Media** | DTLS-SRTP video, SCTP data channels | The whole stream | Peer to peer, or via TURN |
| **Web page** | The iPhone client (`web/`) | Once | **Cloudflare Worker** (static assets) |

Signaling is small and bursty, so it fits a Worker. TURN carries the stream, so it doesn't:
Workers can't relay UDP, so TURN has to run somewhere else.

```
                 HTTPS / WSS (signaling only)
  iPhone Safari ◄───────────────────────────► Cloudflare Worker ◄──── WSS ──── router agent ── USB ── phone
        │                                      (Durable Object hub)
        │                 media (DTLS-SRTP over UDP)
        └──────────── direct, peer to peer ──────────────────────────────────────────────────────────┘
        └──────────── or via your TURN server ───────────────────────────────────────────────────────┘
```

---

## 2. Why HTTPS is required

With RemoDroid's own server you can run plain HTTP. With a Worker, HTTPS is mandatory, and it
also makes the iPhone side better:

- **Cloudflare serves HTTPS automatically** on `*.workers.dev` and on custom domains.
- **Secure context:** on HTTPS Safari enables `navigator.clipboard` (one-tap clipboard sync
  without the paste box), `crypto.subtle` (fast SHA-256 for uploads), the screen wake lock, and
  service workers (a proper offline-capable Home Screen app).
- **Safe signaling:** the SDP carries the DTLS certificate fingerprint. Over plain HTTP, anyone
  on the path could swap it and intercept the video. Over HTTPS they can't.
- **Credentials:** the login password and session cookie can't be read in transit.

TLS only protects signaling, which is a few messages at connect time. It adds nothing to stream
latency, because media goes over WebRTC's own encrypted UDP path.

---

## 3. The TURN server you provide

Your users need STUN and TURN addresses plus credentials. Options:

| Option | Notes |
|---|---|
| **`remodroid-server` with only `[turn]` enabled** | The built-in relay. Run it on any small VPS: one static binary, UDP 3478 plus a relay port range. |
| **coturn** | Standard open-source TURN server. Use `use-auth-secret` and `static-auth-secret` (TURN REST credentials). |
| **A hosted TURN service** | Cloudflare also sells a TURN service; check its current pricing and limits before relying on it. |

Rules that matter:

- **UDP.** RemoDroid's phone-side ICE client speaks TURN over UDP only (`turn:host:3478?transport=udp`).
  Safari also supports `turns:` (TLS on 443) for networks that block UDP; you can list both, and
  the phone will use the UDP one.
- **Public address.** The relayed address must be reachable from the internet. On cloud VMs, set
  the public IP (`public_ip` in `server.ini`, `external-ip` in coturn).
- **Short-lived credentials.** Never give users the shared secret. The Worker turns it into
  credentials valid for about an hour (§5.3), in the same format the built-in relay and coturn accept:

  ```
  username   = "<unix expiry>:<user>"
  credential = base64(HMAC-SHA1(shared_secret, username))
  ```

- **Bandwidth.** A relayed session moves the full stream twice (in and out). At 8 Mbit/s that's
  about 3.6 GB per hour each way.

Built-in relay on a VPS, `server.ini`:

```ini
[http]
listen = 127.0.0.1:8080          ; not used by clients in this setup
[turn]
listen = 0.0.0.0:3478
public_ip = 203.0.113.10
relay_min_port = 49152
relay_max_port = 49252
shared_secret = <long random string, also stored as a Worker secret>
```

---

## 4. The Worker: what it must do

The Worker replaces `remodroid-server`'s signaling role by speaking the **same WebSocket
protocols** (plan Appendix A.4 and A.5). The router agent and the web client then need no
protocol changes.

| Endpoint | Purpose |
|---|---|
| `GET /` and static files | Serves `web/` (Workers static assets) |
| `GET /login` | HTTP Basic auth, then sets a signed session cookie (`HttpOnly; Secure; SameSite=Strict`) |
| `GET /ws/viewer` | Browser WebSocket: devices, `session.request`, `signal`, `session.end` |
| `GET /ws/agent` | Router agent WebSocket: `hello`, `status`, `event`, `signal`, `session.ended` |
| `GET /api/v1/me`, `/api/v1/devices` | The small REST subset the web client calls |

One **Durable Object per device** holds the agent's socket and the current viewer's socket and
forwards messages between them. Durable Objects are needed because a plain Worker invocation
can't keep a WebSocket that another request can write to.

### 4.1 Message flow

```
viewer → hub   {"t":"session.request","id":1,"d":{"device":"router1"}}
hub → agent    {"t":"session.start","sid":"s_…","d":{"sid":"s_…","ice_servers":[…TURN creds…]}}
hub → viewer   {"t":"ack","id":1,"d":{"sid":"s_…"}}
hub → viewer   {"t":"session.ice","d":{"ice_servers":[…]}}
agent → hub    {"t":"signal","sid":"s_…","d":{"type":"offer","sdp":"…"}}        → viewer
viewer → hub   {"t":"signal","d":{"type":"answer","sdp":"…"}}                    → agent
both           {"t":"signal","d":{"type":"candidate","candidate":"…"}}          → the other side
viewer → hub   {"t":"session.end"}                                                → agent
```

The hub only routes messages and never inspects SDP. The phone is always the offerer and the
DTLS client (`a=setup:active`).

---

## 5. Reference Worker

> **The finished version is in [`worker/`](worker/)**, tested by `tests/worker.mjs`:
> - one Durable Object holds every WebSocket, with its own SQLite storage;
> - setup wizard, Basic login + cookies, router enrollment codes, rename/remove;
> - command forwarding to agents;
> - Cloudflare TURN (or other STUN/TURN servers) handed out per session;
> - the same `web/` pages as `remodroid-server`.
>
> Deploy it with `cd worker && npm install && npx wrangler deploy`. The sketch below shows the shape
> of the idea in fewer lines.

### 5.1 `wrangler.toml`

```toml
name = "remodroid"
main = "src/index.js"
compatibility_date = "2026-01-01"

[assets]
directory = "../web"            # the RemoDroid web client
binding = "ASSETS"
run_worker_first = true          # so /ws/* and auth go through the Worker

[[durable_objects.bindings]]
name = "HUB"
class_name = "Hub"

[[migrations]]
tag = "v1"
new_sqlite_classes = ["Hub"]

[vars]
TURN_URLS = "turn:203.0.113.10:3478?transport=udp"
SESSION_DAYS = "30"
```

Secrets (never in the file):

```sh
wrangler secret put USER_PASSWORD       # viewer login, user "me"
wrangler secret put DEVICE_SECRETS      # JSON: {"router1":"<secret>"}
wrangler secret put COOKIE_KEY          # random, signs session cookies
wrangler secret put TURN_SECRET         # same as the TURN server's shared secret
```

### 5.2 `src/index.js`: routing and login

```js
import { Hub } from './hub.js';
export { Hub };

const enc = s => new TextEncoder().encode(s);
const b64 = buf => btoa(String.fromCharCode(...new Uint8Array(buf)));

async function hmac(keyText, data, hash = 'SHA-256') {
  const key = await crypto.subtle.importKey('raw', enc(keyText), { name: 'HMAC', hash }, false, ['sign']);
  return crypto.subtle.sign('HMAC', key, enc(data));
}
function same(a, b) { // constant-time string compare
  const x = enc(a), y = enc(b);
  return x.length === y.length && crypto.subtle.timingSafeEqual(x, y);
}

/* Stateless session cookie: "<user>.<expiry>.<hmac>". */
async function makeCookie(env, user) {
  const exp = Date.now() + Number(env.SESSION_DAYS) * 864e5;
  const sig = b64(await hmac(env.COOKIE_KEY, `${user}.${exp}`));
  return `rd_session=${user}.${exp}.${sig}; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=${env.SESSION_DAYS * 86400}`;
}
async function cookieUser(env, req) {
  const m = /(?:^|;\s*)rd_session=([^;]+)/.exec(req.headers.get('Cookie') || '');
  if (!m) return null;
  const [user, exp, sig] = m[1].split('.');
  if (!sig || Date.now() > Number(exp)) return null;
  return same(sig, b64(await hmac(env.COOKIE_KEY, `${user}.${exp}`))) ? user : null;
}
function basic(req) {
  const h = req.headers.get('Authorization') || '';
  if (!h.startsWith('Basic ')) return null;
  const [u, ...p] = atob(h.slice(6)).split(':');
  return { user: u, pass: p.join(':') };
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);

    if (url.pathname === '/ws/agent') { // router agent: device id + secret via Basic auth
      const b = basic(req), devices = JSON.parse(env.DEVICE_SECRETS);
      if (!b || !devices[b.user] || !same(b.pass, devices[b.user])) return new Response('unauthorized', { status: 401 });
      return env.HUB.get(env.HUB.idFromName(b.user)).fetch(req, { headers: { 'X-Role': 'agent', 'X-Device': b.user } });
    }

    let user = await cookieUser(env, req);
    let setCookie = null;
    if (!user) {
      const b = basic(req);
      if (b && b.user === 'me' && same(b.pass, env.USER_PASSWORD)) {
        user = 'me';
        setCookie = await makeCookie(env, user);
      }
    }
    if (!user) return new Response('Login required', { status: 401, headers: { 'WWW-Authenticate': 'Basic realm="RemoDroid"' } });

    if (url.pathname === '/ws/viewer') {
      if (req.headers.get('Origin') !== url.origin) return new Response('forbidden', { status: 403 });
      const device = url.searchParams.get('device') || Object.keys(JSON.parse(env.DEVICE_SECRETS))[0];
      return env.HUB.get(env.HUB.idFromName(device)).fetch(req, { headers: { 'X-Role': 'viewer', 'X-Device': device, 'X-User': user } });
    }
    if (url.pathname === '/api/v1/me')
      return Response.json({ user, session_expires: Date.now() + Number(env.SESSION_DAYS) * 864e5 });

    const res = await env.ASSETS.fetch(req); // the web client
    if (!setCookie) return res;
    const r = new Response(res.body, res);
    r.headers.append('Set-Cookie', setCookie);
    return r;
  },
};

export async function turnCredentials(env, user, ttl = 3600) {
  const username = `${Math.floor(Date.now() / 1000) + ttl}:${user}`;
  return { urls: env.TURN_URLS.split(','), username, credential: b64(await hmac(env.TURN_SECRET, username, 'SHA-1')) };
}
```

### 5.3 `src/hub.js`: one Durable Object per device

```js
import { DurableObject } from 'cloudflare:workers';
import { turnCredentials } from './index.js';

export class Hub extends DurableObject {
  async fetch(req) {
    const role = req.headers.get('X-Role'), device = req.headers.get('X-Device');
    const [client, server] = Object.values(new WebSocketPair());
    if (role === 'agent') this.ctx.getWebSockets('agent').forEach(ws => ws.close(1000, 'replaced'));
    // Hibernation API: the object can sleep between messages without dropping the sockets.
    this.ctx.acceptWebSocket(server, [role]);
    server.serializeAttachment({ role, device, user: req.headers.get('X-User') });
    if (role === 'viewer') this.sendDevices(server, device);
    return new Response(null, { status: 101, webSocket: client });
  }

  agent() { return this.ctx.getWebSockets('agent')[0]; }
  viewer() { return this.ctx.getWebSockets('viewer')[0]; }
  send(ws, m) { try { ws && ws.send(JSON.stringify(m)); } catch (_) {} }

  async sendDevices(ws, device) {
    const status = (await this.ctx.storage.get('status')) || {};
    this.send(ws, { t: 'devices', d: { devices: [{ id: device, name: device, online: !!this.agent(),
      phone: status.phone || { attached: false }, session: { active: !!(await this.ctx.storage.get('sid')) } }] } });
  }

  async webSocketMessage(ws, data) {
    let m; try { m = JSON.parse(data); } catch (_) { return; }
    const me = ws.deserializeAttachment(), d = m.d || {};
    const sid = await this.ctx.storage.get('sid');

    if (me.role === 'agent') {
      if (m.t === 'hello') this.send(ws, { t: 'welcome', d: { proto: 1, server_time: Date.now(), ping_s: 20, timeout_s: 60, heartbeat_s: 30 } });
      if (m.t === 'hello' || m.t === 'status') {
        await this.ctx.storage.put('status', m.t === 'status' ? d : { phone: d.phone });
        const v = this.viewer(); if (v) this.sendDevices(v, me.device);
      }
      if (m.t === 'event') this.send(this.viewer(), { t: 'device.event', d: { id: me.device, ...d } });
      if (m.t === 'signal' && m.sid === sid) this.send(this.viewer(), m);
      if (m.t === 'session.ended' && m.sid === sid) {
        await this.ctx.storage.delete('sid');
        this.send(this.viewer(), { t: 'session.ended', sid, d: { reason: d.reason } });
      }
      return;
    }

    // viewer
    if (m.t === 'session.request') {
      if (!this.agent()) return this.send(ws, { t: 'error', id: m.id, d: { code: 'offline', msg: 'the router agent is offline' } });
      const newSid = 's_' + crypto.randomUUID().replace(/-/g, '').slice(0, 16);
      await this.ctx.storage.put('sid', newSid);
      const ice = [await turnCredentials(this.env, me.user)];
      this.send(this.agent(), { t: 'session.start', sid: newSid,
        d: { sid: newSid, ice_servers: ice, quality: d.quality || {}, force_relay: !!d.force_relay } });
      this.send(ws, { t: 'ack', id: m.id, d: { sid: newSid } });
      this.send(ws, { t: 'session.ice', sid: newSid, d: { ice_servers: ice } });
    } else if (m.t === 'signal' && sid) {
      this.send(this.agent(), { t: 'signal', sid, d });
    } else if (m.t === 'session.end' && sid) {
      this.send(this.agent(), { t: 'session.end', sid, d: { sid, reason: 'viewer_left' } });
      await this.ctx.storage.delete('sid');
    }
  }

  async webSocketClose(ws) {
    const me = ws.deserializeAttachment(), sid = await this.ctx.storage.get('sid');
    if (sid && me.role === 'viewer') this.send(this.agent(), { t: 'session.end', sid, d: { sid, reason: 'viewer_left' } });
    if (sid && me.role === 'agent') this.send(this.viewer(), { t: 'session.ended', sid, d: { reason: 'agent_offline' } });
    if (sid) await this.ctx.storage.delete('sid');
  }
}
```

### 5.4 Deploy

```sh
npm create cloudflare@latest remodroid -- --type hello-world   # or: npm i -D wrangler
# put wrangler.toml and src/ from above in place, then:
wrangler secret put USER_PASSWORD
wrangler secret put DEVICE_SECRETS
wrangler secret put COOKIE_KEY
wrangler secret put TURN_SECRET
wrangler deploy                      # → https://remodroid.<you>.workers.dev
```

Open the `workers.dev` URL on the iPhone, log in, then Share → Add to Home Screen.

---

## 6. Changes RemoDroid needs for this setup

| Change | Why | Status |
|---|---|---|
| **TLS in the agent's WebSocket client** (`wss://` URLs in `agent.ini`) | Cloudflare terminates TLS; the agent must connect with `wss://` (TLS 1.2 via mbedTLS; CA bundle or `[server] ca_file`) | Done |
| Web client: `wss://` on HTTPS pages | Already handled: `web/app.js` picks `wss://` when the page is HTTPS | Done |
| Several phones | `worker/` keeps all routers in one Durable Object, so the web client needs no `?device=` | Done |
| REST admin commands (`/api/v1/devices/{id}/cmd`) | `worker/` forwards them to the agent and waits for its `ack`, as `remodroid-server` does | Done |

Nothing changes on the phone. The phone stack only sees `session.start`, `signal` and
`session.end` arriving through the agent, whatever the agent is connected to.

---

## 7. Checklist and troubleshooting

- **The page loads but the stream never starts.** Settings → Logs → Protocol messages shows whether the
  offer arrived. No offer usually means the agent isn't connected to the Worker; check `logread`
  on the router.
- **Works on home Wi-Fi, fails on 5G.** That's carrier NAT, so it needs TURN. Check the TURN server's UDP
  ports are open and `public_ip` is right. "Always relay through AWS" in Settings → Connection forces
  TURN, which is a good way to test it.
- **ICE fails everywhere.** The TURN credentials may be wrong. The Worker's `TURN_SECRET` must match the TURN
  server's shared secret, and the clocks must be roughly right because credentials expire.
- **Clipboard still needs the paste box.** The page isn't in a secure context. Make sure you opened the
  `https://` URL.
- **Costs.** Signaling traffic on Cloudflare is tiny. The TURN server's bandwidth is the real cost, and
  only for sessions that can't connect directly.

---

## 8. Walkthrough: one connection from start to finish

This follows a single connection with concrete examples. The messages and byte layouts are
RemoDroid's real formats. Addresses, keys and IDs are made up:

| Who | Address |
|---|---|
| iPhone on 5G (behind carrier NAT) | public `198.51.100.7` |
| Router at home | public `203.0.113.50`, LAN `192.168.1.1` |
| TURN server | `203.0.113.10:3478` |
| Signaling | `https://remodroid.example.workers.dev`, or `remodroid-server`, which is identical on the wire |

Timeline of a typical connection, as measured on the test machine (`tests/e2e_webrtc.py`, `tests/e2e_browser.py`):

| t (ms) | Step |
|---|---|
| 0 | Viewer asks for a session (step 3) |
| ~5 | Phone sends the offer and starts gathering candidates (steps 4–5) |
| ~100 | Answer arrives; connectivity checks start (steps 6–8) |
| ~250 | ICE connected; DTLS handshake (step 9) |
| ~300 | SRTP keys ready; SCTP association and data channels up (steps 10–11) |
| ~350 | First keyframe on screen (step 12) |

### Step 0. The router agent is already online

Long before anyone watches, the agent keeps one WebSocket open to the signaling server:

```
agent → server   GET /ws/agent   Authorization: Basic cm91dGVyMTpzM2NyZXQ=      ("router1:s3cret")
server → agent   101 Switching Protocols   Set-Cookie: rd_session=…
agent → server   {"t":"hello","d":{"proto":1,"version":"0.1.0","arch":"mipsel","phone":{"attached":true,"model":"Pixel 8","server":"running"}}}
server → agent   {"t":"welcome","d":{"proto":1,"ping_s":20,"timeout_s":60,"heartbeat_s":30}}
agent → server   {"t":"status","d":{"phone":{…,"battery":{"level":82}},"adb":{"auth":"authorized"}}}     (every 30 s)
```

A WebSocket ping every 20 s keeps the home router's NAT entry alive, so the server can always
reach the agent. No inbound port is opened at home.

### Step 1. The iPhone logs in (HTTPS)

```
iPhone → server  GET /                         → 401  WWW-Authenticate: Basic realm="RemoDroid"
iPhone → server  GET /  Authorization: Basic …  → 200  Set-Cookie: rd_session=…; HttpOnly; Secure; SameSite=Strict; Max-Age=2592000
```

Safari shows its own login prompt once. The cookie covers the next 30 days, including the
WebSocket in the next step.

### Step 2. The web client opens its WebSocket

```
iPhone → server  GET /ws/viewer   (Cookie + Origin checked)
server → iPhone  {"t":"devices","d":{"devices":[{"id":"router1","online":true,"phone":{"server":"running",…},"session":{"active":false}}]}}
```

The device is online and the phone server is running, so the client starts a session.

### Step 3. Session request and TURN credentials

```
iPhone → server  {"t":"session.request","id":1,"d":{"device":"router1","quality":{"mode":"auto","preset":"720p","fps":60,"kbps":8000,"codec":"auto"}}}
server → agent   {"t":"session.start","sid":"s_7f3a9c0e1b2d4f60","d":{"sid":"s_7f3a9c0e1b2d4f60",
                   "ice_servers":[{"urls":["turn:203.0.113.10:3478?transport=udp"],
                                   "username":"1792180800:me","credential":"q8Vd0kF2…="}],
                   "quality":{…}}}
server → iPhone  {"t":"ack","id":1,"d":{"sid":"s_7f3a9c0e1b2d4f60"}}
server → iPhone  {"t":"session.ice","sid":"s_7f3a9c0e1b2d4f60","d":{"ice_servers":[…same…]}}
```

`credential` is `base64(HMAC-SHA1(TURN_SECRET, "1792180800:me"))`. The TURN server recomputes it,
so it needs no user database. It stops working at Unix time 1792180800, one hour later.

The agent resolves host names in `urls` before passing the message on, because the phone may have
no DNS of its own. It then forwards the message over USB as a tunnel `CTRL` frame.

```
tunnel frame:  00 nn | 30 | 00 | 00 | {"t":"session.start",…}
               len     CTRL  sock  af   JSON payload
```

### Step 4. The phone gathers candidates

Candidates are the addresses where the phone can be reached. The phone opens one UDP socket.
On the default route (Path A) this socket really lives on the router:

```
phone → router  UDP_OPEN   00 0b | 01 | 01 | 04 | 00 00 00 00 | 00 00         (socket 1, IPv4, any address)
router → phone  UDP_BOUND  00 0b | 05 | 01 | 04 | c0 a8 01 01 | b6 2e         (192.168.1.1:46638)
```

**Host candidate.** The router's LAN address. It only helps when the iPhone is on the same home
network:

```
candidate:2163085347 1 udp 2130706431 192.168.1.1 46638 typ host
```

**Server-reflexive (srflx) candidate.** The router's public address, learned with a STUN Binding
request to the TURN server. It goes out as a `UDP_SEND` frame and the router sends the payload as real UDP:

```
phone → router  00 1d | 02 | 01 | 04 | cb 00 71 0a | 0d 96 | <20-byte STUN>      UDP_SEND to 203.0.113.10:3478
STUN request    00 01 00 00 21 12 a4 42 <12-byte transaction id>                 Binding, no attributes
STUN response   01 01 … XOR-MAPPED-ADDRESS = 203.0.113.50:46638
→ candidate:1209337164 1 udp 1694498815 203.0.113.50 46638 typ srflx raddr 0.0.0.0 rport 0
```

**Relay candidate.** An address on the TURN server that forwards to the phone:

```
→ Allocate  REQUESTED-TRANSPORT=UDP, LIFETIME=600                       (no credentials yet)
← 401 Unauthorized  REALM="remodroid"  NONCE="5b1c…"
→ Allocate  USERNAME="1792180800:me" REALM NONCE MESSAGE-INTEGRITY      key = MD5("1792180800:me:remodroid:q8Vd0kF2…=")
← Success   XOR-RELAYED-ADDRESS=203.0.113.10:49160  XOR-MAPPED-ADDRESS=203.0.113.50:46638  LIFETIME=600
→ candidate:3011245501 1 udp 16777215 203.0.113.10 49160 typ relay raddr 203.0.113.50 rport 46638
```

The priority numbers encode preference: host (126) > srflx (100) > relay (0) in the top byte.
That way ICE tries the cheapest path first.

### Step 5. The offer

The phone is always the offerer and starts sending immediately. Candidates follow separately
("trickle ICE"). This is the SDP RemoDroid sends, shortened:

```
v=0
o=- 4611686018427387 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0 1                      ← video and data share one transport
m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99 100 101
a=ice-ufrag:Xk3p9aQ1                    ← ICE credentials (step 8)
a=ice-pwd:Vb7…24 chars…
a=ice-options:trickle
a=fingerprint:sha-256 4F:2A:…:9C        ← the phone's DTLS certificate (step 9)
a=setup:active                          ← the phone is the DTLS client
a=mid:0
a=extmap:1 …abs-send-time
a=extmap:2 …transport-wide-cc-extensions-01
a=extmap:3 …playout-delay
a=sendonly
a=rtcp-mux
a=rtpmap:96 H265/90000                  ← first choice
a=fmtp:96 level-id=123;profile-id=1;tier-flag=0;tx-mode=SRST
a=rtpmap:97 rtx/90000                   ← retransmissions for 96
a=fmtp:97 apt=96
a=rtpmap:98 H264/90000                  ← Constrained High
a=fmtp:98 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=640c2a
a=rtpmap:100 H264/90000                 ← Constrained Baseline
a=fmtp:100 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e02a
a=rtcp-fb:96 nack / nack pli / ccm fir / transport-cc / goog-remb     (for each codec)
a=ssrc-group:FID 2864434397 1311768467  ← video SSRC + its RTX SSRC
m=application 9 UDP/DTLS/SCTP webrtc-datachannel
a=sctp-port:5000
a=max-message-size:262144
```

It travels as `{"t":"signal","sid":"s_7f3a…","d":{"type":"offer","sdp":"v=0\r\n…"}}`: phone → agent
(CTRL frame) → server → iPhone.

### Step 6. The iPhone answers

The web client runs the standard browser API:

```js
await pc.setRemoteDescription({ type: 'offer', sdp });
transceiver.setCodecPreferences([...H265, ...H264, ...rtx]); // H.265 only if decoded in hardware (§5.6 of the plan)
await pc.setLocalDescription(await pc.createAnswer());
ws.send(JSON.stringify({ t: 'signal', d: { type: 'answer', sdp: pc.localDescription.sdp } }));
```

The answer keeps only what Safari supports:

```
m=video 9 UDP/TLS/RTP/SAVPF 96 97 98 99
a=ice-ufrag:pQ7z
a=ice-pwd:…
a=fingerprint:sha-256 A1:77:…:0E        ← Safari's certificate
a=setup:passive                         ← Safari is the DTLS server
a=extmap:2 …transport-wide-cc-extensions-01
a=recvonly
a=rtpmap:96 H265/90000                  ← H.265 accepted
a=rtcp-fb:96 nack / nack pli / transport-cc
m=application 9 UDP/DTLS/SCTP webrtc-datachannel
a=sctp-port:5000
```

The phone parses the answer and picks the codec in its own preference order among what Safari
kept: here H.265, PT 96 with RTX on PT 97. It also reads the header extensions Safari accepted,
the ICE credentials and the fingerprint.

### Step 7. Candidates trickle both ways

```
phone → iPhone   {"t":"signal","d":{"type":"candidate","candidate":"candidate:1209337164 1 udp 1694498815 203.0.113.50 46638 typ srflx …","sdpMid":"0"}}
phone → iPhone   {"t":"signal","d":{"type":"candidate","candidate":null}}                      ← gathering finished
iPhone → phone   {"t":"signal","d":{"type":"candidate","candidate":"candidate:9 1 udp 2122260223 4b3f…e1.local 61231 typ host"}}
iPhone → phone   {"t":"signal","d":{"type":"candidate","candidate":"candidate:5 1 udp 1686052607 198.51.100.7 40122 typ srflx …"}}
```

Safari hides its LAN address behind a random `.local` name (mDNS), and the phone skips it. It
doesn't matter: as soon as Safari's checks arrive, the phone learns the real source address as a
**peer-reflexive** candidate.

### Step 8. Connectivity checks (ICE)

Each side pairs its candidates with the other side's and sends a STUN Binding request on every
pair, highest priority first, one every 20 ms. Here is a phone check towards Safari's srflx
candidate:

```
Binding request   00 01 00 54 21 12 a4 42 <tid>
  USERNAME         "pQ7z:Xk3p9aQ1"            (remote ufrag : local ufrag)
  PRIORITY         1862270975                 (prflx priority the phone would have)
  ICE-CONTROLLING  0x5e1f…                    (the phone decides)
  MESSAGE-INTEGRITY HMAC-SHA1 with Safari's ice-pwd
  FINGERPRINT      CRC32 ^ 0x5354554e
Binding success   01 01 … XOR-MAPPED-ADDRESS, MESSAGE-INTEGRITY with Safari's ice-pwd
```

Requests in both directions open holes in both NATs. Then:

- **Home Wi-Fi:** the host pair `192.168.1.1 ↔ iPhone LAN` succeeds first, with an RTT of about 2 ms.
- **5G behind carrier NAT:** direct pairs usually fail, and the relay pair through `203.0.113.10:49160` succeeds.

After the first success the phone waits about 120 ms for a better pair. It then **nominates** the
best one by repeating the check with `USE-CANDIDATE`. When that succeeds, ICE is *connected*.

On a relay pair the phone first asks the TURN server to accept traffic from Safari's address,
then binds a channel so media has only 4 bytes of overhead:

```
→ CreatePermission  XOR-PEER-ADDRESS=198.51.100.7
→ ChannelBind       CHANNEL-NUMBER=0x4000  XOR-PEER-ADDRESS=198.51.100.7:40122
media afterwards:   40 00 | 04 b0 | <1200-byte SRTP packet>       ChannelData: channel, length, data
```

Every 4–5 s the phone repeats a check on the chosen pair (consent freshness). After 10 s without
answers the state becomes "disconnected"; after 30 s the session ends.

### Step 9. DTLS handshake

The first data on the chosen pair is DTLS. The phone is the client:

```
phone  → iPhone  ClientHello        (use_srtp: SRTP_AES128_CM_HMAC_SHA1_80, extended master secret)
iPhone → phone   ServerHello, Certificate, ServerKeyExchange, CertificateRequest, ServerHelloDone
phone  → iPhone  Certificate, ClientKeyExchange, CertificateVerify, ChangeCipherSpec, Finished
iPhone → phone   ChangeCipherSpec, Finished
```

Both certificates are self-signed. Trust comes from the signaling instead: each side checks that
the SHA-256 of the peer's certificate equals the `a=fingerprint` from the SDP. That's why the
signaling must be HTTPS (§2).

Then both sides derive the SRTP keys from the handshake (RFC 5764):

```
60 bytes = TLS-PRF(master_secret, "EXTRACTOR-dtls_srtp", client_random + server_random)
         = client_key(16) | server_key(16) | client_salt(14) | server_salt(14)
phone (client) encrypts with client_key + client_salt and decrypts with server_key + server_salt
```

### Step 10. Data channels (SCTP inside DTLS)

The phone starts SCTP on port 5000 inside the DTLS connection and opens three channels with the
DCEP protocol. As DTLS client it uses even stream numbers:

```
DATA_CHANNEL_OPEN on stream 0:  03 | 00 | 00 00 | 00 00 00 00 | 00 04 | 00 00 | 63 74 72 6c
                                type  reliable,  prio  reliability   label   proto   "ctrl"
                                      ordered          param         length  length
← DATA_CHANNEL_ACK:             02
streams 2 and 4: "input" and "file"
```

When `ctrl` opens, the phone greets the viewer:

```
{"t":"session.ready","d":{"proto":1,"screen":{"w":1080,"h":2400,"rotation":0},"codec":"h265","caps":{"codecs":["h265","h264"],"max_fps":120}}}
```

### Step 11. Video (SRTP)

The phone's encoder delivers H.265 access units. Before every keyframe the phone puts the
parameter sets (VPS, SPS, PPS), so a viewer can start or recover from any keyframe. A
54 KB IDR picture becomes about 50 RTP packets:

```
RTP header       90 e0 1a 2b | 5e 6f 70 81 | aa bb cc dd
                 V=2,X=1     M=1, PT=96 (last packet of the frame), seq 0x1a2b, timestamp, SSRC
extensions       be de 00 03 | 12 4f a1 07 | 21 00 2c | 32 00 00 00
                 one-byte hdr  abs-send-time  transport-cc seq 44  playout-delay min=max=0 ("show at once")
H.265 payload    62 01 | 93 | <first 1,097 bytes of the NAL>     FU: start of an IDR_W_RADL (type 19)
                 62 01 | 13 | <middle>                           FU: middle
                 62 01 | 53 | <last bytes>                       FU: end
```

Each packet is then encrypted (AES-128 counter mode) and authenticated (HMAC-SHA1, 10-byte tag)
by SRTP. It goes `phone → router` as a `UDP_SEND` frame over USB, and then as a real UDP packet
to the iPhone or the TURN server. Total size stays under 1200 bytes, so nothing fragments.

Safari reassembles the frame, decodes it in hardware and shows it. The web client asks for a
zero jitter buffer (`jitterBufferTarget = 0`) to keep latency low.

### Step 12. The feedback loop

While streaming, Safari sends RTCP back over the same path, and the phone reacts:

| Safari sends | Example | The phone does |
|---|---|---|
| **transport-cc feedback** (~every 50 ms) | "packets 44–80 arrived at these times" | Estimates the available bandwidth from delay growth. A rising one-way delay means queues are filling, so the target bitrate goes down by 15%; otherwise it rises up to 8% per second. The new target goes to `MediaCodec.setParameters(video-bitrate)` without restarting the encoder. |
| **NACK** | `pid=0x1a40, blp=0x0005` → packets 0x1a40, 0x1a41, 0x1a43 lost | Resends them from a 1024-packet history as RTX packets (PT 97, own SSRC), if they're less than a second old |
| **PLI / FIR** | "I lost the picture" | Requests an immediate keyframe from the encoder (at most every 300 ms) |
| **Receiver Report** | fraction lost 2%, LSR/DLSR | Computes RTT; heavy loss (>10%) also lowers the bitrate |

The phone sends a Sender Report every second, so Safari can measure RTT too.

### Step 13. Touch input

A tap at the centre of the screen becomes one 14-byte message on the `input` channel:

```
01 | 00 | 00 | 01 | 80 00 | 80 00 | ff ff | 00 00 03 e8
touch down  ptr 0 last x=32768  y=32768  pressure  t=1000 ms
```

The phone maps 0..65535 onto the real screen in its current rotation and injects an Android
`MotionEvent`. Moves (action 02) follow at screen refresh rate, then an up (01).

### Step 14. Ending

```
iPhone → server  {"t":"session.end"}                       (or the WebSocket just closes)
server → agent   {"t":"session.end","sid":"s_7f3a…","d":{"reason":"viewer_left"}}
```

The phone closes DTLS and its TURN allocation (a Refresh with `LIFETIME=0`) and stops the
encoder. It turns the panel back on and, after the 10 s grace period, locks the phone. If the
iPhone reconnects within those 10 s, the whole process starts again from step 3, about a second
of work, and the phone stays unlocked.

---

## 9. Where each step lives in the code

| Step | Phone (`android/native/`) | Router (`agent/`) | Server (`server/`) | Browser (`web/app.js`) |
|---|---|---|---|---|
| 0, 2, 3 | — | `wsc.c` | `hub.c`, `auth.c` | `connectWs`, `startSession` |
| 4, 7, 8 | `ice.c`, `stun` (`common/stun.c`), `net.c` | `phone.c` (UDP relay, `UDP_BOUND`) | `turn.c` | `onicecandidate`, `addIceCandidate` |
| 5, 6 | `sdp.c`, `rtc.c` | forwards `CTRL` | forwards `signal` | `onSignal`, `preferCodecs` |
| 9 | `dtls.c` | — | — | browser |
| 10 | `sctp.c` | — | — | `ondatachannel` |
| 11, 12 | `rtp.c`, `cc.c`, `ScreenEncoder.java` | — | — | `ontrack`, stats overlay |
| 13 | `Controller.java` | — | — | `sendTouch` |
| 14 | `rtc.c`, `Session.java`, `Server.java` | `phone.c` | `hub.c` | `endSession` |

---

## 10. The standards (RFC index)

WebRTC is a bundle of separate standards. This is every one RemoDroid touches, with what it
defines. "Draft" means an IETF draft or a de-facto spec that browsers implement, not an RFC.

| Layer | Standard | What it defines |
|---|---|---|
| Overview | **RFC 8825** | The WebRTC architecture and which protocols are mandatory |
| | **RFC 8826 / RFC 8827** | Security considerations and the security architecture: everything encrypted, keys from DTLS, trust anchored in the signaling |
| | **RFC 8834 / RFC 8835** | Media transport (RTP usage) and transports (ICE, DTLS, SCTP) required of WebRTC endpoints |
| | **RFC 7742** | Video codec requirements: browsers must support H.264 Constrained Baseline and VP8 |
| Session description | **RFC 8866** | SDP syntax: `v=`, `o=`, `m=`, `a=` lines |
| | **RFC 9429** (JSEP, replaces RFC 8829) | How browsers create and apply offers and answers: `createOffer`, `setLocalDescription`, BUNDLE use, `a=setup` rules |
| | **RFC 9143** (replaces RFC 8843) | BUNDLE: several `m=` sections share one transport (`a=group:BUNDLE 0 1`) |
| | **RFC 8830** | `a=msid`: which media stream and track an `m=` section belongs to |
| | **RFC 8285** | RTP header extensions in SDP (`a=extmap`) and on the wire (one-byte `0xBEDE` and two-byte forms); `a=extmap-allow-mixed` |
| NAT traversal | **RFC 8445** | ICE: candidates, priorities, pairs, connectivity checks, nomination, controlling and controlled roles |
| | **RFC 8838** | Trickle ICE: send candidates as they're found; end-of-candidates |
| | **RFC 8839** | ICE in SDP: `a=candidate`, `a=ice-ufrag`, `a=ice-pwd`, `a=ice-options` |
| | **RFC 7675** | Consent freshness: keep checking the chosen pair (every ~5 s) and stop sending if no answer for 30 s |
| | **RFC 8489** | STUN: message format, `XOR-MAPPED-ADDRESS`, `MESSAGE-INTEGRITY`, `FINGERPRINT`, long-term credentials |
| | **RFC 8656** | TURN: `Allocate`, `Refresh`, `CreatePermission`, `ChannelBind`, Send/Data indications, ChannelData; UDP, TCP or TLS to the server |
| | draft-uberti-behave-turn-rest | TURN REST credentials: `username = expiry:user`, `password = base64(HMAC-SHA1(secret, username))` |
| | draft-ietf-mmusic-mdns-ice-candidates | Browsers hide LAN addresses behind random `.local` names |
| | **RFC 6544** | ICE TCP candidates (not used by RemoDroid) |
| Security | **RFC 6347** (DTLS 1.2), **RFC 9147** (DTLS 1.3) | TLS over datagrams: retransmission, fragmentation, replay protection |
| | **RFC 5763 / RFC 5764** | DTLS-SRTP: the framework and the `use_srtp` extension; SRTP keys exported from DTLS |
| | **RFC 8122** | `a=fingerprint` and `a=setup` (active / passive / actpass) |
| | **RFC 8842** | Offer/answer rules for DTLS in SDP; updates RFC 5763 |
| | **RFC 3711** | SRTP / SRTCP: encryption and authentication of RTP and RTCP |
| | **RFC 7714** | AES-GCM for SRTP |
| Media | **RFC 3550** | RTP and RTCP: headers, SSRC, sequence numbers, timestamps, Sender and Receiver Reports |
| | **RFC 4585** | RTP/AVPF: early feedback, generic NACK, PLI (`a=rtcp-fb`) |
| | **RFC 5104** | Codec control messages, including FIR (full intra request) |
| | **RFC 5761** (updated by RFC 8035) | `a=rtcp-mux`: RTP and RTCP on one port |
| | **RFC 5506** | Reduced-size RTCP (`a=rtcp-rsize`) |
| | **RFC 4588** | RTX retransmission payload (`rtx/90000`, `apt=`) |
| | **RFC 6184** | H.264 RTP payload: single NAL, STAP-A, FU-A, `profile-level-id`, `packetization-mode` |
| | **RFC 7798** | H.265 RTP payload: single NAL, AP (48), FU (49), `profile-id`, `level-id`, `tier-flag`, `tx-mode` |
| | **RFC 7587** | Opus RTP payload and SDP parameters (for audio, milestone M4) |
| | **RFC 5109 / RFC 8627**, **RFC 2198** | ULPFEC / FlexFEC forward error correction, RED (not used yet) |
| | webrtc.org: abs-send-time | 24-bit send timestamp (6.18 fixed-point seconds) for receive-side bandwidth estimation |
| | draft-holmer-rmcat-transport-wide-cc-extensions-01 | transport-cc: a per-packet sequence number plus RTCP feedback (RTPFB FMT 15) with arrival times |
| | webrtc.org: playout-delay | Asks the receiver for a min/max playout delay (10 ms units) |
| | 3GPP TS 26.114: video-orientation (CVO) | Signals frame rotation so the receiver rotates instead of the sender re-encoding |
| | draft-ietf-rmcat-gcc | Google Congestion Control: delay-gradient trendline plus loss-based control |
| | draft-alvestrand-rmcat-remb | REMB: the receiver's bandwidth estimate (PSFB FMT 15) |
| Data channels | **RFC 8831** | WebRTC data channels: SCTP over DTLS, reliability options, message size |
| | **RFC 8832** | DCEP: `DATA_CHANNEL_OPEN` / `ACK`, stream ID parity |
| | **RFC 8841** | SCTP in SDP: `UDP/DTLS/SCTP`, `a=sctp-port`, `a=max-message-size` |
| | **RFC 8261** | SCTP encapsulated in DTLS |
| | **RFC 9260** | SCTP itself (replaces RFC 4960) |
| | **RFC 3758**, **RFC 8260** | Partial reliability (PR-SCTP) and I-DATA interleaving |
| Signaling (RemoDroid) | **RFC 6455** | WebSocket |
| | **RFC 7617**, **RFC 6265** | HTTP Basic authentication; cookies |

---

## 11. Options: definitions, choices, and what to decide

Each option below lists the definition with the standard section, the choices, what RemoDroid
does today, and a recommendation. Settings that exist now are named with their `agent.ini` /
`server.ini` key. Settings marked **(proposed)** don't exist yet; deciding on them tells me what
to add.

### 11.1 Signaling transport and security

- **Definition.** WebRTC deliberately does not standardize signaling (RFC 8825 §5; JSEP RFC 9429 §1).
  Anything that delivers the SDP and candidates reliably works.
- **Security requirement.** RFC 8827 §6.5: the DTLS certificates are self-signed, and
  authentication relies on the integrity of the `a=fingerprint` carried by the signaling. If
  the signaling can be modified, an attacker can substitute their own fingerprint and decrypt the media.

| Choice | Effect |
|---|---|
| Plain `ws://` / `http://` (current `remodroid-server` default) | Works anywhere; fingerprint and credentials exposed |
| `wss://` / `https://` (Cloudflare Worker, or `remodroid-server` with `[acme] domains`) | Meets RFC 8827; enables Safari's secure-context APIs |

**Recommendation:** HTTPS whenever users other than you are involved. It costs nothing in stream latency.
**Keys:** `[acme] domains` (server: automatic Let's Encrypt certificate) or `[tls] cert`/`key`; `[server] url = ws://…|wss://…` (agent).

### 11.2 Who offers

- **Definition.** JSEP RFC 9429 §3.5: either side may offer; the answerer can only accept or reject
  what is offered.
- **Choices:** phone offers (current), or browser offers.
- **Trade-off.** The offerer decides the codec order, the SSRCs and the data channel layout. With
  the phone offering, the phone knows its encoder capabilities before writing the SDP (H.265
  offered only if a hardware encoder exists). With the browser offering, a Safari without H.265
  wouldn't offer it in the first place.

**Recommendation:** keep the phone as the offerer. **Key:** none (fixed).

### 11.3 Trickle ICE

- **Definition.** RFC 8838: candidates are sent incrementally after the SDP, and
  "end-of-candidates" closes the list. Advertised with `a=ice-options:trickle` (RFC 8839).
- **Choices:**
  - **Trickle (current):** the offer goes out at t≈5 ms and checks start as soon as the first candidates meet.
  - **Full gathering:** the offer waits for STUN and TURN, adding 50–300 ms or up to the
    gathering timeout (4 s) on a bad network.

**Recommendation:** trickle. **Key:** none.

### 11.4 Candidate types and relay policy

- **Definition.** RFC 8445 §5.1.1: host candidates (local interface addresses), server-reflexive
  (the NAT's public mapping, from STUN), relayed (from TURN), and peer-reflexive (learned from an
  incoming check, §7.2.5.3.1). Type preferences in priorities (§5.1.2.2): host 126, prflx 110,
  srflx 100, relay 0.
- **Browser equivalent:** `RTCConfiguration.iceTransportPolicy = "all" | "relay"`.

| Choice | When |
|---|---|
| all (current default) | Lowest latency: home Wi-Fi gets host pairs (~2 ms), many 5G networks get srflx pairs |
| relay only | Hides home and phone IPs; always works through firewalls; costs TURN bandwidth and adds a hop |
| no host candidates (proposed) | Don't reveal the router's LAN address to the viewer |

**Keys:** `[phone] force_relay` (agent side); "Always relay through AWS" in Settings → Connection
(viewer side); `[webrtc] host_candidates = on|off` **(proposed)**.

### 11.5 ICE pacing, timers and nomination

- **Pacing.** RFC 8445 §14.2: Ta is the minimum interval between new checks. The RFC's default
  is 50 ms, and it says Ta should be configurable. RemoDroid uses **20 ms**, which is faster
  because only one session runs and there are few pairs.
- **Retransmission.** RFC 8445 §14.3 and RFC 8489 §6.2.1: the initial RTO is 500 ms for STUN in
  general, with the RTO for ICE checks derived from Ta. RemoDroid retransmits at 100, 200, 400, 800 ms
  and then 1.6 s, up to 8 tries.
- **Nomination.** RFC 8445 §8.1.1: the controlling agent nominates a valid pair by repeating the
  check with `USE-CANDIDATE` ("regular nomination"). The aggressive nomination of RFC 5245 was
  removed. RemoDroid waits 120 ms after the first success for a better pair.
- **Consent.** RFC 7675 §5.1: checks every 5 s (randomized 4–6 s); stop sending after 30 s without
  a response. RemoDroid also reports "disconnected" after 10 s.

| Option | Choices | Current |
|---|---|---|
| Ta | 5–50 ms | 20 ms |
| Nomination wait | 0–500 ms (0 = take the first working pair) | 120 ms |
| Disconnected after | 5–15 s | 10 s |

**Recommendation:** keep these. Lowering the nomination wait to 0 saves ~0.1 s at connect but may
lock in a relay pair when a direct pair was about to succeed. **Keys:** `[webrtc] ice_pacing_ms`,
`nomination_wait_ms` **(proposed)**.

### 11.6 TURN transport and credentials

- **Definition.** RFC 8656 §3.1: client to server over UDP, TCP or TLS (`turn:…?transport=udp|tcp`,
  `turns:` on TLS). Relayed traffic to the peer is always UDP.
- **Lifetimes.** RFC 8656 §2.2: allocations default to 600 s and are refreshed; permissions last
  300 s; channel bindings last 600 s.
- **Framing.** Data goes in Send/Data indications (36+ bytes overhead) or ChannelData after a
  `ChannelBind` (4 bytes, §12).

| Option | Choices | Current |
|---|---|---|
| Phone → TURN transport | UDP (TCP/TLS would need code) | UDP |
| Viewer → TURN | UDP, plus `turns:…:443` as fallback for networks that block UDP | whatever `urls` lists |
| Framing | ChannelData after binding | yes, once the relay pair is chosen |
| Credential TTL | minutes to hours | 60 min (`[turn] credential_ttl_minutes`) |
| Credentials | REST shared secret (current) or static user/password | REST |

**Recommendation:** list UDP first and `turns:` on 443 second for the viewer. Keep REST credentials.

### 11.7 DTLS version, role and certificate

- **Roles.** RFC 8122 §4 / RFC 8842 §5: `a=setup:actpass` means "I can be either", `active` means
  "I'll be the DTLS client", `passive` means "I'll be the server". RFC 5763 §5 / RFC 8842 and JSEP
  RFC 9429 §5.2.1 say an initial **offer MUST use `actpass`**, and the answerer picks.
- **Versions.** DTLS 1.2 (RFC 6347) is what every browser supports today; DTLS 1.3 (RFC 9147) is
  being rolled out in browsers.
- **Certificate.** Self-signed. RFC 8827 §6.5 requires ECDSA P-256 or RSA; browsers use ECDSA P-256.

> **Important: RemoDroid currently offers `a=setup:active`. That is not what the RFCs say.**
> It was done because Chrome's ClientHello (with large post-quantum key shares) arrives fragmented,
> and mbedTLS 3.6 cannot reassemble a fragmented ClientHello when it is the DTLS *server*. Chrome
> (libwebrtc) accepts an `active` offer and answers `passive`, which the headless-Chrome test confirms.
> Safari uses libwebrtc too, so it should accept it, but that needs checking on your iPhone (M0).

| Choice | Pros | Cons |
|---|---|---|
| **A. Offer `active` (current)** | Works with Chrome today; no extra code | Violates RFC 5763/8842/JSEP; a stricter browser could reject the offer |
| **B. Offer `actpass`, answerer chooses** | Standard | Safari and Chrome answer `active` (they become the client), so the phone is the server and fails on fragmented ClientHellos, unless mbedTLS is fixed |
| **C. B plus a ClientHello reassembly patch for mbedTLS** (or a newer mbedTLS that supports it) | Standard and robust | Work in a vendored crypto library |
| **D. DTLS 1.3 later** | Smaller handshakes | mbedTLS 3.6 has no DTLS 1.3 |

**Recommendation:** keep A now, check Safari in M0, and plan C. **Key:** `[webrtc] dtls_role = active|actpass` **(proposed)**.

### 11.8 SRTP protection profile

- **Definition.** RFC 5764 §4.1.2 defines `SRTP_AES128_CM_HMAC_SHA1_80` (0x0001) and `_32` (0x0002);
  RFC 7714 adds `SRTP_AEAD_AES_128_GCM` (0x0007).
- **Overhead.** The authentication tag is 10, 4 or 16 bytes per packet. SRTCP adds 4 bytes of index.
- **Requirements.** RFC 8827 §6.5 requires `AES_CM_128_HMAC_SHA1_80` support; GCM is recommended
  where available.

| Profile | Tag | Notes |
|---|---|---|
| AES_CM_128_HMAC_SHA1_80 (current) | 10 B | Universally supported; libsrtp internal crypto |
| AES_CM_128_HMAC_SHA1_32 | 4 B | 6 B saved per packet; weaker tag; not offered by browsers by default for video |
| AEAD_AES_128_GCM | 16 B | Faster on phones with AES instructions; needs libsrtp's mbedTLS backend |

**Recommendation:** keep `_80`; consider GCM later if profiling shows SRTP cost on the phone.
**Key:** `[webrtc] srtp_profiles` **(proposed)**.

### 11.9 Video codec and its SDP parameters

- **H.265** (RFC 7798 §7.1):
  - `profile-id=1`: Main profile.
  - `tier-flag=0`: Main tier.
  - `level-id=123`: 30 × level, so 123 = level 4.1, enough for 1080p60.
  - `tx-mode=SRST`: single RTP stream on a single transport.
  - Parameter sets can be sent in-band (RemoDroid's choice, before every keyframe) or in SDP
    (`sprop-vps/sps/pps`).
- **H.264** (RFC 6184 §8.1):
  - `profile-level-id` = profile_idc, constraint flags, level_idc in hex. `42e01f` is Constrained
    Baseline 3.1; `42e02a` is Constrained Baseline 4.2; `640c2a` is Constrained High 4.2.
  - `packetization-mode=1`: non-interleaved, allows FU-A and STAP-A. Mode 0 is single NAL only and
    too restrictive.
  - `level-asymmetry-allowed=1`: the two directions may use different levels.
- **Other codecs.** RFC 7742 makes H.264 Constrained Baseline and VP8 mandatory in browsers. VP9 and
  AV1 are supported widely, but Android hardware encoders for them are rare and slow to set up.

| Option | Choices | Current |
|---|---|---|
| Codec order | H.265 → H.264 High → H.264 Baseline | yes (`[video] codec = auto|h265|h264`) |
| H.264 profile | Constrained High (better compression), Baseline (widest compatibility) | both offered, High preferred |
| Level | Limits resolution × fps the decoder accepts | 4.1 (H.265), 4.2 (H.264) |
| Parameter sets | in-band before keyframes / SDP sprop | in-band |

**Recommendation:** keep this. Choose `codec = h264` if M0 shows H.265 encodes slowly on your phone.

### 11.10 Keyframes and error recovery

- **Definitions.**
  - PLI (RFC 4585 §6.3.1): "picture loss, send a refresh".
  - FIR (RFC 5104 §4.3.1): "send a full intra picture".
  - Generic NACK (RFC 4585 §6.2.1): a PID plus a 16-bit bitmask (BLP) of lost sequence numbers.
  - RTX (RFC 4588): retransmissions use their own SSRC and payload type (`apt=` maps to the
    original), carrying the original sequence number (OSN) in the first 2 payload bytes.
- **Encoder side.** A keyframe interval of 10 s, plus immediate keyframes on PLI/FIR, rate-limited
  to one per 300 ms. Intra refresh (MediaCodec `KEY_INTRA_REFRESH_PERIOD`) spreads the refresh over
  many frames instead of one big keyframe.
- **FEC.** ULPFEC (RFC 5109), FlexFEC (RFC 8627) and RED (RFC 2198) send redundancy so losses
  can be repaired without a round trip.

| Option | Choices | Current |
|---|---|---|
| Keyframe interval | 1–60 s, or on request only | 10 s |
| Keyframe rate limit | 100–1000 ms | 300 ms |
| NACK + RTX | on / off | on (RTX if Safari accepts it) |
| NACK history | how old a packet can still be resent | 1024 packets / 1 s |
| Intra refresh | off / N frames | off |
| FEC | none / ULPFEC / FlexFEC | none |

Trade-offs:
- **NACK** repairs a loss in about one RTT. That's good on home Wi-Fi (RTT ~5 ms) and costly on
  5G (RTT ~60 ms) for a game.
- **FEC** costs 10–30% extra bitrate and repairs instantly.
- **Intra refresh** removes the 5–10× bitrate spike of a keyframe, which otherwise causes a burst
  of latency.

**Recommendation:** keep NACK+RTX, and try intra refresh in M3 if keyframe spikes are visible.
**Keys:** `[video] keyframe_interval_s`, `intra_refresh_frames`, `[webrtc] fec = none|ulpfec|flexfec`
**(proposed)**.

### 11.11 Header extensions

- **Definition.** RFC 8285: one-byte elements (ID 1–14, ≤16 bytes) after `0xBEDE`, negotiated with
  `a=extmap:<id> <uri>`.
- **Extensions RemoDroid can use:**
  - **abs-send-time:** for receive-side BWE (REMB).
  - **transport-cc:** per-packet sequence number for send-side BWE.
  - **playout-delay:** `min=0, max=0` asks the receiver to render immediately.
  - **video-orientation (CVO, 3GPP TS 26.114):** carries the rotation, so the receiver rotates the
    picture instead of the sender restarting the encoder.

| Extension | Cost/packet | Current | Notes |
|---|---|---|---|
| abs-send-time | 4 B | on | Only useful for REMB |
| transport-cc | 3 B | on | Needed for the delay-based controller |
| playout-delay | 4 B (keyframes) | on | Honoured by libwebrtc; check Safari in M0 |
| CVO | 2 B (last packet of a frame) | not implemented | Would make rotation free (no ~200 ms encoder restart) |

**Recommendation:** keep transport-cc and playout-delay; drop abs-send-time once transport-cc is
confirmed on Safari; add CVO if rotation glitches matter in games. **Key:**
`[webrtc] extensions` **(proposed)**.

### 11.12 Congestion control

- **Definitions.**
  - GCC (draft-ietf-rmcat-gcc): a delay-based estimate from the trend of one-way delay
    variation, plus a loss-based bound (decrease if loss > 10%, increase if < 2%).
  - Send-side BWE uses transport-cc feedback; receive-side uses REMB.
- **RemoDroid's controller (`android/native/cc.c`):**
  - **Signals:** a trendline over 20 packet groups with an adaptive threshold, and a loss bound.
    REMB is ignored while transport-cc feedback is arriving.
  - **Decrease:** to 85% of the acknowledged rate on overuse.
  - **Increase:** 8%/s far from the last congestion point, 50 kbit/s per second near it.
  - **Floor:** never below the start rate while the screen is app-limited, unless congestion was
    seen in the last 30 s.

| Option | Choices | Current |
|---|---|---|
| min / start / max bitrate | kbit/s | 300 / 2500 / `[video] max_bitrate_kbps` (20000) |
| Mode | auto (controller drives the encoder), manual (fixed bitrate) | `[video] mode` |
| Backoff factor | 0.7–0.9 | 0.85 |
| Ramp-up | 5–15% per second | 8% |

**Recommendation:** decide the start rate from your typical network. A higher start looks sharp
at once but can stutter for a second on slow links. **Keys:** `[video] min_bitrate_kbps`,
`start_bitrate_kbps` **(proposed)**.

### 11.13 Data channel reliability

- **Definition.** RFC 8831 §6.1:
  - Each channel is *reliable* or *partially reliable*: a maximum number of retransmissions or a
    maximum lifetime (PR-SCTP, RFC 3758).
  - Each channel is *ordered* or *unordered*.
  - DCEP (RFC 8832 §5.1) carries the choice as a channel type: 0x00 reliable ordered,
    0x80 reliable unordered, 0x01/0x81 limited retransmissions, 0x02/0x82 limited lifetime.
  - Stream IDs: the DTLS client uses even numbers, the server odd (§6).
  - Max message size: `a=max-message-size` (RFC 8841 §6), 64 KB if absent.

| Channel | Choices | Current | Trade-off |
|---|---|---|---|
| `ctrl` | reliable ordered | reliable ordered | Must not lose or reorder commands |
| `input` | reliable ordered / unordered + `maxRetransmits=0` for moves | reliable ordered | A lost packet holds back later touches until it's retransmitted (head-of-line blocking). Unreliable moves never wait, but downs/ups must stay reliable, which means two channels |
| `file` | reliable ordered | reliable ordered | Bulk; flow-controlled by `bufferedAmount` |

**Recommendation:** split input into `input` (reliable: down, up, keys) and `input-move`
(unordered, `maxRetransmits=0`) if 5G play shows touch stutter. **Key:**
`[webrtc] input_reliability = reliable|split` **(proposed)**.

### 11.14 Packet size

- **Definition.** RFC 8834 §4.4 / JSEP: endpoints must avoid IP fragmentation; ~1200 bytes is the
  common safe RTP size.
- **RemoDroid.** RTP payload ≤ 1100 bytes, so the packet stays below 1200 after SRTP. TURN
  ChannelData adds 4 bytes, IPv6 adds 20 more than IPv4, and the tunnel adds nothing on the internet side.

**Recommendation:** keep it. Raising it to 1400 saves ~1% overhead and risks fragmentation on some
mobile networks.

### 11.15 Jitter buffer

- **Definition.** Not standardized. The receiver chooses. The playout-delay extension is a hint;
  in the browser, `RTCRtpReceiver.jitterBufferTarget` (W3C webrtc-extensions) sets a target in ms.
- **RemoDroid.** Asks for 0 ms with both. The browser still buffers enough to smooth jitter.

| Choice | Effect |
|---|---|
| 0 ms (current) | Lowest latency; occasional micro-stutter on jittery networks |
| 20–50 ms | Smoother on 5G; +20–50 ms latency |

**Key:** Settings → Stream → "Jitter buffer" **(proposed)**.

---

## 12. Worked examples

Each example shows only what differs from the full walkthrough in §8.

### Example A — iPhone on the same home Wi-Fi (direct, ~2 ms RTT)

```
phone candidates:   192.168.1.1:46638 host · 203.0.113.50:46638 srflx · 203.0.113.10:49160 relay
Safari candidates:  4b3f….local host (mDNS, skipped) · 203.0.113.50:51022 srflx (same router!)
first check that arrives:  Safari 192.168.1.23:61231 → 192.168.1.1:46638
                            → the phone learns 192.168.1.23:61231 as peer-reflexive (priority from PRIORITY)
pairs that succeed:  host↔prflx (rtt 2 ms), srflx↔srflx only if the router does NAT hairpinning
nominated:           192.168.1.1:46638 ↔ 192.168.1.23:61231
```

Media stays inside the home and never reaches AWS or Cloudflare. The `UDP_BOUND` frame (§8 step 4)
is what makes this possible: without it the phone would only know the public and relay
addresses.

### Example B — iPhone on 5G behind carrier NAT (relay)

```
Safari candidates:  .local host (skipped) · 100.64.12.9:5001 srflx?  (often missing or symmetric NAT)
checks:  phone srflx → Safari srflx       ✗  no answer: the carrier NAT changes the port per destination
         phone relay → Safari srflx       ✓  after CreatePermission(198.51.100.7)
TURN log:  allocation 203.0.113.50:46638 -> relay 203.0.113.10:49160 for 1792180800:me
nominated: relay pair, rtt 38 ms
```

Then `ChannelBind 0x4000`, and all media is `40 00 <len> <SRTP>` between router and TURN server.
Cost: the full stream crosses the TURN server twice.

### Example C — Safari without H.265 (fallback to H.264)

```
offer:   m=video … 96 97 98 99 100 101        (H265, rtx, H264 High, rtx, H264 Baseline, rtx)
web client: mediaCapabilities.decodingInfo({type:"webrtc", contentType:"video/H265"}) → powerEfficient:false
            → setCodecPreferences([H264 …, rtx])
answer:  m=video 9 UDP/TLS/RTP/SAVPF 98 99 100 101
phone:   "answer: h264" → encoder configured AVCProfileConstrainedHigh, level 4.2
```

The same happens on its own for an older iOS whose WebRTC has no H.265: its answer simply leaves
out PT 96.

### Example D — a packet is lost

```
phone sends     seq 0x1a40 0x1a41 0x1a42 0x1a43 …       (0x1a41 and 0x1a43 lost on 5G)
Safari → RTCP   RTPFB FMT 1 (NACK): PID=0x1a41 BLP=0x0002   → lost: 0x1a41 and 0x1a43
phone           looks both up in the 1024-packet history (sent 45 ms ago, < 1 s) →
                RTX: PT 97, SSRC 1311768467, its own seq 0x0310, payload = 1a 41 | original payload
                RTX: … 1a 43 | original payload
Safari          frame complete ~1 RTT later → decoded
```

If the retransmission also fails, Safari sends a **PLI**. The phone then calls
`requestKeyframe()` (MediaCodec `PARAMETER_KEY_REQUEST_SYNC_FRAME`), and the next frame is an IDR
with VPS/SPS/PPS in front.

### Example E — the network gets congested

```
t=0      target 8000 kbit/s; transport-cc feedback every ~50 ms
t=3.0 s  one-way delay starts rising 2 ms per packet group (a queue is building)
         trendline slope × gain > threshold for > 10 ms → overuse
         target = 0.85 × acked rate (6900 kbit/s) = 5865 kbit/s
         → on_bitrate(5865) → MediaCodec.setParameters(video-bitrate = 5865000)   (no restart, no keyframe)
t=3.5 s  delay stops rising → normal → near the last decrease: +50 kbit/s per second
t=10 s   far above the last congestion point: +8 % per second again
```

In **manual** mode the controller still runs, but the encoder keeps your fixed bitrate, so
congestion then shows up as delay instead of blur.

### Example F — the viewer reconnects within the grace period

```
t=0     iPhone locked by the user → Safari suspends the page → WebSocket closes
        server → agent  session.end {reason:"viewer_left"}
        phone: encoder stops, panel restored, lock scheduled in 10 s
t=6 s   iPhone unlocked → web client reconnects → session.request → new sid
        phone: pending lock cancelled; new ICE/DTLS session (≈0.3–1 s); first keyframe
```

After 10 s without a new session, the phone instead injects `KEYCODE_SLEEP` and sends
`event phone.locked`. The next session then starts on the lock screen.

### Example G — the router's uplink drops for 15 seconds

```
t=0      home internet goes down; media and the agent's WebSocket stop
t=0–10 s phone's consent checks get no answer → ICE "disconnected"; Safari shows a frozen frame
t=10 s   (consent < 30 s) internet back → the next consent check succeeds → ICE "connected" again,
         same session, a PLI brings a fresh keyframe
         agent: WebSocket reconnects with backoff 1, 2, 4 … s → hello/status again
if the outage lasts > 30 s: ICE "failed" → phone sends session.ended {reason:"ice_failed"}
         → the web client shows "Disconnected" with a Reconnect button
```

If the router's public IP changes during the outage, the old pair can't recover, and the session
ends after 30 s. ICE restart (new ufrag/pwd, RFC 8445 §9) would recover without a new session, but
RemoDroid doesn't implement it yet.

### Example H — the phone rotates to landscape

```
Display 1080×2400 rotation 0 → 2400×1080 rotation 1 (game switches to landscape)
encoder thread (checks every 500 ms) sees the change → restarts the encoder at 1600×720
phone → ctrl  {"t":"screen.changed","d":{"w":1600,"h":720,"rotation":1}}
first frame after restart is a keyframe with new SPS (new resolution) → Safari resizes the video
glitch: ~100–300 ms
```

With CVO (§11.11) this could become a 2-byte header change with no restart. That's one of the
open decisions.

### Example I — TURN credentials expire mid-session

```
credential expires at 1792180800; the session started at 1792177300 (≈58 min before)
phone → TURN  Refresh (LIFETIME 600) with the old username → 401 Unauthorized
```

**Behaviour today:** the existing allocation keeps working until its own lifetime runs out. The
TURN relay doesn't re-check the expiry on Refresh in the current code, so the session survives.

**Spec-correct behaviour:** the server rejects the Refresh, and the allocation dies at most 600 s
later. A fix is to send fresh credentials to the phone during long sessions, or to issue
credentials for several hours. This is another decision for §13.

### Example J — the phone (not the router) uses its own Wi-Fi (Path B)

```
[phone] path = wifi → Native.setDirect(true)
candidates: 192.168.1.42 host (the phone's Wi-Fi IP, from getifaddrs) · srflx via the phone's own route
media: phone ⇄ iPhone directly; the router only carries ADB control (and still the signaling via its WebSocket)
```

This uses less router CPU and has one fewer hop. Use it when the router is weak and the phone is
on good Wi-Fi.

---

## 13. Decision sheet

Fill in the last column. Each row is either a setting that exists today or a change I'd make.

| # | Decision | Options | Now | Recommended | Your choice |
|---|---|---|---|---|---|
| 1 | Signaling security (§11.1) | ws / wss | ws, or wss with `[acme] domains` | wss for anything shared | |
| 2 | Signaling host | own server / Cloudflare Worker | own server | Worker if you have no server | |
| 3 | Agent TLS (`wss://`) (§6) | implement / not | implemented | — | done |
| 4 | DTLS role (§11.7) | active / actpass + mbedTLS fix | active | active now, fix later | |
| 5 | Relay policy (§11.4) | all / relay only | all | all | |
| 6 | Host candidates to viewer | on / off | on | on (home Wi-Fi speed) | |
| 7 | Nomination wait (§11.5) | 0–500 ms | 120 ms | 120 ms | |
| 8 | TURN URLs for the viewer (§11.6) | UDP only / UDP + `turns:443` | UDP | UDP + `turns:443` | |
| 9 | TURN credential TTL (§11.6, ex. I) | 1 h / 12 h / refresh in session | 1 h | 12 h or in-session refresh | |
| 10 | SRTP profile (§11.8) | `_80` / GCM | `_80` | `_80` | |
| 11 | Codec policy (§11.9) | auto / h265 / h264 | auto | auto, revisit after M0 | |
| 12 | Keyframe interval (§11.10) | 1–60 s | 10 s | 10 s | |
| 13 | Intra refresh (§11.10) | off / on | off | try in M3 | |
| 14 | FEC (§11.10) | none / ULPFEC / FlexFEC | none | none for now | |
| 15 | Header extensions (§11.11) | + CVO, − abs-send-time | abs-send-time, transport-cc, playout-delay | add CVO later | |
| 16 | Bitrate min / start / max (§11.12) | kbit/s | 300 / 2500 / 20000 | depends on your uplink | |
| 17 | Input channel (§11.13) | reliable / split | reliable | split if 5G stutters | |
| 18 | Jitter buffer (§11.15) | 0 / 20–50 ms | 0 | 0 at home, 30 on 5G | |
| 19 | ICE restart (ex. G) | implement / not | not implemented | implement later | |
