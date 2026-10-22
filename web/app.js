/* RemoDroid web client (plan §4.4, Appendix A). Plain JS, no build step.
 * Live mode talks to remodroid-server (REST + /ws/viewer) and the phone (WebRTC).
 * Append #demo to the URL for the offline mock (no server needed). */
(() => {
'use strict';
const $ = s => document.querySelector(s);
const $$ = s => Array.from(document.querySelectorAll(s));
const clamp = (v, a, b) => Math.min(b, Math.max(a, v));
const now = () => performance.now();
const DEMO = location.hash === '#demo' || location.protocol === 'file:' || location.hostname.endsWith('claude.ai');

/* ================= settings (per-viewer conveniences) ================= */
const KEY = 'remodroid-v1';
const DEFAULTS = {
  mode: 'auto', preset: '720p', fps: 60, kbps: 8000, codec: 'auto', stats: true, touches: false,
  screenOff: true, lockOnDisc: true, grace: 10, rotation: 'auto', clipSync: true, autoPaste: false, overheat: 45,
  destDir: '/sdcard/DCIM/RemoDroid', mediaScan: true, overwrite: false, maxMb: 2048, route: 'auto', relay: false,
  audio: true, sound: true, device: '', swapCmd: 'auto'
};
const S = Object.assign({}, DEFAULTS);
try { Object.assign(S, JSON.parse(localStorage.getItem(KEY) || '{}')); } catch (e) {}
const save = () => { try { localStorage.setItem(KEY, JSON.stringify(S)); } catch (e) {} };
{ const want = new URLSearchParams(location.search).get('device'); if (want) { S.device = want; save(); history.replaceState(null, '', '/'); } }
let ballPos = null;
try { ballPos = JSON.parse(localStorage.getItem(KEY + '-ball') || 'null'); } catch (e) {}

/* ================= small helpers ================= */
function toast(msg) {
  const t = document.createElement('div');
  t.className = 'toast'; t.textContent = msg;
  $('#toasts').appendChild(t);
  setTimeout(() => t.remove(), 2800);
}
const hex = u8 => Array.from(u8, b => b.toString(16).padStart(2, '0')).join(' ');
const fmtSize = n => n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB' : Math.max(1, Math.round(n / 1024)) + ' KB';
function el(tag, attrs, ...kids) {
  const e = document.createElement(tag);
  for (const k in attrs || {}) {
    if (k === 'class') e.className = attrs[k];
    else if (k === 'text') e.textContent = attrs[k];
    else if (k.startsWith('on')) e.addEventListener(k.slice(2), attrs[k]);
    else e.setAttribute(k, attrs[k]);
  }
  for (const k of kids) if (k != null) e.append(k);
  return e;
}
function copyText(text) {
  const fallback = () => {
    const ta = $('#copybuf');
    ta.value = text; ta.readOnly = true;
    ta.focus({ preventScroll: true }); ta.select(); ta.setSelectionRange(0, text.length);
    let ok = false; try { ok = document.execCommand('copy'); } catch (e) {}
    ta.blur();
    return ok;
  };
  if (window.isSecureContext && navigator.clipboard && navigator.clipboard.writeText)
    return navigator.clipboard.writeText(text).then(() => true).catch(() => fallback());
  return Promise.resolve(fallback()); /* plain http: no Clipboard API (§4.8) */
}
function sha256js(bytes) {
  const K = [0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2];
  const H = [0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19];
  const l = bytes.length, n = ((l + 9 + 63) >> 6) << 6;
  const m = new Uint8Array(n); m.set(bytes); m[l] = 0x80;
  const dv = new DataView(m.buffer);
  dv.setUint32(n - 8, Math.floor(l / 0x20000000)); dv.setUint32(n - 4, (l << 3) >>> 0);
  const w = new Int32Array(64);
  for (let o = 0; o < n; o += 64) {
    for (let i = 0; i < 16; i++) w[i] = dv.getInt32(o + i * 4);
    for (let i = 16; i < 64; i++) {
      const a = w[i - 15], b = w[i - 2];
      const s0 = ((a >>> 7) | (a << 25)) ^ ((a >>> 18) | (a << 14)) ^ (a >>> 3);
      const s1 = ((b >>> 17) | (b << 15)) ^ ((b >>> 19) | (b << 13)) ^ (b >>> 10);
      w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
    }
    let [a, b, c, d, e, f, g, h] = H;
    for (let i = 0; i < 64; i++) {
      const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7));
      const t1 = (h + S1 + ((e & f) ^ (~e & g)) + K[i] + w[i]) | 0;
      const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10));
      const t2 = (S0 + ((a & b) ^ (a & c) ^ (b & c))) | 0;
      h = g; g = f; f = e; e = (d + t1) | 0; d = c; c = b; b = a; a = (t1 + t2) | 0;
    }
    H[0] = (H[0] + a) | 0; H[1] = (H[1] + b) | 0; H[2] = (H[2] + c) | 0; H[3] = (H[3] + d) | 0;
    H[4] = (H[4] + e) | 0; H[5] = (H[5] + f) | 0; H[6] = (H[6] + g) | 0; H[7] = (H[7] + h) | 0;
  }
  return H.map(x => (x >>> 0).toString(16).padStart(8, '0')).join('');
}
async function sha256(buf) {
  if (window.crypto && crypto.subtle && window.isSecureContext) {
    try { return hex(new Uint8Array(await crypto.subtle.digest('SHA-256', buf))).replace(/ /g, ''); } catch (e) {}
  }
  return sha256js(new Uint8Array(buf)); /* crypto.subtle needs HTTPS */
}

/* ================= protocol log (Logs tab) ================= */
const proto = [];
let renderProto = null;
function logProto(dir, chan, text) {
  proto.push({ dir, line: `${new Date().toTimeString().slice(0, 8)} ${dir} ${chan.padEnd(5)} ${text.slice(0, 400)}` });
  if (proto.length > 150) proto.shift();
  if (renderProto) renderProto();
}

/* ================= state ================= */
const st = {
  devices: [], device: null, sid: null, ws: null, wsBackoff: 1,
  pc: null, ch: {}, ready: null, connected: false, ended: true,
  phoneStats: {}, localStats: {}, screen: { w: 1080, h: 2400 },
  pending: new Map(), msgId: 1, lastHex: '—', inputs: 0, panel: 'on', locked: false,
  hw: { h265: null, h264: null }, pulls: new Map(), logStreams: new Map(),
};
let codecLabel = () => (st.ready && st.ready.codec) ? st.ready.codec.toUpperCase().replace('H26', 'H.26') : '—';

/* ================= REST ================= */
async function api(method, path, body) {
  logProto('→', 'http', `${method} ${path}${body ? ' ' + JSON.stringify(body) : ''}`);
  if (DEMO) return { ok: true, status: 200, json: async () => ({ result: {} }) };
  const r = await fetch('/api/v1' + path, { method, headers: body ? { 'Content-Type': 'application/json' } : {},
    body: body ? JSON.stringify(body) : undefined, credentials: 'same-origin' });
  if (r.status === 401) { toLogin(); throw new Error('login required'); }
  return r;
}
/* The login page brings you back here, to the same phone. */
function toLogin() { location.href = '/login?next=' + encodeURIComponent(location.pathname + location.search); }
async function cmd(name, args, msg) {
  if (!st.device) { toast('No device selected'); return; }
  try {
    const r = await api('POST', `/devices/${encodeURIComponent(st.device.id)}/cmd`, { cmd: name, args: args || {} });
    const j = await r.json().catch(() => ({}));
    if (r.ok) toast(msg); else toast(j.msg || `Failed (${r.status})`);
    return r.ok;
  } catch (e) { toast('Network error: ' + e.message); return false; }
}

/* ================= viewer WebSocket ================= */
function wsSend(m) {
  if (st.ws && st.ws.readyState === 1) { st.ws.send(JSON.stringify(m)); logProto('→', 'ws', JSON.stringify(m)); }
}
function connectWs() {
  if (DEMO) return;
  const ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws/viewer');
  st.ws = ws;
  ws.onopen = () => { st.wsBackoff = 1; setOverlay('Connecting', 'Looking for your phone…'); };
  ws.onmessage = e => { let m; try { m = JSON.parse(e.data); } catch (_) { return; } onWs(m); };
  ws.onclose = () => {
    st.ws = null;
    setOverlay('Server unreachable', `Retrying in ${st.wsBackoff} s…`, true);
    /* a refused upgrade looks the same as a dead server: a logged-out browser goes to the login page */
    fetch('/api/v1/me', { credentials: 'same-origin' }).then(r => { if (r.status === 401) toLogin(); }).catch(() => {});
    setTimeout(connectWs, st.wsBackoff * 1000);
    st.wsBackoff = Math.min(st.wsBackoff * 2, 30);
  };
}
function pickDevice() {
  const saved = st.devices.find(d => d.id === S.device);
  return (saved && saved.online) ? saved : st.devices.find(d => d.online) || saved || st.devices[0] || null;
}
function onWs(m) {
  if (m.t !== 'signal') logProto('←', 'ws', JSON.stringify(m));
  const d = m.d || {};
  switch (m.t) {
  case 'devices':
    st.listed = true;
    st.devices = d.devices || [];
    st.device = pickDevice();
    deviceChanged();
    maybeStart();
    break;
  case 'device.update': {
    const i = st.devices.findIndex(x => x.id === d.id);
    if (i >= 0) st.devices[i] = d.patch; else st.devices.push(d.patch);
    if (!st.device || st.device.id === d.id) st.device = st.devices.find(x => x.id === d.id) || pickDevice();
    deviceChanged();
    maybeStart();
    break;
  }
  case 'device.removed':
    st.devices = st.devices.filter(x => x.id !== d.id);
    if (st.device && st.device.id === d.id) { st.device = pickDevice(); deviceChanged(); }
    maybeStart();
    break;
  case 'device.event':
    if (st.device && d.id === st.device.id) {
      const msgs = { 'adb.unauthorized': 'Allow USB debugging on the phone', 'phone.detached': 'Phone unplugged',
        'phone.attached': 'Phone connected', 'agent.offline': 'Router agent went offline', 'server.exited': 'Phone server stopped',
        'phone.overheat': 'The phone is getting hot', 'phone.busy': 'Another program holds the phone (adb?)', 'phone.locked': 'Phone locked' };
      if (msgs[d.kind]) toast(msgs[d.kind]);
      if (d.kind === 'phone.locked') st.locked = true;
    }
    break;
  case 'session.ice': startPeer(d.ice_servers || []); break;
  case 'signal': onSignal(d); break;
  case 'session.ended': sessionEnded(d.reason || 'ended'); break;
  case 'ack': case 'error': {
    const p = st.pending.get(m.id);
    if (p) { st.pending.delete(m.id); m.t === 'ack' ? p.res(d) : p.rej(Object.assign(new Error(d.msg || d.code), d)); }
    break;
  }
  }
}
function request(t, d) {
  const id = st.msgId++;
  return new Promise((res, rej) => {
    st.pending.set(id, { res, rej });
    wsSend({ t, id, d });
    setTimeout(() => { if (st.pending.delete(id)) rej(new Error('timeout')); }, 10000);
  });
}

/* ================= session / WebRTC ================= */
let wantSession = true;
function maybeStart() {
  if (!DEMO && st.listed && !st.device && !st.devices.length) { setOverlay('No routers yet', 'Install the agent on the router the phone is plugged into, then enroll it with a code.'); return; }
  if (DEMO || !wantSession || st.sid || !st.device) return;
  const ph = st.device.phone || {};
  if (desk()) {
    if (!st.device.online) { setOverlay('Computer offline', `${st.device.name} isn't connected to the server. Is it on, with RemoDroid running?`); return; }
    if (ph.server !== 'running') { setOverlay('Computer not ready', ph.problem ? `${st.device.name}: ${ph.problem}.` : `${st.device.name} can't be controlled right now.`); return; }
  } else {
  if (!st.device.online) { setOverlay('Router offline', `${st.device.name} isn't connected to the server.`); return; }
  if (!ph.attached) { setOverlay(ph.problem ? 'Phone busy' : 'No phone', ph.problem ? `The router sees the phone, but ${ph.problem}.` : 'Plug the phone into the router.'); return; }
  if (ph.adb_state === 'unauthorized') { setOverlay('Allow USB debugging', 'Accept the prompt on the phone (tick Always allow).'); return; }
  if (ph.server !== 'running') { setOverlay('Starting', 'The phone server is starting…'); return; }
  }
  const vs = st.device.session;
  if (vs && vs.active) {
    const who = vs.viewer ? `${vs.viewer} is` : 'Someone is', at = vs.since ? ` since ${new Date(vs.since).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}` : '';
    setOverlay('In use', `${who} viewing this ${noun()} from another browser${at}. Take over to disconnect them and connect here.`, false, true);
    return;
  }
  startSession();
}
async function startSession(takeover) {
  st.sid = 'pending';
  setOverlay('Connecting', `Connecting to ${st.device.name}…`);
  try {
    const r = await request('session.request', { device: st.device.id, quality: quality(), force_relay: S.relay, ...(takeover ? { takeover: true } : {}) });
    st.sid = r.sid;
    st.ended = false;
  } catch (e) {
    st.sid = null;
    setOverlay('Cannot connect', e.msg || e.message, true);
  }
}
function startPeer(iceServers) {
  closePeer();
  const pc = new RTCPeerConnection({ iceServers, bundlePolicy: 'max-bundle', iceTransportPolicy: S.relay ? 'relay' : 'all' });
  st.pc = pc;
  st.ch = {};
  pc.onicecandidate = e => {
    if (!st.sid) return;
    wsSend({ t: 'signal', d: e.candidate ? { type: 'candidate', candidate: e.candidate.candidate, sdpMid: e.candidate.sdpMid,
      sdpMLineIndex: e.candidate.sdpMLineIndex } : { type: 'candidate', candidate: null } });
  };
  pc.ontrack = e => {
    try { e.receiver.playoutDelayHint = 0; } catch (_) {}   /* Chrome */
    try { e.receiver.jitterBufferTarget = 0; } catch (_) {} /* spec / newer Safari */
    const v = $('#video'), stream = e.streams[0];
    if (stream) { if (v.srcObject !== stream) v.srcObject = stream; }
    else { if (!(v.srcObject instanceof MediaStream)) v.srcObject = new MediaStream(); v.srcObject.addTrack(e.track); }
    if (e.track.kind === 'audio') { st.hasAudio = true; syncMenuStates(); }
    v.play().catch(() => { $('#tapToPlay').hidden = false; });
  };
  pc.ondatachannel = e => {
    const c = e.channel;
    c.binaryType = 'arraybuffer';
    st.ch[c.label] = c;
    if (c.label === 'file') c.bufferedAmountLowThreshold = 256 * 1024;
    c.onmessage = ev => onChannel(c.label, ev.data);
  };
  pc.onconnectionstatechange = () => {
    logProto('·', 'note', 'peer ' + pc.connectionState);
    if (pc.connectionState === 'connected') { st.connected = true; $('#disc').hidden = true; ball.hidden = !!openId; }
    else if (pc.connectionState === 'failed') sessionEnded('connection_failed');
  };
}
async function onSignal(d) {
  const pc = st.pc;
  if (!pc) return;
  if (d.type === 'offer') {
    await pc.setRemoteDescription({ type: 'offer', sdp: d.sdp });
    preferCodecs(pc);
    const ans = await pc.createAnswer();
    await pc.setLocalDescription(ans);
    wsSend({ t: 'signal', d: { type: 'answer', sdp: pc.localDescription.sdp } });
  } else if (d.type === 'candidate') {
    try { await pc.addIceCandidate(d.candidate ? { candidate: d.candidate, sdpMid: d.sdpMid || '0', sdpMLineIndex: d.sdpMLineIndex || 0 } : null); }
    catch (e) { logProto('·', 'note', 'candidate rejected: ' + e.message); }
  }
}
/* Keep H.265 only if this iPhone decodes it in hardware (§5.6), or honor a manual choice. */
function preferCodecs(pc) {
  const tr = pc.getTransceivers().find(t => t.receiver.track && t.receiver.track.kind === 'video');
  if (!tr || !tr.setCodecPreferences || !RTCRtpReceiver.getCapabilities) return;
  try {
    const caps = RTCRtpReceiver.getCapabilities('video').codecs;
    const want = S.codec === 'h264' ? ['video/H264'] : S.codec === 'h265' ? ['video/H265'] :
      (st.hw.h265 === false ? ['video/H264'] : ['video/H265', 'video/H264']);
    const pick = [];
    for (const w of want) pick.push(...caps.filter(c => c.mimeType === w));
    pick.push(...caps.filter(c => c.mimeType === 'video/rtx'));
    if (pick.length) tr.setCodecPreferences(pick);
  } catch (e) { logProto('·', 'note', 'setCodecPreferences: ' + e.message); }
}
function closePeer() {
  if (st.pc) { try { st.pc.close(); } catch (_) {} }
  st.pc = null; st.ch = {}; st.connected = false; st.ready = null;
}
function sessionEnded(reason) {
  closePeer();
  st.sid = null;
  st.ended = true;
  if (reason === 'taken_over') wantSession = false; /* don't grab it straight back */
  const nice = { viewer_left: 'You left the session.', agent_offline: 'The router agent went offline.', phone_unavailable: 'The phone server is not ready.',
    ice_failed: `No network path to the ${noun()}.`, connection_failed: 'The connection dropped.', replaced: 'Another session started.',
    taken_over: `Another browser took over this ${noun()}.`,
    timeout: `The ${noun()} did not answer in time.`,
    declined: 'The person at the computer declined the connection.', ended_at_computer: 'Disconnected at the computer.',
    computer_unavailable: 'The computer can\'t be controlled right now (see the device list).' };
  setOverlay('Disconnected', (nice[reason] || reason) + (S.lockOnDisc && !desk() && reason !== 'taken_over' ? ` The phone locks in ${S.grace} s.` : ''), true);
}
function endSession() {
  wantSession = false;
  if (st.sid && st.sid !== 'pending') wsSend({ t: 'session.end', d: {} });
  sessionEnded('viewer_left');
}
function setOverlay(title, text, showReconnect, showTakeover) {
  if (DEMO) return;
  $('#disc').hidden = false;
  $('#discTitle').textContent = title;
  $('#discText').textContent = text;
  $('#discCount').textContent = '';
  $('#reconnect').hidden = !showReconnect;
  $('#takeover').hidden = !showTakeover;
  $('#discLink').hidden = title !== 'No routers yet';
  ball.hidden = !!openId; /* above the overlay: a tap goes to the device list */
}
$('#reconnect').addEventListener('click', () => {
  if (DEMO) { $('#disc').hidden = true; ball.hidden = false; return; }
  wantSession = true;
  setOverlay('Connecting', '…');
  if (!st.ws) connectWs(); else maybeStart();
});
$('#takeover').addEventListener('click', () => {
  wantSession = true;
  if (st.device && !st.sid) startSession(true);
});
$('#tapToPlayBtn').addEventListener('click', () => { $('#tapToPlay').hidden = true; unmute(); $('#video').play().catch(() => {}); });

/* ================= data channels (A.6–A.8) ================= */
function sendCtrl(t, d, wait) {
  const m = { t, d: d || {} };
  if (wait) m.id = st.msgId++;
  const c = st.ch.ctrl;
  logProto('→', 'ctrl', JSON.stringify(m));
  if (DEMO) return Promise.resolve({});
  if (!c || c.readyState !== 'open') return Promise.reject(new Error('not connected'));
  c.send(JSON.stringify(m));
  if (!wait) return Promise.resolve();
  return new Promise((res, rej) => {
    st.pending.set(m.id, { res, rej });
    setTimeout(() => { if (st.pending.delete(m.id)) rej(new Error('timeout')); }, 15000);
  });
}
function onChannel(label, data) {
  if (label === 'file') return onFileChunk(new Uint8Array(data));
  if (label !== 'ctrl') return;
  let m; try { m = JSON.parse(data); } catch (_) { return; }
  if (m.t !== 'stats' && m.t !== 'logcat.lines') logProto('←', 'ctrl', data.slice(0, 400));
  const d = m.d || {};
  switch (m.t) {
  case 'session.ready':
    st.ready = d;
    st.screen = d.screen || st.screen;
    $('#disc').hidden = true; ball.hidden = !!openId;
    toast(`Connected · ${codecLabel()}${d.caps && d.caps.view_only ? ' · view only' : ''}`);
    if (desk()) { renderQuickKeys(); if (document.fullscreenElement) lockKeys(true); }
    if (S.mode !== 'auto' || S.preset !== '720p') sendCtrl('quality.set', quality());
    if (!S.sound && d.caps && d.caps.audio) sendCtrl('audio.set', { on: false }); /* keep the phone's speaker */
    syncMenuStates();
    break;
  case 'audio.state': st.audioOn = d.on; syncMenuStates(); break;
  case 'quality.state': st.quality = d; break;
  case 'screen.changed': st.screen = d; break;
  case 'stats': st.phoneStats = d; st.panel = d.panel || st.panel; break;
  case 'screen.state': st.panel = d.panel; st.locked = d.locked; syncMenuStates(); break;
  case 'clip.changed': androidCopied(d.text || ''); break;
  case 'logcat.lines': { const s = st.logStreams.get(d.stream); if (s) s(d.lines || []); break; }
  case 'file.progress': { const u = uploads.get(d.xfer); if (u) u.phoneBytes = d.bytes; break; }
  case 'file.done': { const p = st.pulls.get(d.xfer); if (p) finishPull(d.xfer); break; }
  case 'ack': case 'error': {
    const p = st.pending.get(m.id);
    if (p) { st.pending.delete(m.id); m.t === 'ack' ? p.res(d) : p.rej(Object.assign(new Error(d.msg || d.code), d)); }
    else if (m.t === 'error') toast(d.msg || d.code);
    break;
  }
  }
}

/* ================= stream area + input ================= */
const stage = $('#stage'), video = $('#video'), cv = $('#screen'), ctx = cv.getContext('2d');
/* iOS 26 Home Screen apps with the "black-translucent" status bar get a page as tall as the screen
 * minus the status bar, so the bottom of the picture is cut off; index.html uses "default", which
 * still draws under the status bar but gets the whole screen. If a page still comes out short,
 * stretch the stage over the band. */
function fitStage(ins) {
  const full = innerWidth > innerHeight ? Math.min(screen.width, screen.height) : Math.max(screen.width, screen.height);
  const short = navigator.standalone === true && ins.t > 0 && Math.abs(innerHeight + ins.t - full) < 2;
  stage.style.height = short ? full + 'px' : '';
}
/* In the Home Screen app the picture fills the whole screen, under the status bar and the home
 * indicator, like a full-screen app. In Safari it stays inside the safe area, clear of the toolbars. */
function layout() {
  const safe = bounds();
  fitStage(safe);
  const ins = standalone() ? { t: 0, r: 0, b: 0, l: 0 } : safe;
  const W = stage.clientWidth - ins.l - ins.r, H = stage.clientHeight - ins.t - ins.b;
  let w, h;
  if (DEMO) { w = cv.width; h = cv.height; }
  else { w = video.videoWidth || st.screen.w || 9; h = video.videoHeight || st.screen.h || 20; }
  const sc = Math.min(W / w, H / h), cw = Math.floor(w * sc), ch = Math.floor(h * sc);
  for (const e of [video, cv]) {
    e.style.width = cw + 'px'; e.style.height = ch + 'px';
    e.style.left = Math.floor(ins.l + (W - cw) / 2) + 'px'; e.style.top = Math.floor(ins.t + (H - ch) / 2) + 'px';
  }
  if (S.lc) layoutCheck(true);
}
/* Layout check: colours each layer (red: page canvas, green: body, blue: stage, yellow: picture),
 * draws bars as tall as each CSS viewport unit and prints what iOS reports, so a photo of the
 * screen shows which layer stops short. */
function layoutCheck(on) {
  document.documentElement.classList.toggle('lc', on);
  let box = $('#lc');
  if (!on) { if (box) box.remove(); return; }
  const bars = [['100vh', '#fff'], ['100dvh', '#0ff'], ['100lvh', '#f0f'], ['100svh', '#f80'], [innerHeight + 'px', '#000']];
  if (!box) { box = document.createElement('div'); box.id = 'lc'; document.body.append(box); }
  const safe = bounds(), vv = window.visualViewport, mm = q => matchMedia(q).matches;
  box.innerHTML = bars.map(([h, c], i) => `<i style="height:${h};left:${6 + i * 12}px;background:${c}"></i>`).join('') + '<pre></pre>';
  box.querySelector('pre').textContent = [
    'bars: vh white · dvh cyan · lvh magenta · svh orange · innerHeight black',
    `screen ${screen.width}x${screen.height} @${devicePixelRatio}x`,
    `inner ${innerWidth}x${innerHeight} · visualViewport ${vv ? Math.round(vv.width) + 'x' + Math.round(vv.height) : '-'}`,
    `html ${document.documentElement.clientHeight} · stage ${stage.clientHeight}${stage.style.height ? ' (stretched)' : ''}`,
    `safe area t${safe.t} r${safe.r} b${safe.b} l${safe.l}`,
    `standalone ${navigator.standalone} · display ${mm('(display-mode: fullscreen)') ? 'fullscreen' : mm('(display-mode: standalone)') ? 'standalone' : 'browser'}`,
    `picture ${video.style.width} x ${video.style.height} at ${video.style.left}, ${video.style.top}`,
    navigator.userAgent.replace(/^[^(]*\(|\).*$/g, ''),
  ].join('\n');
}
window.addEventListener('resize', layout);
window.addEventListener('orientationchange', () => setTimeout(layout, 150));
video.addEventListener('resize', layout);
video.addEventListener('loadedmetadata', layout);

function encTouch(action, pid, x, y, p, ts) {
  const b = new ArrayBuffer(14), v = new DataView(b);
  v.setUint8(0, 0x01); v.setUint8(1, action); v.setUint8(2, pid); v.setUint8(3, 1);
  v.setUint16(4, x); v.setUint16(6, y); v.setUint16(8, p); v.setUint32(10, ts >>> 0);
  return new Uint8Array(b);
}
const ACT = ['down', 'up', 'move', 'cancel'];
function sendTouch(action, pid, nx, ny, pressure) {
  const u8 = encTouch(action, pid, Math.round(nx * 65535), Math.round(ny * 65535), Math.round(clamp(pressure, 0, 1) * 65535), Math.floor(now()));
  st.inputs++; st.lastHex = hex(u8);
  const c = st.ch.input;
  if (c && c.readyState === 'open') c.send(u8);
  if (action !== 2) logProto('→', 'input', `touch ${ACT[action]} p${pid} [${st.lastHex}]`);
}
const pidMap = new Map(), touches = new Map(), ripples = [];
function allocPid(id) { for (let i = 0; i < 10; i++) if (![...pidMap.values()].includes(i)) { pidMap.set(id, i); return i; } return 9; }
const target = () => DEMO ? cv : video;
function norm(e) {
  const r = target().getBoundingClientRect();
  return [clamp((e.clientX - r.left) / r.width, 0, 1), clamp((e.clientY - r.top) / r.height, 0, 1)];
}
function down(e) {
  if (desk()) { deskPointer('down', e); return; }
  e.preventDefault(); closeMenu();
  try { e.target.setPointerCapture(e.pointerId); } catch (_) {}
  const pid = allocPid(e.pointerId), [x, y] = norm(e);
  touches.set(pid, { x, y }); ripples.push({ x, y, t: now() });
  demoPlayer.tx = x; demoPlayer.ty = y;
  sendTouch(0, pid, x, y, e.pressure || 1);
}
function move(e) {
  if (desk()) { deskPointer('move', e); return; }
  if (!pidMap.has(e.pointerId)) return;
  e.preventDefault();
  const pid = pidMap.get(e.pointerId), [x, y] = norm(e);
  touches.set(pid, { x, y });
  if (pid === 0) { demoPlayer.tx = x; demoPlayer.ty = y; }
  sendTouch(2, pid, x, y, e.pressure || 1);
}
function up(e, action) {
  if (desk()) { deskPointer(action === 1 ? 'up' : 'cancel', e); return; }
  if (!pidMap.has(e.pointerId)) return;
  const pid = pidMap.get(e.pointerId), [x, y] = norm(e);
  sendTouch(action, pid, x, y, 0);
  touches.delete(pid); pidMap.delete(e.pointerId);
}
for (const t of [video, cv]) {
  t.addEventListener('pointerdown', down);
  t.addEventListener('pointermove', move);
  t.addEventListener('pointerup', e => { up(e, 1); unmute(); });
  t.addEventListener('pointercancel', e => up(e, 3));
  t.addEventListener('contextmenu', e => e.preventDefault());
}
document.addEventListener('gesturestart', e => e.preventDefault());
/* deltaMode: pixels (most browsers), lines (Firefox with a mouse wheel) or pages; 100 px = one notch */
const wheelPx = (d, mode) => mode === 1 ? d * 33 : mode === 2 ? d * 800 : d;
function sendScroll(x, y, dx, dy) {
  const b = new ArrayBuffer(13), v = new DataView(b);
  v.setUint8(0, 2); v.setUint16(1, Math.round(x * 65535)); v.setUint16(3, Math.round(y * 65535));
  v.setInt16(5, clamp(Math.round(-dx / 100 * 256), -32768, 32767)); v.setInt16(7, clamp(Math.round(-dy / 100 * 256), -32768, 32767));
  v.setUint32(9, Math.floor(now()) >>> 0);
  const c = st.ch.input; if (c && c.readyState === 'open') c.send(new Uint8Array(b));
}
stage.addEventListener('wheel', e => {
  e.preventDefault();
  const [x, y] = norm(e);
  sendScroll(x, y, wheelPx(e.deltaX, e.deltaMode), wheelPx(e.deltaY, e.deltaMode));
}, { passive: false });

/* ================= computers: mouse and keyboard (multi-platform.md §8.2, §9) ================= */
const desk = () => !DEMO && !!(st.device && st.device.platform && st.device.platform !== 'android');
const appleHere = /Mac|iPhone|iPad|iPod/.test(navigator.platform || navigator.userAgent);
const noun = () => desk() ? 'computer' : 'phone';
function sendInput(u8, log) {
  st.inputs++; st.lastHex = hex(u8);
  const c = st.ch.input;
  if (c && c.readyState === 'open') c.send(u8);
  if (log) logProto('→', 'input', `${log} [${st.lastHex}]`);
}
/* DOM buttons (left 1, right 2, middle 4, back 8, forward 16) → one bit per button number */
const btnBits = b => (b & 1) | ((b & 4) >> 1) | ((b & 2) << 1) | (b & 24);
function sendMouse(action, button, buttons, x, y) {
  const b = new ArrayBuffer(12), v = new DataView(b);
  v.setUint8(0, 3); v.setUint8(1, action); v.setUint8(2, Math.max(0, button)); v.setUint8(3, btnBits(buttons));
  v.setUint16(4, Math.round(x * 65535)); v.setUint16(6, Math.round(y * 65535)); v.setUint32(8, Math.floor(now()) >>> 0);
  sendInput(new Uint8Array(b), action ? `mouse ${action === 1 ? 'down' : 'up'} b${button}` : null);
}
/* moves are sent at most once per frame */
let moveQ = null, moveRaf = 0;
function flushMove() {
  if (moveRaf) { cancelAnimationFrame(moveRaf); moveRaf = 0; }
  if (moveQ) { sendMouse(0, 0, moveQ.buttons, moveQ.x, moveQ.y); moveQ = null; }
}
function queueMove(x, y, buttons) {
  moveQ = { x, y, buttons };
  if (!moveRaf) moveRaf = requestAnimationFrame(() => { moveRaf = 0; flushMove(); });
}
const touchPts = new Map();
let tstate = null; /* one gesture: { x, y, sx, sy, mode: null|'drag'|'scroll'|'done', timer, cx, cy } */
function deskTouch(type, e) {
  e.preventDefault();
  const [x, y] = norm(e);
  if (type === 'down') {
    closeMenu();
    try { e.target.setPointerCapture(e.pointerId); } catch (_) {}
    touchPts.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (touchPts.size === 1) {
      tstate = { x, y, sx: e.clientX, sy: e.clientY, mode: null };
      tstate.timer = setTimeout(() => { /* long press: right click */
        if (tstate && !tstate.mode) { tstate.mode = 'done'; sendMouse(0, 0, 0, tstate.x, tstate.y); sendMouse(1, 2, 2, tstate.x, tstate.y); sendMouse(2, 2, 0, tstate.x, tstate.y); if (navigator.vibrate) navigator.vibrate(10); }
      }, 550);
    } else if (tstate && tstate.mode !== 'drag') { /* two fingers: scroll */
      clearTimeout(tstate.timer);
      tstate.mode = 'scroll';
      const pts = [...touchPts.values()];
      tstate.cx = pts.reduce((a, p) => a + p.x, 0) / pts.length; tstate.cy = pts.reduce((a, p) => a + p.y, 0) / pts.length;
    }
    return;
  }
  if (!touchPts.has(e.pointerId) || !tstate) return;
  if (type === 'move') {
    touchPts.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (tstate.mode === 'scroll') {
      const pts = [...touchPts.values()];
      const cx = pts.reduce((a, p) => a + p.x, 0) / pts.length, cy = pts.reduce((a, p) => a + p.y, 0) / pts.length;
      sendScroll(x, y, (tstate.cx - cx) * 2, (tstate.cy - cy) * 2); /* content follows the fingers */
      tstate.cx = cx; tstate.cy = cy;
    } else if (!tstate.mode && Math.hypot(e.clientX - tstate.sx, e.clientY - tstate.sy) > 8) {
      clearTimeout(tstate.timer);
      tstate.mode = 'drag';
      sendMouse(0, 0, 0, tstate.x, tstate.y); sendMouse(1, 0, 1, tstate.x, tstate.y);
    }
    if (tstate.mode === 'drag') queueMove(x, y, 1);
    return;
  }
  touchPts.delete(e.pointerId);
  if (touchPts.size) return;
  clearTimeout(tstate.timer);
  if (tstate.mode === 'drag') { flushMove(); sendMouse(2, 0, 0, x, y); }
  else if (!tstate.mode && type === 'up') { sendMouse(0, 0, 0, tstate.x, tstate.y); sendMouse(1, 0, 1, tstate.x, tstate.y); sendMouse(2, 0, 0, tstate.x, tstate.y); }
  tstate = null;
  unmute();
}
function deskPointer(type, e) {
  if (e.pointerType === 'touch') { deskTouch(type, e); return; }
  const [x, y] = norm(e);
  if (type === 'down') {
    e.preventDefault(); closeMenu();
    try { e.target.setPointerCapture(e.pointerId); } catch (_) {}
    flushMove(); sendMouse(1, e.button, e.buttons, x, y);
  } else if (type === 'move') {
    if (e.button >= 0) { /* a second button pressed or released during a drag (chorded buttons) */
      flushMove();
      const bit = [1, 4, 2, 8, 16][e.button] || 0;
      sendMouse(e.buttons & bit ? 1 : 2, e.button, e.buttons, x, y);
    } else queueMove(x, y, e.buttons);
  } else {
    flushMove(); sendMouse(2, Math.max(0, e.button), e.buttons, x, y);
  }
}
/* KeyboardEvent.code → USB HID usage (desktop/core/keymap.c has the same names) */
const KEYS = {
  KeyA: 0x04, KeyB: 0x05, KeyC: 0x06, KeyD: 0x07, KeyE: 0x08, KeyF: 0x09, KeyG: 0x0A, KeyH: 0x0B, KeyI: 0x0C, KeyJ: 0x0D,
  KeyK: 0x0E, KeyL: 0x0F, KeyM: 0x10, KeyN: 0x11, KeyO: 0x12, KeyP: 0x13, KeyQ: 0x14, KeyR: 0x15, KeyS: 0x16, KeyT: 0x17,
  KeyU: 0x18, KeyV: 0x19, KeyW: 0x1A, KeyX: 0x1B, KeyY: 0x1C, KeyZ: 0x1D,
  Digit1: 0x1E, Digit2: 0x1F, Digit3: 0x20, Digit4: 0x21, Digit5: 0x22, Digit6: 0x23, Digit7: 0x24, Digit8: 0x25,
  Digit9: 0x26, Digit0: 0x27, Enter: 0x28, Escape: 0x29, Backspace: 0x2A, Tab: 0x2B, Space: 0x2C, Minus: 0x2D, Equal: 0x2E,
  BracketLeft: 0x2F, BracketRight: 0x30, Backslash: 0x31, Semicolon: 0x33, Quote: 0x34, Backquote: 0x35, Comma: 0x36,
  Period: 0x37, Slash: 0x38, CapsLock: 0x39, F1: 0x3A, F2: 0x3B, F3: 0x3C, F4: 0x3D, F5: 0x3E, F6: 0x3F, F7: 0x40,
  F8: 0x41, F9: 0x42, F10: 0x43, F11: 0x44, F12: 0x45, PrintScreen: 0x46, ScrollLock: 0x47, Pause: 0x48, Insert: 0x49,
  Home: 0x4A, PageUp: 0x4B, Delete: 0x4C, End: 0x4D, PageDown: 0x4E, ArrowRight: 0x4F, ArrowLeft: 0x50, ArrowDown: 0x51,
  ArrowUp: 0x52, NumLock: 0x53, NumpadDivide: 0x54, NumpadMultiply: 0x55, NumpadSubtract: 0x56, NumpadAdd: 0x57,
  NumpadEnter: 0x58, Numpad1: 0x59, Numpad2: 0x5A, Numpad3: 0x5B, Numpad4: 0x5C, Numpad5: 0x5D, Numpad6: 0x5E,
  Numpad7: 0x5F, Numpad8: 0x60, Numpad9: 0x61, Numpad0: 0x62, NumpadDecimal: 0x63, IntlBackslash: 0x64,
  ContextMenu: 0x65, NumpadEqual: 0x67, F13: 0x68, F14: 0x69, F15: 0x6A, F16: 0x6B, F17: 0x6C, F18: 0x6D, F19: 0x6E,
  F20: 0x6F, AudioVolumeMute: 0x7F, AudioVolumeUp: 0x80, AudioVolumeDown: 0x81, NumpadComma: 0x85, IntlRo: 0x87,
  KanaMode: 0x88, IntlYen: 0x89, Convert: 0x8A, NonConvert: 0x8B, Lang1: 0x90, Lang2: 0x91,
  ControlLeft: 0xE0, ShiftLeft: 0xE1, AltLeft: 0xE2, MetaLeft: 0xE3, ControlRight: 0xE4, ShiftRight: 0xE5,
  AltRight: 0xE6, MetaRight: 0xE7, OSLeft: 0xE3, OSRight: 0xE7,
};
/* ⌘ on one side, Ctrl on the other: map them so shortcuts keep their meaning (Settings → Stream).
 * From a Mac, ⌘ becomes Ctrl and Ctrl stays Ctrl, so ⌘C copies and Ctrl+C still stops a
 * terminal; towards a Mac, Ctrl and the Windows key trade places. */
const swapCmd = () => S.swapCmd === 'on' || (S.swapCmd === 'auto' && st.device && appleHere !== (st.device.platform === 'macos'));
const SWAP = appleHere ? { 0xE3: 0xE0, 0xE7: 0xE4 } : { 0xE0: 0xE3, 0xE3: 0xE0, 0xE4: 0xE7, 0xE7: 0xE4 };
let held = 0; /* modifiers down here, bit n = usage 0xE0 + n (before swapping) */
function sendKey(down, hid, mods) {
  const b = new ArrayBuffer(10), v = new DataView(b);
  v.setUint8(0, 4); v.setUint8(1, down ? 0 : 1); v.setUint16(2, hid); v.setUint16(4, mods); v.setUint32(6, Math.floor(now()) >>> 0);
  sendInput(new Uint8Array(b), down ? `key 0x${hid.toString(16)}` : null);
}
function remoteMods(m) {
  if (!swapCmd()) return m;
  const ctrl = m & 0x11, meta = m & 0x88;
  if (appleHere) return (m & ~0x88) | (meta >> 3);
  return (m & ~0x99) | (ctrl << 3) | (meta >> 3);
}
function keyTarget(e) {
  const t = e.target;
  return !(t && (t.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(t.tagName)));
}
function onKey(e, down) {
  if (!desk() || !st.connected || openId || !menu.hidden || !keyTarget(e) || e.isComposing || e.key === 'Process') return;
  const local = KEYS[e.code];
  if (!local) return;
  e.preventDefault();
  if (local >= 0xE0) held = down ? held | (1 << (local - 0xE0)) : held & ~(1 << (local - 0xE0));
  /* resync with what the browser says is down (keys released while another window had focus) */
  if (!e.ctrlKey) held &= ~0x11;
  if (!e.shiftKey) held &= ~0x22;
  if (!e.altKey) held &= ~0x44;
  if (!e.metaKey) held &= ~0x88;
  const hid = swapCmd() && SWAP[local] ? SWAP[local] : local;
  const mods = remoteMods(held);
  if (!down && hid >= 0xE0 && mods & (1 << (hid - 0xE0))) return; /* Ctrl and ⌘ both map here; the other is still down */
  sendKey(down, hid, mods);
}
document.addEventListener('keydown', e => onKey(e, true));
document.addEventListener('keyup', e => onKey(e, false));
function releaseAll() {
  if (!desk()) return;
  held = 0;
  const b = new ArrayBuffer(5), v = new DataView(b);
  v.setUint8(0, 5); v.setUint32(1, Math.floor(now()) >>> 0);
  sendInput(new Uint8Array(b), 'reset');
}
window.addEventListener('blur', releaseAll);
document.addEventListener('visibilitychange', () => { if (document.hidden) releaseAll(); });
/* A key combination from the Keys sheet: press in order, release in reverse. Each event
 * carries the modifiers the combination holds at that moment: the computer releases any
 * modifier a key event leaves out, which would turn Ctrl+C into a plain C. */
function sendCombo(hids) {
  const bit = h => h >= 0xE0 ? 1 << (h - 0xE0) : 0;
  let mods = 0;
  for (const h of hids) { mods |= bit(h); sendKey(true, h, mods); }
  for (const h of hids.slice().reverse()) { mods &= ~bit(h); sendKey(false, h, mods); }
}
/* full screen on Chromium: also capture Esc, Alt+Tab, Ctrl+W… */
function lockKeys(on) {
  try { if (navigator.keyboard && navigator.keyboard.lock) { if (on && desk()) navigator.keyboard.lock(); else navigator.keyboard.unlock(); } } catch (_) {}
}
function selectScreen(index) {
  sendCtrl('screen.select', { index }, true).then(() => toast(`Screen ${index + 1}`)).catch(e => toast(e.msg || e.message));
}

/* ---- demo stream (offline preview) ---- */
const demoPlayer = { x: .5, y: .62, tx: .5, ty: .62 };
const stars = Array.from({ length: 90 }, () => ({ x: Math.random(), y: Math.random(), z: .2 + Math.random() * .8 }));
let demoScore = 1240, lastFrame = 0;
function drawDemo(t) {
  requestAnimationFrame(drawDemo);
  if (t - lastFrame < 1000 / S.fps - 2) return;
  lastFrame = t;
  const W = cv.width, H = cv.height, u = Math.min(W, H);
  const g = ctx.createLinearGradient(0, 0, 0, H);
  g.addColorStop(0, '#0B1430'); g.addColorStop(.55, '#22285A'); g.addColorStop(1, '#4A2B5E');
  ctx.fillStyle = g; ctx.fillRect(0, 0, W, H);
  ctx.fillStyle = '#E8ECFF';
  for (const s of stars) { s.y += s.z * .0016; if (s.y > 1) { s.y = 0; s.x = Math.random(); } ctx.globalAlpha = .35 + s.z * .6; ctx.fillRect(s.x * W, s.y * H * .7, u * .004 * s.z + 1, u * .004 * s.z + 1); }
  ctx.globalAlpha = 1;
  demoPlayer.x += (demoPlayer.tx - demoPlayer.x) * .14; demoPlayer.y += (demoPlayer.ty - demoPlayer.y) * .14;
  ctx.fillStyle = '#6CC8EE'; ctx.beginPath(); ctx.arc(demoPlayer.x * W, demoPlayer.y * H, u * .045, 0, Math.PI * 2); ctx.fill();
  demoScore += 3;
  ctx.font = `500 ${u * .05}px ui-monospace, monospace`; ctx.fillStyle = '#F1F4FA';
  ctx.fillText('SCORE ' + String(demoScore).padStart(6, '0'), u * .05, u * .12);
  ctx.font = `400 ${u * .03}px ui-monospace, monospace`; ctx.fillStyle = 'rgba(241,244,250,.6)';
  ctx.fillText('DEMO STREAM · no server', u * .05, H - u * .05);
  drawTouches(ctx, W, H, u, t);
}
function drawTouches(c, W, H, u, t) {
  if (!S.touches) return;
  for (const [, p] of touches) { c.fillStyle = 'rgba(255,255,255,.28)'; c.beginPath(); c.arc(p.x * W, p.y * H, u * .055, 0, Math.PI * 2); c.fill(); }
  for (let i = ripples.length - 1; i >= 0; i--) {
    const r = ripples[i], k = (t - r.t) / 420;
    if (k > 1) { ripples.splice(i, 1); continue; }
    c.strokeStyle = `rgba(255,255,255,${.6 * (1 - k)})`; c.lineWidth = u * .004;
    c.beginPath(); c.arc(r.x * W, r.y * H, u * (.05 + k * .09), 0, Math.PI * 2); c.stroke();
  }
}

/* ================= HUD ================= */
/* How the WebRTC media gets here, from the selected ICE candidate pair:
 * relay = through a TURN server, srflx/prflx = straight through NAT (STUN found the address),
 * host = straight on the same network. */
function rtcRoute(rep) {
  let pair = null;
  rep.forEach(r => { if (r.type === 'transport' && r.selectedCandidatePairId) pair = rep.get(r.selectedCandidatePairId); });
  if (!pair) rep.forEach(r => { if (!pair && r.type === 'candidate-pair' && r.nominated && r.state === 'succeeded') pair = r; });
  if (!pair) return null;
  const l = rep.get(pair.localCandidateId) || {}, r = rep.get(pair.remoteCandidateId) || {};
  const types = [l.candidateType, r.candidateType];
  const mode = types.includes('relay') ? 'TURN relay' : types.some(t => t === 'srflx' || t === 'prflx') ? 'P2P (STUN)' : 'direct (LAN)';
  const proto = (l.candidateType === 'relay' && l.relayProtocol) || l.protocol || '';
  const via = types.includes('relay') ? (l.candidateType === 'relay' ? (l.url || l.address || '') : (r.address || '')) : (r.address || '');
  return { mode, proto, via: String(via).replace(/^turns?:/, '').replace(/\?.*$/, ''), local: l.candidateType, remote: r.candidateType };
}
let prevBytes = 0, prevT = 0;
async function tickStats() {
  if (st.pc) {
    try {
      const rep = await st.pc.getStats();
      rep.forEach(r => {
        if (r.type === 'inbound-rtp' && r.kind === 'video') {
          const t = r.timestamp, b = r.bytesReceived;
          if (prevT) st.localStats.kbps = (b - prevBytes) * 8 / (t - prevT);
          prevBytes = b; prevT = t;
          st.localStats.fps = r.framesPerSecond || 0;
          st.localStats.lost = r.packetsLost || 0;
          st.localStats.decoder = r.decoderImplementation || (r.powerEfficientDecoder === true ? 'hardware' : r.powerEfficientDecoder === false ? 'software' : '');
          if (r.jitterBufferEmittedCount) st.localStats.jbMs = Math.round(r.jitterBufferDelay / r.jitterBufferEmittedCount * 1000);
          if (r.framesDecoded && r.totalDecodeTime) st.localStats.decMs = (r.totalDecodeTime / r.framesDecoded * 1000).toFixed(1);
          st.localStats.size = `${r.frameWidth || 0}×${r.frameHeight || 0}`;
        }
        if (r.type === 'candidate-pair' && r.nominated && r.state === 'succeeded' && r.currentRoundTripTime != null)
          st.localStats.rtt = Math.round(r.currentRoundTripTime * 1000);
      });
      st.localStats.route = rtcRoute(rep);
    } catch (_) {}
  }
  const hud = $('#hud');
  hud.hidden = !S.stats;
  const p = st.phoneStats, l = st.localStats;
  if (S.stats) {
    const dec = l.decoder || (st.hw[(codecLabel() === 'H.265') ? 'h265' : 'h264'] ? 'hardware' : '?');
    hud.innerHTML = DEMO ? `<b>demo</b> · no server<br><span class="dim">input</span> ${st.inputs}<span class="hex">${st.lastHex}</span>` :
      `<b>${Math.round(l.fps || 0)}</b> fps <span class="dim">/ ${p.fps || '—'}</span> · <b>${((l.kbps || 0) / 1000).toFixed(1)}</b> Mbps<br>` +
      `rtt <b>${l.rtt ?? p.rtt_ms ?? '—'}</b> ms · jb <b>${l.jbMs ?? '—'}</b> ms · lost <b>${l.lost ?? 0}</b><br>` +
      `${codecLabel()} · ${l.size || '—'} · ${S.mode} <span class="dim">${p.target_kbps ? '→ ' + (p.target_kbps / 1000).toFixed(1) + ' Mbps' : ''}</span><br>` +
      `<span class="dim">webrtc</span> <span class="rtc">${l.route ? l.route.mode : '—'}</span>${l.route ? ` ${l.route.proto} <span class="dim">${l.route.local}→${l.route.remote}</span>` : ''}<br>` +
      `<span class="dim">decoder</span> ${dec} ${l.decMs ? l.decMs + ' ms' : ''} · <span class="dim">usb</span> ${p.path || '—'}${st.hasAudio ? ` · <span class="dim">sound</span> ${p.audio ? (video.muted ? 'muted' : 'on') : 'off'}` : ''}<br>` +
      `<span class="dim">panel</span> ${st.panel} · <span class="dim">input</span> ${st.inputs}<span class="hex">${st.lastHex}</span>`;
  }
  $('#chipText').textContent = st.device ? `${st.device.name}${l.rtt != null ? ' · ' + l.rtt + ' ms' : ''}` : (DEMO ? 'demo' : 'no device');
  $('#chipDot').className = 'dot' + (st.connected || DEMO ? '' : ' warn');
  if (statusRefresh) statusRefresh();
}
setInterval(tickStats, 1000);
/* Hold the chip for the layout check; a tap toggles the stats. */
let chipHold = 0, chipHeld = false;
$('#chip').addEventListener('pointerdown', () => {
  chipHeld = false;
  chipHold = setTimeout(() => {
    chipHeld = true; S.lc = !S.lc; save(); layoutCheck(S.lc);
    toast(S.lc ? 'Layout check on. Hold the chip again to hide it.' : 'Layout check off');
  }, 700);
});
for (const t of ['pointerup', 'pointercancel', 'pointerleave']) $('#chip').addEventListener(t, () => clearTimeout(chipHold));
$('#chip').addEventListener('click', () => { if (chipHeld) { chipHeld = false; return; } S.stats = !S.stats; save(); tickStats(); syncMenuStates(); });

/* ================= floating ball + menu ================= */
const ball = $('#ball'), menu = $('#menu');
const MARGIN = 8;
let idleTimer = 0, B = { t: 0, r: 0, b: 0, l: 0 };
function bounds() {
  const probe = document.createElement('div');
  probe.style.cssText = 'position:fixed;inset:0;padding:env(safe-area-inset-top,0px) env(safe-area-inset-right,0px) env(safe-area-inset-bottom,0px) env(safe-area-inset-left,0px);visibility:hidden;pointer-events:none';
  document.body.appendChild(probe);
  const cs = getComputedStyle(probe);
  const b = { t: parseFloat(cs.paddingTop), r: parseFloat(cs.paddingRight), b: parseFloat(cs.paddingBottom), l: parseFloat(cs.paddingLeft) };
  probe.remove();
  return b;
}
function placeBall(x, y, snap) {
  const W = innerWidth, H = innerHeight, s = ball.offsetWidth || 58;
  const minX = B.l + MARGIN, maxX = W - B.r - MARGIN - s, minY = B.t + MARGIN, maxY = H - B.b - MARGIN - s;
  if (snap) x = (x + s / 2 < W / 2) ? minX : maxX;
  x = clamp(x, minX, maxX); y = clamp(y, minY, maxY);
  ball.style.left = x + 'px'; ball.style.top = y + 'px';
  return { x, y };
}
function settleBall() {
  const r = ball.getBoundingClientRect(), p = placeBall(r.left, r.top, true);
  ballPos = { side: p.x < innerWidth / 2 ? 'l' : 'r', fy: p.y / innerHeight };
  try { localStorage.setItem(KEY + '-ball', JSON.stringify(ballPos)); } catch (e) {}
}
function restoreBall() {
  B = bounds();
  placeBall(ballPos && ballPos.side === 'l' ? 0 : innerWidth, (ballPos ? ballPos.fy : .58) * innerHeight, true);
}
function armIdle() {
  ball.classList.remove('idle'); clearTimeout(idleTimer);
  idleTimer = setTimeout(() => { if (menu.hidden) ball.classList.add('idle'); }, 3000);
}
let drag = null;
ball.addEventListener('pointerdown', e => {
  e.preventDefault(); B = bounds();
  try { ball.setPointerCapture(e.pointerId); } catch (_) {}
  const r = ball.getBoundingClientRect();
  drag = { id: e.pointerId, sx: e.clientX, sy: e.clientY, ox: r.left, oy: r.top, moved: false };
  armIdle();
});
ball.addEventListener('pointermove', e => {
  if (!drag || e.pointerId !== drag.id) return;
  const dx = e.clientX - drag.sx, dy = e.clientY - drag.sy;
  if (!drag.moved && Math.hypot(dx, dy) > 6) { drag.moved = true; ball.classList.add('drag'); closeMenu(); }
  if (drag.moved) placeBall(drag.ox + dx, drag.oy + dy, false);
});
function ballUp(e) {
  if (!drag || e.pointerId !== drag.id) return;
  const moved = drag.moved; drag = null; ball.classList.remove('drag');
  if (moved) settleBall(); else if (!$('#disc').hidden) toDevices(); else toggleMenu();
  armIdle();
}
ball.addEventListener('pointerup', ballUp);
ball.addEventListener('pointercancel', ballUp);
ball.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); $('#disc').hidden ? toggleMenu() : toDevices(); } });
function toDevices() { location.href = '/panel.html#devices'; }
window.addEventListener('resize', () => { restoreBall(); if (!menu.hidden) positionMenu(); });
function positionMenu() {
  const r = ball.getBoundingClientRect(), mw = menu.offsetWidth, mh = menu.offsetHeight, W = innerWidth, H = innerHeight;
  let x = r.left < W / 2 ? r.right + 10 : r.left - 10 - mw;
  if (x < B.l + 8 || x + mw > W - B.r - 8) x = (W - mw) / 2;
  menu.style.left = x + 'px';
  menu.style.top = clamp(r.top + r.height / 2 - mh / 2, B.t + 8, H - B.b - 8 - mh) + 'px';
}
function toggleMenu() { menu.hidden ? openMenu() : closeMenu(); }
function openMenu() { syncMenuStates(); menu.hidden = false; ball.setAttribute('aria-expanded', 'true'); ball.classList.remove('idle'); positionMenu(); }
function closeMenu() { if (menu.hidden) return; menu.hidden = true; ball.setAttribute('aria-expanded', 'false'); armIdle(); }
document.addEventListener('pointerdown', e => { if (!menu.hidden && !menu.contains(e.target) && e.target !== ball) closeMenu(); }, true);
document.addEventListener('keydown', e => { if (e.key === 'Escape') { closeMenu(); closeSheet(); } });
function syncMenuStates() {
  const dk = desk(), screens = (st.ready && st.ready.screens) || [];
  for (const a of ['back', 'home', 'recents', 'photo']) menu.querySelector(`[data-act="${a}"]`).hidden = dk && a !== 'photo';
  menu.querySelector('[data-act="photo"] span').textContent = dk ? 'Upload' : 'Photo';
  $('#menuSound').hidden = dk && !(st.ready && st.ready.caps && st.ready.caps.audio);
  $('#menuMonitor').hidden = !dk || screens.length < 2;
  const sb = $('#menuScreen');
  sb.classList.toggle('on', st.panel === 'off');
  sb.querySelector('span').textContent = dk ? (st.panel === 'off' ? 'Display on' : 'Display off') : st.panel === 'off' ? 'Screen on' : 'Screen off';
  $('#menuStats').classList.toggle('on', S.stats);
  const snd = $('#menuSound'), audible = S.sound && !video.muted;
  snd.classList.toggle('on', audible); snd.classList.toggle('muted', !audible);
  snd.querySelector('span').textContent = audible ? 'Sound on' : 'Sound off';
  $('#menuLock').querySelector('span').textContent = st.locked ? 'Wake' : 'Lock';
  const bark = $('#menuBark'), barkOn = st.device && st.device.bark;
  bark.hidden = !st.device || typeof st.device.bark !== 'boolean'; /* only when the server has a Bark URL */
  bark.classList.toggle('on', !!barkOn); bark.classList.toggle('muted', !barkOn);
  bark.querySelector('span').textContent = barkOn ? 'Bark on' : 'Bark off';
  $('#menuFull').classList.toggle('on', standalone() || !!(document.fullscreenElement || document.webkitFullscreenElement));
}
const standalone = () => navigator.standalone === true || matchMedia('(display-mode: standalone)').matches || matchMedia('(display-mode: fullscreen)').matches;

/* Sound: Safari only starts audio from a tap, so the first touch on the phone's picture unmutes it.
 * Switching Sound off also stops the capture on the phone, so its own speaker plays again. */
function unmute() {
  if (!S.sound || !video.muted) return;
  video.muted = false;
  video.play().catch(() => {});
  syncMenuStates();
}
function toggleSound() {
  const on = !(S.sound && !video.muted);
  S.sound = on; save();
  video.muted = !on;
  if (on) video.play().catch(() => {});
  sendCtrl('audio.set', { on }, true).then(r => {
    if (on && !r.on) toast(st.device && st.device.phone && st.device.phone.sdk < 31 ? 'Phone sound needs Android 12 or newer' : 'The phone isn\'t sending sound (Settings → Phone → Sound)');
    else toast(on ? 'Sound plays here — the phone is quiet' : 'Sound back on the phone');
  }).catch(() => toast(on ? 'Sound on' : 'Sound off'));
  syncMenuStates();
}
/* Bark: the server's push of phone notifications to an iPhone. Paused per router, for every viewer;
 * notifications still reach this page. */
async function toggleBark() {
  if (!st.device) return;
  const on = !st.device.bark;
  try {
    const r = await api('PATCH', `/devices/${encodeURIComponent(st.device.id)}`, { bark: on });
    if (!r.ok) throw new Error((await r.json().catch(() => ({}))).msg || `HTTP ${r.status}`);
    st.device.bark = on;
    toast(on ? 'Bark notifications on' : 'Bark notifications paused');
  } catch (e) { toast(`Couldn't switch Bark: ${e.message}`); }
  syncMenuStates();
}
function toggleFullscreen() {
  const d = document, root = d.documentElement;
  if (standalone()) { toast('Already full screen (Home Screen app)'); return; }
  const fsEl = d.fullscreenElement || d.webkitFullscreenElement;
  const req = root.requestFullscreen || root.webkitRequestFullscreen;
  if (fsEl) { (d.exitFullscreen || d.webkitExitFullscreen).call(d); return; }
  if (req && (d.fullscreenEnabled || d.webkitFullscreenEnabled)) {
    Promise.resolve(req.call(root)).catch(() => openSheet('full'));
    return;
  }
  openSheet('full'); /* iPhone: only Home Screen web apps run full screen */
}
/* A fresh WebRTC session to the same phone: the fix for a frozen picture or a stuck connection. */
function reconnectStream() {
  if (st.sid && st.sid !== 'pending') wsSend({ t: 'session.end', d: {} });
  closePeer();
  st.sid = null; st.ended = true; wantSession = true;
  setOverlay('Reconnecting', st.device ? `Connecting to ${st.device.name}…` : '…');
  if (!st.ws) connectWs(); else setTimeout(maybeStart, 300);
}
document.addEventListener('fullscreenchange', () => { layout(); syncMenuStates(); lockKeys(!!document.fullscreenElement); });
document.addEventListener('webkitfullscreenchange', () => { layout(); syncMenuStates(); });
menu.addEventListener('click', e => {
  const b = e.target.closest('button[data-act]'); if (!b) return;
  const a = b.dataset.act;
  if (['photo', 'clip', 'settings', 'keys'].includes(a)) { closeMenu(); openSheet(a); }
  else if (['back', 'home', 'recents'].includes(a)) { sendCtrl('nav', { button: a }).catch(() => toast('Not connected')); closeMenu(); }
  else if (a === 'screen') {
    const on = st.panel === 'off', what = desk() ? 'Display' : 'Phone screen';
    sendCtrl('screen.power', { on }, true).then(() => { st.panel = on ? 'on' : 'off'; syncMenuStates();
      toast(on ? `${what} turned on` : `${what} turned off — stream continues`); }).catch(e => toast(e.msg || e.message));
  } else if (a === 'monitor') {
    const n = ((st.ready && st.ready.screens) || []).length, cur = (st.screen && st.screen.index) || 0;
    if (n > 1) selectScreen((cur + 1) % n);
  } else if (a === 'stats') { S.stats = !S.stats; save(); tickStats(); syncMenuStates(); }
  else if (a === 'sound') { toggleSound(); }
  else if (a === 'bark') { toggleBark(); }
  else if (a === 'lock') {
    closeMenu();
    const N = desk() ? 'Computer' : 'Phone';
    if (st.locked) cmd('phone.wake', {}, `${N} woken`).then(ok => { if (ok) { st.locked = false; syncMenuStates(); } });
    else cmd('phone.sleep', {}, `${N} locked`).then(ok => { if (ok) { st.locked = true; syncMenuStates(); } });
  }
  else if (a === 'reconnect') { closeMenu(); reconnectStream(); }
  else if (a === 'fullscreen') { closeMenu(); toggleFullscreen(); }
  else if (a === 'devices') { toDevices(); }
});

/* ================= sheets ================= */
let openId = null;
function openSheet(id) {
  closeSheet();
  openId = id;
  $('#backdrop').hidden = false;
  $('#sheet-' + id).hidden = false;
  if (id === 'clip') refreshClip();
  if (id === 'settings') renderTab(currentTab);
  if (id === 'photo') $('#ph-dest').textContent = desk() ? `to ${uploadDir()}` : `to ${S.destDir}${S.mediaScan ? ' · added to Gallery' : ''}`;
  $('#photoInput').accept = desk() ? '' : 'image/*';
  ball.hidden = true;
}
function closeSheet() {
  if (!openId) return;
  $('#sheet-' + openId).hidden = true; $('#backdrop').hidden = true;
  openId = null; ball.hidden = false; armIdle();
  stopLogcat();
}
$('#backdrop').addEventListener('click', closeSheet);
$$('[data-close]').forEach(b => b.addEventListener('click', closeSheet));

/* ================= clipboard (§4.8) ================= */
let androidClip = '';
const clipHist = [];
function pushHist(dir, text) {
  if (!text) return;
  const i = clipHist.findIndex(h => h.text === text); if (i >= 0) clipHist.splice(i, 1);
  clipHist.unshift({ dir, text }); if (clipHist.length > 8) clipHist.pop();
}
function refreshClip() {
  $('#clipAndroid').value = androidClip;
  $('#clipSyncState').textContent = S.clipSync ? 'Sync on · text both ways' : 'Sync off · manual only';
  const ul = $('#clipHist'); ul.textContent = '';
  if (!clipHist.length) ul.append(el('li', { class: 'note', text: 'Nothing copied yet.' }));
  for (const h of clipHist) {
    const b = el('button', { type: 'button' }, el('span', { class: 'pill ' + (h.dir === 'android' ? 'ok' : ''), text: h.dir === 'android' ? 'Android' : 'iPhone' }),
      el('span', { class: 't', text: h.text }));
    b.addEventListener('click', () => { $('#clipAndroid').value = h.text; $('#clipAndroid').focus(); });
    ul.append(el('li', {}, b));
  }
}
function sendClip(text, paste) {
  if (!text.trim()) { toast('Nothing to send — the text is empty'); return; }
  sendCtrl('clip.set', { text, paste: !!paste }, true).then(() => {
    androidClip = text; pushHist('iphone', text); refreshClip();
    toast(paste ? 'Sent and pasted on Android' : 'Android clipboard updated');
  }).catch(e => toast('Not sent: ' + e.message));
}
$('#clipSend').addEventListener('click', () => sendClip($('#clipAndroid').value, S.autoPaste));
$('#clipSendPaste').addEventListener('click', () => sendClip($('#clipAndroid').value, true));
$('#clipCopy').addEventListener('click', () => copyText($('#clipAndroid').value).then(ok => toast(ok ? 'Copied to iPhone clipboard' : 'Copy blocked — select the text and copy it')));
$('#clipPaste').addEventListener('paste', () => setTimeout(() => toast('Pasted — tap Send to Android'), 0));
$('#clipPasteSend').addEventListener('click', () => { sendClip($('#clipPaste').value, S.autoPaste); $('#clipPaste').value = ''; });
$('#clipPasteEdit').addEventListener('click', () => { $('#clipAndroid').value = $('#clipPaste').value; $('#clipPaste').value = ''; });
let clipbarTimer = 0;
function androidCopied(text) {
  androidClip = text; pushHist('android', text);
  if (openId === 'clip') refreshClip();
  if (!S.clipSync) return;
  $('#clipbarText').textContent = text; $('#clipbar').hidden = false;
  clearTimeout(clipbarTimer); clipbarTimer = setTimeout(() => { $('#clipbar').hidden = true; }, 12000);
}
$('#clipbarCopy').addEventListener('click', () => copyText(androidClip).then(ok => { toast(ok ? 'Copied to iPhone clipboard' : 'Copy blocked — open Clipboard to copy'); $('#clipbar').hidden = true; }));
$('#clipbarClose').addEventListener('click', () => { $('#clipbar').hidden = true; });
$('#clipSim').hidden = !DEMO;
$('#clipSim').addEventListener('click', () => { closeSheet(); androidCopied('Gift code: SPRING-26-QX9'); });

/* ================= keyboard ================= */
const QUICK = [['Enter', 66], ['Delete', 67], ['Tab', 61], ['Esc', 111], ['↑', 19], ['↓', 20], ['←', 21], ['→', 22], ['Vol −', 25], ['Vol +', 24], ['Power', 26], ['Menu', 82]];
/* computers: combinations as HID usages; ⌘ stands for the remote's own command key */
function deskQuick() {
  const mac = st.device && st.device.platform === 'macos', win = st.device && st.device.platform === 'windows';
  const CMD = mac ? 0xE3 : 0xE0, ALT = 0xE2, SHIFT = 0xE1, GUI = 0xE3;
  const k = [['Esc', [0x29]], ['Tab', [0x2B]], ['Enter', [0x28]], ['⌫', [0x2A]], ['Del', [0x4C]],
    ['↑', [0x52]], ['↓', [0x51]], ['←', [0x50]], ['→', [0x4F]], ['Home', [0x4A]], ['End', [0x4D]], ['PgUp', [0x4B]], ['PgDn', [0x4E]],
    [mac ? '⌘C' : 'Ctrl+C', [CMD, 0x06]], [mac ? '⌘V' : 'Ctrl+V', [CMD, 0x19]], [mac ? '⌘X' : 'Ctrl+X', [CMD, 0x1B]],
    [mac ? '⌘Z' : 'Ctrl+Z', [CMD, 0x1D]], [mac ? '⌘A' : 'Ctrl+A', [CMD, 0x04]], [mac ? '⌘S' : 'Ctrl+S', [CMD, 0x16]],
    [mac ? '⌘Tab' : 'Alt+Tab', [mac ? GUI : ALT, 0x2B]], [mac ? '⌘Q' : 'Alt+F4', mac ? [GUI, 0x14] : [ALT, 0x3D]],
    [mac ? '⌘Space' : win ? 'Win' : 'Super', mac ? [GUI, 0x2C] : [GUI]]];
  if (win) k.push(['Win+D', [GUI, 0x07]], ['Win+E', [GUI, 0x08]], ['Ctrl+Shift+Esc', [0xE0, SHIFT, 0x29]], ['Win+L', [GUI, 0x0F]]);
  if (mac) k.push(['⌃↑ Mission', [0xE0, 0x52]], ['⌘⇧3 Shot', [GUI, SHIFT, 0x20]], ['⌃⌘Q Lock', [0xE0, GUI, 0x14]]);
  for (let i = 1; i <= 12; i++) k.push(['F' + i, [0x39 + i]]);
  return k;
}
function renderQuickKeys() {
  const box = $('#kbKeys'); box.textContent = '';
  if (desk()) {
    for (const [label, hids] of deskQuick())
      box.append(el('button', { type: 'button', text: label, onclick: () => { if (!st.connected) toast('Not connected'); else sendCombo(hids); } }));
  } else {
    for (const [label, code] of QUICK)
      box.append(el('button', { type: 'button', text: label, onclick: () => sendCtrl('key', { action: 'press', code, meta: 0 }).catch(() => toast('Not connected')) }));
  }
  const sub = $('#sheet-keys header small');
  if (sub) sub.textContent = desk() ? 'Typing on a keyboard here goes straight to the computer; this sends text and shortcuts' : 'Text goes into the focused field on the phone';
}
renderQuickKeys();
$('#kbSend').addEventListener('click', () => {
  const v = $('#kbText').value; if (!v) { toast('Type something first'); return; }
  sendCtrl('text', { text: v }, true).then(() => { $('#kbText').value = ''; toast('Text sent'); }).catch(e => toast(e.message));
});
$('#kbText').addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); $('#kbSend').click(); } });

/* ================= photo upload (A.6 file.push + A.8 chunks) ================= */
let xferId = 1;
const uploads = new Map();
const uploadQ = [];
let uploading = false;
function stamp() {
  const d = new Date(), p = n => String(n).padStart(2, '0');
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}_${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
}
$('#photoInput').addEventListener('change', e => {
  for (const f of Array.from(e.target.files || [])) addUpload(f);
  e.target.value = '';
  pumpUploads();
});
function addUpload(f) {
  $('#queueEmpty').hidden = true;
  const ext = (f.name.split('.').pop() || 'jpg').toLowerCase();
  const name = /^image\.(jpe?g|heic|png)$/i.test(f.name) || !f.name ? `IMG_${stamp()}.${ext}` : f.name;
  const img = el('img', { alt: '' });
  try { img.src = URL.createObjectURL(f); } catch (_) {}
  const meta = el('div', { class: 'meta', text: `${fmtSize(f.size)} · ${f.type || 'image'} · waiting` });
  const fill = el('i'), stp = el('span', { class: 'pill', text: 'Queued' });
  $('#queue').prepend(el('li', { class: 'qi' }, img, el('div', { style: 'min-width:0' }, el('div', { class: 'nm', text: name }), meta, el('div', { class: 'bar' }, fill)), stp));
  uploadQ.push({ f, name, meta, fill, st: stp });
}
const waitDrain = c => new Promise(res => { if (c.bufferedAmount < 1024 * 1024) return res(); c.onbufferedamountlow = () => { c.onbufferedamountlow = null; res(); }; });
async function pumpUploads() {
  if (uploading) return;
  uploading = true;
  while (uploadQ.length) {
    const j = uploadQ.shift();
    if (j.f.size > S.maxMb * 1048576) { j.st.className = 'pill bad'; j.st.textContent = 'Too large'; continue; }
    try {
      j.st.className = 'pill warn'; j.st.textContent = 'Hashing';
      const buf = await j.f.arrayBuffer(), sum = await sha256(buf);
      const xfer = xferId++, path = `${(desk() ? uploadDir() : S.destDir).replace(/[\/\\]+$/, '')}/${j.name}`;
      uploads.set(xfer, j);
      await sendCtrl('file.push', { xfer, path, size: j.f.size, sha256: sum, overwrite: S.overwrite, media_scan: S.mediaScan }, true);
      j.st.textContent = 'Sending';
      const c = st.ch.file, bytes = new Uint8Array(buf), CH = 16 * 1024;
      for (let off = 0, seq = 0; off < bytes.length || (off === 0 && bytes.length === 0); off += CH, seq++) {
        if (!DEMO) { if (!c || c.readyState !== 'open') throw new Error('not connected'); await waitDrain(c); }
        const n = Math.min(CH, bytes.length - off), m = new Uint8Array(8 + n), v = new DataView(m.buffer);
        v.setUint32(0, xfer); v.setUint32(4, seq); m.set(bytes.subarray(off, off + n), 8);
        if (!DEMO) c.send(m);
        j.fill.style.width = ((off + n) / Math.max(1, bytes.length) * 100).toFixed(1) + '%';
        if (bytes.length === 0) break;
      }
      const res = await sendCtrl('file.done', { xfer }, true);
      const gallery = S.mediaScan && !desk();
      j.st.className = 'pill ok'; j.st.textContent = gallery ? 'In Gallery' : 'Saved';
      j.meta.textContent = `${fmtSize(j.f.size)} · ${(res && res.path) || path}`;
      toast(`${j.name} saved to ${gallery ? 'the Android Gallery' : (res && res.path) || path}`);
    } catch (e) {
      j.st.className = 'pill bad'; j.st.textContent = 'Failed';
      j.meta.textContent = e.msg || e.message;
    }
  }
  uploading = false;
}

/* computers: the agent's upload folder (session.ready), "~" until it is known */
const uploadDir = () => (st.ready && st.ready.upload_dir) || '~/Downloads/RemoDroid';

/* ---- pulls (phone -> iPhone) ---- */
function onFileChunk(u8) {
  if (u8.length < 8) return;
  const v = new DataView(u8.buffer, u8.byteOffset), xfer = v.getUint32(0);
  const p = st.pulls.get(xfer);
  if (!p) return;
  p.parts.push(u8.slice(8)); p.got += u8.length - 8;
  if (p.meta) p.meta.textContent = `${fmtSize(p.got)} / ${fmtSize(p.size)}`;
}
function finishPull(xfer) {
  const p = st.pulls.get(xfer); st.pulls.delete(xfer);
  const blob = new Blob(p.parts), a = el('a', { href: URL.createObjectURL(blob), download: p.name });
  document.body.append(a); a.click(); a.remove();
  if (p.meta) p.meta.textContent = fmtSize(p.size) + ' · saved';
  toast(p.name + ' downloaded');
}
async function pull(path, name, meta) {
  const xfer = xferId++;
  try {
    const r = await sendCtrl('file.pull', { xfer, path }, true);
    st.pulls.set(xfer, { name, size: r.size || 0, parts: [], got: 0, meta });
  } catch (e) { toast(e.msg || e.message); }
}

/* ================= settings (all of them) ================= */
const TABS = [['stream', 'Stream'], ['phone', 'Phone'], ['files', 'Files'], ['logs', 'Logs'], ['check', 'Device check'], ['conn', 'Connection']];
let currentTab = 'stream', statusRefresh = null, logcatStream = 0;
for (const [id, label] of TABS) {
  const b = el('button', { type: 'button', role: 'tab', text: label });
  b.dataset.tab = id;
  b.addEventListener('click', () => renderTab(id));
  $('#tabs').append(b);
}
function quality() {
  return { mode: S.mode, preset: S.preset, fps: S.fps, kbps: S.kbps, codec: S.codec };
}
/* names the agent understands in agent.config (agent/phone.c) */
const AGENT_KEYS = ['screenOff', 'lockOnDisc', 'grace', 'clipSync', 'autoPaste', 'overheat', 'rotation', 'route', 'relay', 'destDir', 'mediaScan', 'overwrite', 'maxMb', 'audio'];
function changed(key) {
  save();
  if (['mode', 'preset', 'fps', 'kbps', 'codec'].includes(key)) {
    sendCtrl('quality.set', quality(), true).then(() => toast('Quality updated')).catch(e => { if (!DEMO) toast(e.msg || e.message); });
    if (key === 'codec') toast('The codec changes on the next connection');
  }
  if (key === 'rotation') sendCtrl('rotate', { to: S.rotation }).catch(() => {});
  if (AGENT_KEYS.includes(key)) cmd('agent.config', { [key]: S[key] }, 'Saved to the router');
  if (key === 'stats') tickStats();
}
const lbl = (text, help) => el('div', { class: 'lbl' }, text, help ? el('small', { text: help }) : null);
function seg(key, opts) {
  const box = el('div', { class: 'seg', role: 'group' });
  for (const o of opts) {
    const [v, t] = Array.isArray(o) ? o : [o, String(o)];
    const b = el('button', { type: 'button', 'aria-pressed': String(S[key] === v), text: t });
    b.addEventListener('click', () => { S[key] = v; box.querySelectorAll('button').forEach(x => x.setAttribute('aria-pressed', String(x === b))); changed(key); if (key === 'mode') renderTab(currentTab); });
    box.append(b);
  }
  return box;
}
const segRow = (key, label, opts, help) => el('div', { class: 'row stack' }, lbl(label, help), seg(key, opts));
function toggleRow(key, label, help) {
  const cb = el('input', { type: 'checkbox', class: 'sw', id: 'set-' + key, 'aria-label': label });
  cb.checked = !!S[key];
  cb.addEventListener('change', () => { S[key] = cb.checked; changed(key); });
  return el('div', { class: 'row' }, lbl(label, help), cb);
}
function rangeRow(key, label, min, max, step, fmt, help) {
  const out = el('span', { class: 'val', text: fmt(S[key]) });
  const r = el('input', { type: 'range', min, max, step, id: 'set-' + key, 'aria-label': label });
  r.value = S[key];
  r.addEventListener('input', () => { S[key] = +r.value; out.textContent = fmt(S[key]); });
  r.addEventListener('change', () => changed(key));
  return el('div', { class: 'row stack' }, el('div', { class: 'row', style: 'padding:0;border:0' }, lbl(label, help), out), r);
}
function stepRow(key, label, min, max, step, unit, help) {
  const out = el('output', { text: S[key] + unit });
  const set = v => { S[key] = clamp(v, min, max); out.textContent = S[key] + unit; changed(key); };
  return el('div', { class: 'row' }, lbl(label, help), el('div', { class: 'step' },
    el('button', { type: 'button', 'aria-label': 'Decrease ' + label, text: '−', onclick: () => set(S[key] - step) }), out,
    el('button', { type: 'button', 'aria-label': 'Increase ' + label, text: '+', onclick: () => set(S[key] + step) })));
}
function textRow(key, label, help) {
  const i = el('input', { type: 'text', id: 'set-' + key, autocomplete: 'off', autocapitalize: 'off', spellcheck: 'false' });
  i.value = S[key];
  i.addEventListener('change', () => { S[key] = i.value.trim() || DEFAULTS[key]; i.value = S[key]; changed(key); });
  return el('div', { class: 'row stack' }, lbl(label, help), i);
}
const group = (title, ...rows) => el('div', { class: 'group' }, el('h3', { text: title }), ...rows);
function kv(pairs) {
  const dl = el('dl', { class: 'kv' });
  for (const [k, v] of pairs) dl.append(el('dt', { text: k }), v instanceof Node ? el('dd', {}, v) : el('dd', { text: String(v) }));
  return dl;
}
const pill = (text, cls) => el('span', { class: 'pill ' + (cls || ''), text });

function renderTab(id) {
  currentTab = id;
  if (id !== 'logs') stopLogcat();
  renderProto = null; statusRefresh = null;
  $$('#tabs button').forEach(b => b.setAttribute('aria-selected', String(b.dataset.tab === id)));
  const body = $('#settingsBody'); body.textContent = '';
  const add = (...n) => body.append(...n);
  const d = st.device, ph = (d && d.phone) || {};
  $('#st-sub').textContent = d ? (desk() ? `${d.name} · ${ph.os || d.platform}` : `${d.name} · ${ph.model || 'phone'}${ph.android ? ' · Android ' + ph.android : ''}`) : (DEMO ? 'Demo mode' : 'No device');
  $$('#tabs button').forEach(b => { if (b.dataset.tab === 'phone') b.textContent = desk() ? 'Computer' : 'Phone'; });

  if (id === 'stream') {
    add(group('Quality',
      segRow('mode', 'Quality mode', [['auto', 'Auto'], ['manual', 'Manual']], 'Auto adapts bitrate and resolution to the network.'),
      segRow('preset', 'Resolution', desk() ? [['native', 'Native'], '1080p', '720p', '540p'] : ['1080p', '720p', '540p', '360p'], 'Changing it restarts the encoder for about 0.2 s.'),
      segRow('fps', 'Frame rate', [[30, '30'], [60, '60'], [90, '90'], [120, '120']]),
      rangeRow('kbps', S.mode === 'auto' ? 'Max bitrate' : 'Bitrate', 1000, 20000, 500, v => (v / 1000).toFixed(1) + ' Mbps'),
      segRow('codec', 'Codec', [['auto', 'Auto'], ['h265', 'H.265'], ['h264', 'H.264']], 'Auto uses H.265 when this iPhone decodes it in hardware.')));
    add(group('Overlay',
      toggleRow('stats', 'Stats overlay', 'fps, bitrate, round-trip time, decoder, last input bytes'),
      toggleRow('touches', 'Show touches', 'Draw your touch points (demo stream only)')));
    if (desk()) add(group('Keyboard',
      segRow('swapCmd', '⌘ and Ctrl', [['auto', 'Auto'], ['on', 'Swap'], ['off', 'As is']], 'Auto maps them when only one side is a Mac: from a Mac, ⌘C copies on Windows or Linux and Ctrl+C still stops a terminal; towards a Mac, Ctrl+C copies.')));
  }
  if (id === 'phone' && desk()) renderComputerTab(add);
  if (id === 'phone' && !desk()) {
    const card = el('div', { class: 'card' });
    statusRefresh = () => {
      const dd = st.device || {}, p = dd.phone || {}, a = (dd.adb) || {};
      card.textContent = '';
      card.append(kv([
        ['Model', `${p.model || '—'}${p.android ? ' · Android ' + p.android : ''}`],
        ['Battery', p.battery ? `${p.battery.level}% · ${p.battery.charging ? 'charging' : 'on battery'} · ${p.battery.temp_c} °C` : '—'],
        ['Phone screen', pill(st.panel === 'off' ? 'Panel off' : (p.screen || '—'), st.panel === 'off' ? 'ok' : '')],
        ['Phone server', pill(p.server || '—', p.server === 'running' ? 'ok' : 'bad')],
        ['USB / ADB', pill(p.adb_state || (p.attached ? 'attached' : 'not attached'), p.adb_state === 'authorized' ? 'ok' : 'warn')],
        ['Router', dd.online ? `online · ${(dd.agent && dd.agent.arch) || ''}` : 'offline'],
      ]));
    };
    statusRefresh();
    add(group('Status', card));
    add(group('Screen & lock',
      toggleRow('screenOff', 'Turn phone screen off while connected', 'The stream keeps running; only the physical panel goes dark.'),
      toggleRow('lockOnDisc', 'Lock phone after disconnect'),
      stepRow('grace', 'Lock grace period', 0, 60, 5, ' s', 'Reconnect within this time and nothing locks.'),
      segRow('rotation', 'Rotation', [['auto', 'Auto'], ['portrait', 'Portrait'], ['landscape', 'Landscape']])));
    add(group('Sound',
      toggleRow('audio', 'Play the phone\'s sound here', 'Like scrcpy: the phone\'s speaker goes quiet while you\'re connected. Android 12 or newer.')));
    add(group('Clipboard',
      toggleRow('clipSync', 'Clipboard sync', 'Show copied Android text here, ready to copy with one tap'),
      toggleRow('autoPaste', 'Paste into focused field after sending')));
    add(group('Safety', stepRow('overheat', 'Overheat alert', 35, 60, 1, ' °C', 'Warn when the battery gets hotter than this.')));
    const confirmBox = el('div', { class: 'confirm', hidden: '' },
      el('span', { text: 'Reboot the phone? The stream drops for about a minute.', style: 'flex:1;min-width:180px' }),
      el('button', { class: 'btn danger', type: 'button', text: 'Reboot', onclick: () => { cmd('phone.reboot', { mode: 'normal' }, 'Reboot requested'); confirmBox.hidden = true; } }),
      el('button', { class: 'btn', type: 'button', text: 'Cancel', onclick: () => { confirmBox.hidden = true; } }));
    add(group('Actions', el('div', { class: 'btns', style: 'padding-top:10px' },
      el('button', { class: 'btn', type: 'button', text: 'Wake', onclick: () => cmd('phone.wake', {}, 'Wake sent') }),
      el('button', { class: 'btn', type: 'button', text: 'Sleep & lock', onclick: () => cmd('phone.sleep', {}, 'Phone locked') }),
      el('button', { class: 'btn', type: 'button', text: 'Restart phone server', onclick: () => cmd('server.restart', {}, 'Phone server restarting') }),
      el('button', { class: 'btn', type: 'button', text: 'Reconnect USB', onclick: () => cmd('adb.reconnect', {}, 'Reconnecting ADB over USB') }),
      el('button', { class: 'btn danger', type: 'button', text: 'Reboot phone…', onclick: () => { confirmBox.hidden = false; } })), confirmBox));
  }
  if (id === 'files' && desk()) {
    add(group('Uploads', el('div', { class: 'row' }, lbl('Upload folder', 'Set on the computer ([files] upload_dir in desktop.ini)'), el('span', { class: 'val', text: uploadDir() }))));
  }
  if (id === 'files' && !desk()) {
    add(group('Uploads',
      textRow('destDir', 'Upload folder', 'Photos from the iPhone land here.'),
      toggleRow('mediaScan', 'Add uploads to Gallery', 'Runs a media scan so photos show up in Android albums'),
      toggleRow('overwrite', 'Overwrite existing files'),
      stepRow('maxMb', 'Max file size', 100, 4096, 100, ' MB')));
    const pathIn = el('input', { type: 'text', id: 'browsePath', autocomplete: 'off', autocapitalize: 'off', spellcheck: 'false' });
    pathIn.value = desk() ? ((st.ready && st.ready.home) || '~') : '/sdcard/Download';
    const list = el('ul', { class: 'files' });
    const browse = async () => {
      list.textContent = '';
      try {
        const r = await sendCtrl('file.list', { path: pathIn.value }, true);
        const entries = (r.entries || []).sort((a, b) => (b.dir - a.dir) || a.name.localeCompare(b.name));
        if (!entries.length) list.append(el('li', { class: 'note', text: 'Empty folder.' }));
        for (const e of entries) {
          const meta = el('div', { class: 'meta', text: e.dir ? 'folder' : fmtSize(e.size) });
          const full = pathIn.value.replace(/[\/\\]+$/, '') + (pathIn.value.includes('\\') && !pathIn.value.includes('/') ? '\\' : '/') + e.name;
          const btn = e.dir ? el('button', { class: 'btn', type: 'button', text: 'Open', onclick: () => { pathIn.value = full; browse(); } })
            : el('button', { class: 'btn', type: 'button', text: 'Download', onclick: () => pull(full, e.name, meta) });
          list.append(el('li', {}, el('div', { style: 'flex:1;min-width:0' }, el('div', { class: 'nm', text: e.name }), meta), btn));
        }
      } catch (e) { list.append(el('li', { class: 'note', text: e.msg || e.message })); }
    };
    add(group(`Browse the ${noun()}`, el('div', { class: 'row stack' },
      el('div', { style: 'display:flex;gap:8px' }, pathIn, el('button', { class: 'btn primary', type: 'button', text: 'List', onclick: browse })), list)));
    if (st.connected) browse();
  }
  if (id === 'logs') {
    const lc = el('pre', { class: 'log', 'aria-label': 'Logcat output' });
    const filt = el('input', { type: 'text', id: 'logcatFilter', placeholder: 'Tag filter, e.g. MyGame:V *:S', autocomplete: 'off', autocapitalize: 'off' });
    const lvl = el('div', { class: 'seg' });
    let minLvl = 'I';
    for (const L of ['V', 'D', 'I', 'W', 'E']) {
      const b = el('button', { type: 'button', text: L, 'aria-pressed': String(L === minLvl) });
      b.addEventListener('click', () => { minLvl = L; lvl.querySelectorAll('button').forEach(x => x.setAttribute('aria-pressed', String(x === b))); });
      lvl.append(b);
    }
    const startBtn = el('button', { class: 'btn primary', type: 'button', text: 'Start logcat' });
    startBtn.addEventListener('click', async () => {
      if (logcatStream) { stopLogcat(); startBtn.textContent = 'Start logcat'; return; }
      const spec = filt.value.trim() ? filt.value.trim().split(/\s+/) : [`*:${minLvl}`];
      try {
        const r = await sendCtrl('logcat.start', { filter: spec, format: 'threadtime', tail: 200, buffers: ['main', 'system', 'crash'] }, true);
        logcatStream = r.stream;
        startBtn.textContent = 'Stop';
        st.logStreams.set(r.stream, lines => {
          for (const l of lines) {
            const m = / ([VDIWEF]) /.exec(l);
            lc.append(el('span', { class: 'lv-' + (m ? m[1] : 'I'), text: l + '\n' }));
          }
          while (lc.childNodes.length > 400) lc.firstChild.remove();
          lc.scrollTop = lc.scrollHeight;
        });
      } catch (e) { toast(e.msg || e.message); }
    });
    if (!desk()) add(group('Logcat', el('div', { class: 'row stack' }, filt, el('div', { style: 'display:flex;gap:8px;align-items:center' }, el('div', { style: 'flex:1' }, lvl), startBtn), lc)));
    const shIn = el('input', { type: 'text', id: 'shellIn', placeholder: 'getprop ro.build.version.release', autocomplete: 'off', autocapitalize: 'off', spellcheck: 'false' });
    const shOut = el('pre', { class: 'log', style: 'height:150px', 'aria-label': 'Shell output' });
    const run = async () => {
      const c = shIn.value.trim() || shIn.placeholder;
      shOut.append(el('span', { class: 'lv-out', text: '$ ' + c + '\n' }));
      try {
        const r = await sendCtrl('shell.exec', { cmd: c, timeout_ms: 10000 }, true);
        shOut.append(el('span', { text: (r.stdout || '') + (r.stderr || '') }), el('span', { class: 'lv-D', text: `exit ${r.exit_code} · ${r.ms} ms\n\n` }));
      } catch (e) { shOut.append(el('span', { class: 'lv-E', text: (e.msg || e.message) + '\n\n' })); }
      shOut.scrollTop = shOut.scrollHeight; shIn.value = '';
    };
    shIn.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); run(); } });
    add(group(desk() ? 'Shell (only when the computer allows it: [desktop] allow_shell)' : 'Shell (runs on the phone as the shell user)', el('div', { class: 'row stack' },
      el('div', { style: 'display:flex;gap:8px' }, shIn, el('button', { class: 'btn primary', type: 'button', text: 'Run', onclick: run })),
      el('div', { class: 'btns' }, ...(desk() ? (st.device.platform === 'windows' ? ['ver', 'tasklist', 'ipconfig'] : ['uname -a', 'uptime', 'df -h']) : ['wm size', 'dumpsys battery', 'pm list packages -3']).map(c => el('button', { class: 'btn', type: 'button', text: c, onclick: () => { shIn.value = c; run(); } }))), shOut)));
    const pr = el('pre', { class: 'log', 'aria-label': 'Protocol messages' });
    renderProto = () => {
      pr.textContent = '';
      for (const p of proto.slice(-80)) pr.append(el('span', { class: p.dir === '→' ? 'lv-out' : (p.dir === '←' ? 'lv-in' : 'lv-D'), text: p.line + '\n' }));
      pr.scrollTop = pr.scrollHeight;
    };
    renderProto();
    add(group('Protocol messages (Appendix A)', el('div', { class: 'row stack' }, pr)));
  }
  if (id === 'check') {
    const box = el('div', { class: 'group' });
    add(el('p', { class: 'note', text: 'Real checks of this iPhone and browser.' }), box,
      el('button', { class: 'btn', type: 'button', text: 'Run checks again', onclick: () => runChecks(box) }));
    runChecks(box);
  }
  if (id === 'conn') {
    if (st.devices.length > 1) {
      add(group('Device', ...st.devices.map(dv => el('div', { class: 'row' }, lbl(dv.name, dv.online ? 'online' : 'offline'),
        el('button', { class: 'btn' + (st.device && dv.id === st.device.id ? ' primary' : ''), type: 'button', text: st.device && dv.id === st.device.id ? 'Selected' : 'Use',
          onclick: () => { S.device = dv.id; save(); endSession(); st.device = dv; wantSession = true; maybeStart(); renderTab('conn'); } })))));
    }
    add(group('Route',
      desk() ? null : segRow('route', 'Media route', [['auto', 'Auto'], ['tunnel', 'Via router'], ['wifi', 'Phone Wi-Fi']], 'Auto picks whichever measured faster.'),
      toggleRow('relay', 'Always relay through AWS', 'Forces TURN even when a direct path exists (next connection)')));
    add(group('Session', el('div', { class: 'card' }, kv([
      ['Server', location.host || '—'], ['Device', st.device ? st.device.id : '—'], ['Session', st.sid || '—'],
      ['State', st.connected ? 'connected' : 'not connected'],
    ]))));
    add(el('div', { class: 'btns' },
      el('button', { class: 'btn wide', type: 'button', text: st.sid ? 'Disconnect' : 'Connect', onclick: () => { closeSheet(); if (st.sid) endSession(); else { wantSession = true; maybeStart(); } } }),
      el('button', { class: 'btn danger wide', type: 'button', text: 'Log out', onclick: async () => { await api('POST', '/logout').catch(() => {}); location.href = '/login?next=' + encodeURIComponent(location.pathname + location.search); } })));
  }
  body.scrollTop = 0;
}
/* Settings → Computer: status and the settings in the computer's desktop.ini (agent.config). */
function renderComputerTab(add) {
  const card = el('div', { class: 'card' });
  statusRefresh = () => {
    const dd = st.device || {}, p = dd.phone || {}, ps = st.phoneStats || {}, sc = (st.ready && st.ready.screens) || p.screens || [];
    card.textContent = '';
    card.append(kv([
      ['Computer', `${p.model || dd.name || '—'}`], ['System', p.os || dd.platform || '—'],
      ['State', pill(p.server === 'running' ? 'ready' : (p.problem || 'not ready'), p.server === 'running' ? 'ok' : 'bad')],
      ['Screens', sc.length ? sc.map((x, i) => `${i + 1}: ${x.w}×${x.h}${x.primary ? ' (main)' : ''}`).join(', ') : '—'],
      ['Encoder', ps.encoder ? `${ps.encoder} · ${ps.width || 0}×${ps.height || 0}` : '—'],
      ['Agent', dd.online ? `online · ${(dd.agent && dd.agent.version) || ''} · ${(dd.agent && dd.agent.arch) || ''}` : 'offline'],
    ]));
  };
  statusRefresh();
  add(group('Status', card));
  const sc = (st.ready && st.ready.screens) || [];
  if (sc.length > 1) add(group('Screen', el('div', { class: 'btns', style: 'padding-top:10px' },
    ...sc.map((x, i) => el('button', { class: 'btn' + ((st.screen && st.screen.index) === i ? ' primary' : ''), type: 'button', text: `${i + 1}: ${x.w}×${x.h}`, onclick: () => selectScreen(i) })))));
  const box = el('div');
  add(box);
  const OPTS = [['desktop.view_only', 'View only', 'Mouse and keyboard from viewers are ignored'],
    ['desktop.confirm', 'Ask before each session', 'Someone at the computer must click Allow'],
    ['desktop.notify', 'Notify at the computer', 'When a viewer connects and leaves'],
    ['desktop.clipboard_sync', 'Clipboard sync'], ['desktop.lock_on_disconnect', 'Lock the computer after disconnect'],
    ['desktop.audio', 'Sound', 'Send what the computer plays to viewers']];
  (async () => {
    let cur = {};
    try { const r = await api('POST', `/devices/${encodeURIComponent(st.device.id)}/cmd`, { cmd: 'agent.config.get' }); cur = ((await r.json()).result) || {}; }
    catch (e) { box.append(el('p', { class: 'note', text: 'Settings unavailable: ' + e.message })); return; }
    const rows = OPTS.map(([k, label, help]) => {
      const cb = el('input', { type: 'checkbox', class: 'sw', 'aria-label': label });
      cb.checked = cur[k] === true;
      cb.addEventListener('change', () => cmd('agent.config', { [k]: cb.checked }, 'Saved to the computer'));
      return el('div', { class: 'row' }, lbl(label, help), cb);
    });
    const enc = el('div', { class: 'seg', role: 'group' });
    for (const [v, t] of [['auto', 'Auto'], ['hardware', 'Hardware'], ['software', 'Software']]) {
      const b = el('button', { type: 'button', 'aria-pressed': String((cur['video.encoder'] || 'auto') === v), text: t });
      b.addEventListener('click', () => { enc.querySelectorAll('button').forEach(x => x.setAttribute('aria-pressed', String(x === b))); cmd('agent.config', { 'video.encoder': v }, 'Saved · applies on the next connection'); });
      enc.append(b);
    }
    box.append(group('Settings', ...rows, el('div', { class: 'row stack' }, lbl('Video encoder', 'Auto uses the graphics card when it can'), enc)));
  })();
  add(group('Actions', el('div', { class: 'btns', style: 'padding-top:10px' },
    el('button', { class: 'btn', type: 'button', text: 'Wake the display', onclick: () => cmd('phone.wake', {}, 'Wake sent') }),
    el('button', { class: 'btn', type: 'button', text: 'Lock the computer', onclick: () => cmd('phone.sleep', {}, 'Computer locked') }))));
}
function deviceChanged() {
  if (desk() !== !!st.wasDesk) { st.wasDesk = desk(); renderQuickKeys(); video.style.cursor = desk() ? 'default' : ''; }
  if (!menu.hidden) syncMenuStates();
  if (openId === 'settings' && ['phone', 'conn'].includes(currentTab)) { if (statusRefresh) statusRefresh(); }
  tickStats();
}
function stopLogcat() {
  if (!logcatStream) return;
  sendCtrl('logcat.stop', { stream: logcatStream }).catch(() => {});
  st.logStreams.delete(logcatStream);
  logcatStream = 0;
}

/* ================= device check (§5.6) ================= */
async function runChecks(box) {
  box.textContent = '';
  const row = (label, detail, state, text) => box.append(el('div', { class: 'check' }, el('div', { class: 'lbl' }, label, detail ? el('small', { text: detail }) : null), pill(text, state)));
  const sc = window.isSecureContext;
  row('Page security', `${location.protocol}//… · secure context: ${sc}`, sc ? 'ok' : 'warn', sc ? 'HTTPS' : 'Plain HTTP');
  const standalone = navigator.standalone === true || matchMedia('(display-mode: standalone)').matches || matchMedia('(display-mode: fullscreen)').matches;
  row('Home Screen app', standalone ? 'Running full screen' : 'Share → Add to Home Screen for full screen', standalone ? 'ok' : '', standalone ? 'Yes' : 'No');
  row('Multi-touch', `maxTouchPoints = ${navigator.maxTouchPoints || 0}`, (navigator.maxTouchPoints || 0) >= 5 ? 'ok' : 'warn', (navigator.maxTouchPoints || 0) + ' points');
  const hasPC = typeof RTCPeerConnection === 'function';
  row('WebRTC', hasPC ? 'RTCPeerConnection present' : 'RTCPeerConnection missing', hasPC ? 'ok' : 'bad', hasPC ? 'Available' : 'Missing');
  let codecs = null;
  try { codecs = [...new Set(RTCRtpReceiver.getCapabilities('video').codecs.map(x => x.mimeType.replace('video/', '')))]; } catch (e) {}
  row('WebRTC video codecs', codecs ? codecs.join(', ') : 'Not reported', codecs ? (codecs.includes('H265') ? 'ok' : 'warn') : '', codecs ? (codecs.includes('H265') ? 'H.265 ✓' : 'No H.265') : 'Unknown');
  const mc = navigator.mediaCapabilities;
  async function dec(label, rtcType, fileType) {
    let r = null, how = '';
    if (mc && mc.decodingInfo) {
      try { r = await mc.decodingInfo({ type: 'webrtc', video: { contentType: rtcType, width: 1280, height: 720, bitrate: 8e6, framerate: 60 } }); how = 'webrtc'; } catch (e) {}
      if (!r) try { r = await mc.decodingInfo({ type: 'file', video: { contentType: fileType, width: 1280, height: 720, bitrate: 8e6, framerate: 60 } }); how = 'file'; } catch (e) {}
    }
    if (!r) { row(label, 'mediaCapabilities not available', '', 'Unknown'); return null; }
    const hw = !!(r.supported && r.powerEfficient);
    row(label, `${how}: supported=${r.supported} smooth=${r.smooth} powerEfficient=${r.powerEfficient}`, hw ? 'ok' : (r.supported ? 'warn' : 'bad'), hw ? 'Hardware' : (r.supported ? 'Software' : 'No'));
    return r.supported ? hw : false;
  }
  st.hw.h265 = await dec('H.265 decode (720p60)', 'video/H265', 'video/mp4; codecs="hvc1.1.6.L123.B0"');
  st.hw.h264 = await dec('H.264 decode (720p60)', 'video/H264', 'video/mp4; codecs="avc1.640028"');
  const clip = !!(navigator.clipboard && navigator.clipboard.writeText);
  row('Clipboard API', clip ? 'navigator.clipboard available' : 'Not on plain HTTP → one-tap copy and paste box', clip ? 'ok' : 'warn', clip ? 'Modern' : 'Fallback');
  row('Gamepad API', typeof navigator.getGamepads === 'function' ? 'Bluetooth controllers can be read' : 'Not available', typeof navigator.getGamepads === 'function' ? 'ok' : '', typeof navigator.getGamepads === 'function' ? 'Available' : 'No');
}

/* ================= wake lock (keep the iPhone awake while streaming) ================= */
let wakeLock = null;
async function keepAwake() {
  try { if ('wakeLock' in navigator && !wakeLock) { wakeLock = await navigator.wakeLock.request('screen'); wakeLock.addEventListener('release', () => { wakeLock = null; }); } } catch (_) {}
}
document.addEventListener('visibilitychange', () => { if (document.visibilityState === 'visible') keepAwake(); });

/* ================= boot ================= */
if (DEMO) {
  cv.hidden = false; video.hidden = true;
  const s = { '1080p': 1080, '720p': 720, '540p': 540, '360p': 360 }[S.preset] || 720;
  cv.width = s; cv.height = Math.round(s * 20 / 9 / 16) * 16;
  requestAnimationFrame(drawDemo);
  st.device = { id: 'router1', name: 'Demo phone', online: true, phone: { model: 'Pixel 8', android: '14', server: 'running', adb_state: 'authorized', battery: { level: 82, charging: true, temp_c: 31.5 } } };
  setTimeout(() => androidCopied('Room code: 7Q4-XK2'), 8000);
} else {
  connectWs();
  setOverlay('Connecting', 'Connecting to the server…');
}
layout();
restoreBall();
armIdle();
syncMenuStates();
tickStats();
keepAwake();
runChecks(document.createElement('div'));
})();
