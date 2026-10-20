/* RemoDroid panel (panel.html): three tabs at the bottom.
 *   #devices            every phone, tap one to control it (/?device=ID)
 *   #agents             the routers running the agent; #agents/ID edits one,
 *                       #agents/add[/connect|/phone] enrolls a new one
 *   #server             this server's settings, one item at a time (/api/v1/settings)
 * Plain JS, no build step; works the same on remodroid-server and the Worker. */
(() => {
'use strict';
const $ = s => document.querySelector(s);
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const main = $('#main'), top = $('#top');
/* Where install_agent.sh lives (the RemoDroid install site, wizard/ in the repository). */
const INSTALL = 'https://remodroid.pages.dev';
const RELEASES = 'https://github.com/yinyue123/RemoDroid/releases/latest';
const ICON = {
  phone: '<svg viewBox="0 0 24 24"><rect x="6.5" y="2.5" width="11" height="19" rx="2.2"/><path d="M10.5 18.5h3"/></svg>',
  router: '<svg viewBox="0 0 24 24"><rect x="3" y="12.5" width="18" height="7.5" rx="2"/><path d="M7 16.2h.01M10.5 16.2h.01"/><path d="M8.5 9a5 5 0 0 1 7 0M6 6.5a8.5 8.5 0 0 1 12 0"/></svg>',
  copy: '<svg viewBox="0 0 24 24"><rect x="8" y="8" width="12" height="12" rx="2.5"/><path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"/></svg>',
};

/* ---------------- helpers ---------------- */
async function api(method, path, body) {
  const r = await fetch(path, { method, headers: body ? { 'Content-Type': 'application/json' } : {}, body: body ? JSON.stringify(body) : undefined, credentials: 'same-origin' });
  if (r.status === 401) { location.href = '/login?next=/panel.html'; throw new Error('login required'); }
  let j = {};
  try { j = await r.json(); } catch (_) {}
  if (!r.ok) throw Object.assign(new Error(j.msg || 'error ' + r.status), { status: r.status, code: j.code, j });
  return j;
}
function toast(msg) {
  const t = document.createElement('div');
  t.className = 'toast'; t.textContent = msg;
  document.body.appendChild(t); setTimeout(() => t.remove(), 3000);
}
function setTop(title, opts = {}) {
  top.innerHTML = `${opts.back ? `<a class="back" href="${esc(opts.back)}">${esc(opts.backLabel || 'Back')}</a>` : ''}<h1>${esc(title)}</h1>${opts.action ? `<a class="act" href="${esc(opts.action[1])}">${esc(opts.action[0])}</a>` : ''}`;
  document.title = `${title} · RemoDroid`;
}
const item = (o) => `<${o.href ? `a href="${esc(o.href)}"` : o.act ? `button type="button" data-act="${esc(o.act)}"` : 'div'} class="item${o.cls ? ' ' + o.cls : ''}">
  ${o.icon ? `<span class="ic ${o.iconCls || ''}">${o.icon}</span>` : ''}
  ${o.html != null ? o.html : `<span class="txt"><b>${esc(o.title)}</b>${o.sub ? `<small>${esc(o.sub)}</small>` : ''}</span>`}
  ${o.pill ? `<span class="pill ${o.pillCls || ''}">${esc(o.pill)}</span>` : ''}
  ${o.val != null ? `<span class="val">${esc(o.val)}</span>` : ''}${o.right || ''}${o.href || o.act ? (o.noChev ? '' : '<span class="chev">›</span>') : ''}
  </${o.href ? 'a' : o.act ? 'button' : 'div'}>`;
const group = (title, rows, foot) => `${title ? `<h2 class="gtitle">${esc(title)}</h2>` : ''}<div class="group">${rows.join('')}</div>${foot ? `<p class="gfoot">${foot}</p>` : ''}`;
const ago = ms => { const s = Math.round((Date.now() - ms) / 1000); return s < 90 ? 'just now' : s < 5400 ? Math.round(s / 60) + ' min ago' : s < 129600 ? Math.round(s / 3600) + ' h ago' : Math.round(s / 86400) + ' days ago'; };
const dur = s => s < 3600 ? Math.round(s / 60) + ' min' : s < 172800 ? Math.round(s / 3600) + ' h' : Math.round(s / 86400) + ' days';

/* An edit sheet for one setting: fields [{k, label, type, value, hint, placeholder, min, max, options}] */
function sheet({ title, fields, note, save, saveLabel }) {
  closeSheet();
  const scrim = document.createElement('div'); scrim.className = 'scrim';
  const s = document.createElement('section'); s.className = 'psheet'; s.setAttribute('role', 'dialog'); s.setAttribute('aria-label', title);
  s.innerHTML = `<header><button class="cancel" type="button">Cancel</button><h2>${esc(title)}</h2><button class="save" type="button">${esc(saveLabel || 'Save')}</button></header>
    <div class="form">${fields.map(f => `<div class="field"><label for="sf-${f.k}">${esc(f.label)}</label>${f.type === 'select'
      ? `<select id="sf-${f.k}">${f.options.map(o => `<option value="${esc(o[0])}" ${String(f.value) === String(o[0]) ? 'selected' : ''}>${esc(o[1])}</option>`).join('')}</select>`
      : `<input id="sf-${f.k}" type="${f.type || 'text'}" value="${esc(f.value ?? '')}" placeholder="${esc(f.placeholder || '')}" ${f.type === 'number' ? `inputmode="numeric" min="${f.min ?? 1}" max="${f.max ?? 65535}"` : ''} autocomplete="${f.type === 'password' ? (f.k === 'current_password' ? 'current-password' : 'new-password') : 'off'}" autocapitalize="off" spellcheck="false">`}
      ${f.hint ? `<div class="hint">${f.hint}</div>` : ''}</div>`).join('')}
      ${note ? `<p class="hint">${note}</p>` : ''}<p class="callout bad" id="sheetErr" hidden></p></div>`;
  document.body.append(scrim, s);
  const close = () => closeSheet();
  scrim.onclick = close;
  s.querySelector('.cancel').onclick = close;
  const btn = s.querySelector('.save');
  const go = async () => {
    const v = {};
    for (const f of fields) {
      const el = s.querySelector('#sf-' + f.k);
      v[f.k] = f.type === 'number' ? Number(el.value) : el.value.trim();
    }
    btn.disabled = true;
    try { await save(v); close(); }
    catch (e) { const err = s.querySelector('#sheetErr'); err.textContent = e.message; err.hidden = false; btn.disabled = false; }
  };
  btn.onclick = go;
  s.querySelectorAll('input').forEach(i => i.addEventListener('keydown', e => { if (e.key === 'Enter') go(); }));
  const first = s.querySelector('input,select');
  if (first) setTimeout(() => { first.focus(); if (first.select && first.type !== 'number') first.select(); }, 60);
  document.addEventListener('keydown', escClose);
}
function escClose(e) { if (e.key === 'Escape') closeSheet(); }
function closeSheet() {
  document.querySelectorAll('.scrim,.psheet').forEach(e => e.remove());
  document.removeEventListener('keydown', escClose);
}

/* ---------------- data ---------------- */
let devices = [], loadedDevices = false;
async function loadDevices() {
  try { devices = (await api('GET', '/api/v1/devices')).devices || []; loadedDevices = true; }
  catch (e) { if (e.message !== 'login required') toast(e.message); }
  const waiting = devices.filter(d => d.online && d.phone && d.phone.attached && d.phone.server === 'running').length;
  $('#devBadge').dataset.n = '';
  return waiting;
}
const PHONE_STATE = d => {
  const p = d.phone || {};
  if (!d.online) return ['off', 'agent offline'];
  if (!p.attached) return p.problem ? ['off', p.problem] : ['wait', 'no phone on USB'];
  if (p.adb_state === 'unauthorized') return ['wait', 'tap “Allow USB debugging” on the phone'];
  if (p.server !== 'running') return ['wait', 'phone server ' + (p.server || 'starting')];
  return ['on', 'ready'];
};

/* ---------------- Devices ---------------- */
function renderDevices() {
  setTop('Devices', { action: devices.length ? ['Add', '#agents/add'] : null });
  if (!loadedDevices) { main.innerHTML = '<p class="note">Loading…</p>'; return; }
  if (!devices.length) {
    main.innerHTML = `<div class="empty-state">${ICON.phone}<h2>No devices yet</h2><p>A device is an Android phone plugged into a router that runs the RemoDroid agent.</p>
      <a class="btn" href="#agents/add">Add an agent</a></div>`;
    return;
  }
  main.innerHTML = group('', devices.map(d => {
    const p = d.phone || {}, [cls, state] = PHONE_STATE(d);
    const name = p.model || d.name;
    const bits = p.model ? [d.name] : [];
    if (p.android) bits.push('Android ' + p.android);
    if (cls !== 'on') bits.push(state);
    const inUse = d.session && d.session.active;
    const batt = p.battery && p.battery.level >= 0 ? `${p.battery.level}%${p.battery.charging ? ' ⚡' : ''}` : null;
    return item({ href: '/?device=' + encodeURIComponent(d.id), icon: ICON.phone, iconCls: cls, title: name, sub: bits.join(' · '),
      pill: inUse ? 'in use' : batt, pillCls: inUse ? 'accent' : '' });
  }), 'Tap a phone to control it. “In use” means another browser is connected; you can take it over.');
}

/* ---------------- Agents ---------------- */
function renderAgents() {
  setTop('Agents', { action: ['Add', '#agents/add'] });
  if (!loadedDevices) { main.innerHTML = '<p class="note">Loading…</p>'; return; }
  if (!devices.length) {
    main.innerHTML = `<div class="empty-state">${ICON.router}<h2>No agents yet</h2><p>The agent runs on the router your phone is plugged into and connects out to this panel. No open ports at home.</p>
      <a class="btn" href="#agents/add">Add an agent</a></div>`;
    return;
  }
  main.innerHTML = group('', devices.map(d => {
    const a = d.agent || {};
    const sub = d.online ? [d.id, a.version && 'agent ' + a.version, a.arch].filter(Boolean).join(' · ')
      : `offline${d.last_seen ? ' · seen ' + ago(d.last_seen) : ''}`;
    return item({ href: '#agents/' + encodeURIComponent(d.id), icon: ICON.router, iconCls: d.online ? 'on' : 'off', title: d.name, sub });
  })) + `<h2 class="gtitle"></h2><div class="group">${item({ href: '#agents/add', title: '', html: '<span class="txt"><b style="color:var(--accent)">Add an agent</b><small>Install it on a router and enroll it with a one-time code</small></span>' })}</div>`;
}

/* Router settings: agent.ini through the panel (agent.config.get / agent.config). */
const SETTINGS = [
  ['Video', [
    ['codec', 'Codec', 'select', [['auto', 'Auto'], ['h265', 'H.265'], ['h264', 'H.264']]],
    ['preset', 'Resolution', 'select', [['1080p', '1080p'], ['720p', '720p'], ['540p', '540p'], ['360p', '360p']]],
    ['fps', 'Max frame rate', 'number', null, 'fps'],
    ['kbps', 'Max bitrate', 'number', null, 'kbit/s'],
    ['mode', 'Bitrate', 'select', [['auto', 'Adapts'], ['manual', 'Fixed']]],
  ]],
  ['Sound', [
    ['audio', 'Play the phone\'s sound on the iPhone', 'bool', null, null, 'The phone\'s speaker goes quiet while you\'re connected, like scrcpy. Android 12 or newer.'],
  ]],
  ['Phone', [
    ['screenOff', 'Screen off while connected', 'bool'],
    ['lockOnDisc', 'Lock after disconnecting', 'bool'],
    ['grace', 'Lock after', 'number', null, 's'],
    ['rotation', 'Rotation', 'select', [['auto', 'Auto'], ['portrait', 'Portrait'], ['landscape', 'Landscape']]],
    ['overheat', 'Overheat warning', 'number', null, '°C'],
  ]],
  ['Clipboard & files', [
    ['clipSync', 'Sync the clipboard', 'bool'],
    ['autoPaste', 'Paste after sending text', 'bool'],
    ['destDir', 'Upload folder', 'text'],
    ['mediaScan', 'Add uploads to the Gallery', 'bool'],
    ['maxMb', 'Largest file', 'number', null, 'MB'],
  ]],
  ['Network', [
    ['route', 'Media path', 'select', [['auto', 'Auto'], ['tunnel', 'USB tunnel'], ['wifi', 'Phone Wi-Fi']]],
    ['relay', 'Always relay through TURN', 'bool'],
  ]],
];
let agentCfg = {}, agentCfgFor = null;
const cmd = (id, c, args) => api('POST', `/api/v1/devices/${encodeURIComponent(id)}/cmd`, { cmd: c, args: args || {} });

async function renderAgent(id) {
  const d = devices.find(x => x.id === id);
  setTop(d ? d.name : id, { back: '#agents', backLabel: 'Agents' });
  if (!d) { main.innerHTML = loadedDevices ? '<div class="callout bad">This agent was removed.</div>' : '<p class="note">Loading…</p>'; return; }
  const a = d.agent || {}, p = d.phone || {}, [cls, state] = PHONE_STATE(d);
  const status = group('', [
    item({ title: 'Status', html: '<span class="txt"><b>Status</b></span>', val: d.online ? `online${a.uptime_s ? ' · ' + dur(a.uptime_s) : ''}` : `offline${d.last_seen ? ' · ' + ago(d.last_seen) : ''}` }),
    item({ html: '<span class="txt"><b>Phone</b></span>', val: p.model ? `${p.model}${cls === 'on' ? '' : ' · ' + state}` : state }),
    a.version ? item({ html: '<span class="txt"><b>Agent</b></span>', val: `${a.version} · ${a.arch || ''}` }) : '',
    a.public_ip ? item({ html: '<span class="txt"><b>Address</b></span>', val: a.public_ip }) : '',
    item({ html: '<span class="txt"><b>ID</b></span>', val: d.id }),
  ].filter(Boolean));
  const name = group('', [item({ act: 'rename', html: '<span class="txt"><b>Name</b></span>', val: d.name })]);
  main.innerHTML = status + name + notifyGroup(d) + '<div id="cfg"></div>' +
    group('Actions', [
      item({ act: 'open', html: '<span class="txt"><b style="color:var(--accent)">Open the phone</b></span>' }),
      d.online ? item({ act: 'restart', html: '<span class="txt"><b>Restart the phone server</b><small>When the picture or input is stuck</small></span>', noChev: true }) : '',
      d.online ? item({ act: 'usb', html: '<span class="txt"><b>Reconnect USB</b></span>', noChev: true }) : '',
      d.online ? item({ act: 'wake', html: '<span class="txt"><b>Wake the phone</b></span>', noChev: true }) : '',
      d.online ? item({ act: 'lock', html: '<span class="txt"><b>Lock the phone</b></span>', noChev: true }) : '',
    ].filter(Boolean)) +
    `<h2 class="gtitle"></h2><div class="group"><button class="item danger" type="button" data-act="remove">Remove this agent</button></div>
    <p class="gfoot">Removing disconnects the agent; it must enroll again with a new code to come back.</p>`;
  main.querySelectorAll('[data-act]').forEach(b => b.onclick = () => agentAction(d, b.dataset.act));
  const bark = $('#barkOn');
  if (bark) bark.onchange = async () => {
    try { await api('PATCH', `/api/v1/devices/${encodeURIComponent(id)}`, { bark: bark.checked }); toast(bark.checked ? 'Bark push on' : 'Bark push paused'); await loadDevices(); }
    catch (e) { bark.checked = !bark.checked; toast(e.message); }
  };
  const box = $('#cfg');
  if (!d.online) { box.innerHTML = group('Settings', [item({ html: '<span class="txt"><small>The agent is offline. Its settings can be changed once it reconnects.</small></span>' })]); return; }
  if (agentCfgFor !== id) {
    box.innerHTML = group('Settings', [item({ html: '<span class="txt"><small>Reading the agent\'s settings…</small></span>' })]);
    try { agentCfg = (await cmd(id, 'agent.config.get')).result || {}; agentCfgFor = id; }
    catch (e) { box.innerHTML = group('Settings', [item({ html: `<span class="txt"><small>${esc(e.message)}</small></span>` })]); return; }
    if (route().id !== id) return;
  }
  renderAgentSettings(d, box);
}
/* Phone notifications pushed to Bark (an iPhone app) by remodroid-server; kept in server.ini, so it
 * works while the agent is offline. The Worker has no Bark push and sends no "notify". */
function notifyGroup(d) {
  const n = d.notify;
  if (!n) return '';
  const val = n.bark_url || (n.bark_default ? 'Server default' : 'Not set');
  return group('Notifications', [
    item({ act: 'barkurl', html: '<span class="txt"><b>Bark URL</b></span>', val }),
    typeof d.bark === 'boolean' ? item({ html: '<span class="txt"><b>Push to Bark</b></span>', right: `<input class="sw" type="checkbox" id="barkOn" aria-label="Push to Bark" ${d.bark ? 'checked' : ''}>` }) : '',
  ].filter(Boolean), 'New phone notifications are sent to the <a href="https://github.com/Finb/Bark" target="_blank" rel="noopener">Bark</a> app on your iPhone. The phone also needs notification forwarding on ([notify] enabled in agent.ini).');
}
function renderAgentSettings(d, box) {
  const cur = agentCfg;
  box.innerHTML = SETTINGS.map(([g, items]) => {
    const help = items.find(i => i[5]);
    return group(g, items.map(([k, label, type, opts, unit]) => {
      const v = cur[k];
      if (type === 'bool') return item({ html: `<span class="txt"><b>${esc(label)}</b></span>`, right: `<input class="sw" type="checkbox" data-k="${k}" aria-label="${esc(label)}" ${v === true ? 'checked' : ''}>` });
      if (type === 'select') return item({ html: `<span class="txt"><b>${esc(label)}</b></span>`, right: `<select data-k="${k}" aria-label="${esc(label)}">${opts.map(o => `<option value="${o[0]}" ${String(v) === o[0] ? 'selected' : ''}>${esc(o[1])}</option>`).join('')}</select>` });
      return item({ act: 'edit:' + k, html: `<span class="txt"><b>${esc(label)}</b></span>`, val: v == null || v === '' ? '—' : `${v}${unit ? ' ' + unit : ''}` });
    }), help ? esc(help[5]) : '');
  }).join('');
  const saveOne = async (k, v, el) => {
    try { await cmd(d.id, 'agent.config', { [k]: v }); cur[k] = v; toast('Saved to ' + d.name); }
    catch (e) { toast(e.message); if (el && el.type === 'checkbox') el.checked = !v; renderAgentSettings(d, box); }
  };
  box.querySelectorAll('input.sw').forEach(el => el.onchange = () => saveOne(el.dataset.k, el.checked, el));
  box.querySelectorAll('select[data-k]').forEach(el => el.onchange = () => saveOne(el.dataset.k, el.value, el));
  box.querySelectorAll('[data-act^="edit:"]').forEach(b => b.onclick = () => {
    const k = b.dataset.act.slice(5);
    const [, label, type, , unit] = SETTINGS.flatMap(g => g[1]).find(x => x[0] === k);
    sheet({ title: label, fields: [{ k, label: unit ? `${label} (${unit})` : label, type: type === 'number' ? 'number' : 'text', value: cur[k], min: 0, max: 1000000 }],
      save: async v => { await cmd(d.id, 'agent.config', { [k]: v[k] }); cur[k] = v[k]; toast('Saved to ' + d.name); renderAgentSettings(d, box); } });
  });
}
async function agentAction(d, what) {
  const id = d.id;
  try {
    if (what === 'open') { location.href = '/?device=' + encodeURIComponent(id); return; }
    if (what === 'rename') {
      sheet({ title: 'Name', fields: [{ k: 'name', label: 'Name', value: d.name, hint: 'How this agent and its phone show up in the lists.' }],
        save: async v => { if (!v.name) throw new Error('Enter a name'); await api('PATCH', `/api/v1/devices/${encodeURIComponent(id)}`, { name: v.name }); await loadDevices(); show(); } });
      return;
    }
    if (what === 'barkurl') {
      const n = d.notify || {};
      sheet({ title: 'Bark URL', fields: [{ k: 'bark_url', label: 'Bark URL', type: 'url', value: '', placeholder: n.bark_url || 'https://api.day.app/your-key',
        hint: `Copy it from the Bark app on your iPhone. ${n.bark_url ? 'Saving it empty removes this router\'s URL' : 'Leave it empty'}${n.bark_default ? ' to use the server\'s default ([notify] bark_url).' : '; nothing is pushed without one.'} The key stays on the server and is never shown again in full.` }],
        save: async v => { await api('PATCH', `/api/v1/devices/${encodeURIComponent(id)}`, { bark_url: v.bark_url.trim() }); toast(v.bark_url.trim() ? 'Bark URL saved' : 'Bark URL removed'); await loadDevices(); show(); } });
      return;
    }
    if (what === 'restart') { await cmd(id, 'server.restart'); toast('Phone server restarting'); }
    if (what === 'usb') { await cmd(id, 'adb.reconnect'); toast('Reconnecting USB'); }
    if (what === 'wake') { await cmd(id, 'phone.wake'); toast('Phone woken'); }
    if (what === 'lock') { await cmd(id, 'phone.sleep'); toast('Phone locked'); }
    if (what === 'remove') {
      if (!confirm(`Remove “${d.name}”? Its agent is disconnected and must enroll again to come back.`)) return;
      await fetch(`/api/v1/devices/${encodeURIComponent(id)}`, { method: 'DELETE', credentials: 'same-origin' });
      await loadDevices(); location.hash = '#agents';
    }
  } catch (e) { toast(e.message); }
}

/* ---------------- Agents → add: install → connect → phone ---------------- */
const ADD = [{ id: 'install', label: 'Agent', sub: 'Install' }, { id: 'connect', label: 'Connect', sub: 'Enroll' }, { id: 'phone', label: 'Phone', sub: 'USB' }];
const OS = {
  openwrt: { name: 'OpenWrt', cmd: `wget -qO- ${INSTALL}/install_agent.sh | sh`, pkg: 'ipk', install: 'opkg install ./remodroid-agent_*.ipk', note: 'Run it in an SSH session on the router. Pulls in ca-bundle for HTTPS.' },
  debian: { name: 'Debian · Ubuntu', cmd: `curl -fsSL ${INSTALL}/install_agent.sh | sudo sh`, pkg: 'deb', install: 'sudo apt install ./remodroid-agent_*.deb', note: 'Raspberry Pi OS and Armbian too.' },
  rhel: { name: 'CentOS · Fedora', cmd: `curl -fsSL ${INSTALL}/install_agent.sh | sudo sh`, pkg: 'rpm', install: 'sudo dnf install ./remodroid-agent-*.rpm', note: 'Also Rocky, Alma and Amazon Linux.' },
  alpine: { name: 'Alpine', cmd: `wget -qO- ${INSTALL}/install_agent.sh | sh`, pkg: 'apk', install: 'apk add --allow-untrusted ./remodroid-agent_*.apk', note: 'Run as root.' },
};
let os = 'openwrt', code = null, codeTimer = null, newId = null;
const hl = s => esc(s).replace(/(--[a-z-]+)/g, '<span class="f">$1</span>');
const cmdBox = (text, html) => `<div class="cmd"><code>${html || esc(text)}</code><button type="button" aria-label="Copy" data-copy="${esc(text)}">${ICON.copy}</button></div>`;
document.addEventListener('click', async e => {
  const b = e.target.closest('[data-copy]');
  if (!b) return;
  try { await navigator.clipboard.writeText(b.dataset.copy); } catch (_) {
    const ta = document.createElement('textarea'); ta.value = b.dataset.copy; document.body.appendChild(ta); ta.select();
    try { document.execCommand('copy'); } catch (__) {} ta.remove();
  }
  b.classList.add('ok'); b.innerHTML = '✓'; setTimeout(() => { b.classList.remove('ok'); b.innerHTML = ICON.copy; }, 1400);
});

function renderAdd(stepId) {
  const step = Math.max(0, ADD.findIndex(s => s.id === stepId));
  setTop('Add an agent', { back: '#agents', backLabel: 'Agents' });
  const steps = `<nav class="steps" aria-label="Add an agent">${ADD.map((s, i) => `${i ? `<span class="bar${i <= step ? ' done' : ''}"></span>` : ''}<a class="st${i === step ? ' on' : i < step ? ' done' : ''}" href="#agents/add/${s.id}" style="text-decoration:none"><span class="n">${i < step ? '✓' : i + 1}</span><span><b>${s.label}</b><small>${s.sub}</small></span></a>`).join('')}</nav>`;
  const foot = (next, label) => `<div class="flow-foot">${step ? `<a class="btn ghost" href="#agents/add/${ADD[step - 1].id}">← Back</a>` : ''}<span class="spacer"></span>${next ? `<a class="btn" href="${next}">${label || 'Next →'}</a>` : ''}</div>`;
  if (step === 0) {
    const o = OS[os];
    main.innerHTML = `${steps}<div class="flow"><p class="eyebrow">01 — agent</p><h1>Install the <em>agent</em></h1>
      <p class="lede">It runs on the router the phone is plugged into and dials out to this panel. No open port, no public address.</p>
      <div class="seg" role="tablist">${Object.entries(OS).map(([k, v]) => `<button type="button" role="tab" data-os="${k}" aria-selected="${k === os}">${v.name}</button>`).join('')}</div>
      <div class="num"><i>1</i>One command</div>${cmdBox(o.cmd, hl(o.cmd))}<p class="note">${esc(o.note)}</p>
      <div class="num"><i class="alt">2</i>Or download the package</div>
      <p>From the <a href="${RELEASES}" target="_blank" rel="noopener">latest release</a>, take the <code>remodroid-agent</code> <b>.${esc(o.pkg)}</b> for your CPU${o.pkg === 'ipk' ? ' (the architecture <code>opkg print-architecture</code> lists)' : ''}, then install it with <code>${esc(o.install)}</code>.</p>
      ${foot('#agents/add/connect', 'I\'ve installed it →')}</div>`;
    main.querySelectorAll('[data-os]').forEach(b => b.onclick = () => { os = b.dataset.os; renderAdd('install'); });
  } else if (step === 1) {
    renderConnect(steps, foot);
  } else {
    renderPhone(steps, foot);
  }
}
async function newCode() {
  clearInterval(codeTimer);
  code = await api('POST', '/api/v1/enroll/codes', {});
  code.until = Date.now() + code.expires_in * 1000;
  code.state = 'pending';
  codeTimer = setInterval(tickCode, 1000);
}
function tickCode() {
  const el = $('#expires');
  if (!code || !el) return;
  const s = Math.max(0, Math.round((code.until - Date.now()) / 1000));
  el.textContent = `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
  if (!s && code.state === 'pending') { clearInterval(codeTimer); code.state = 'expired'; show(); }
}
async function renderConnect(steps, foot) {
  if (!code || (code.state === 'pending' && Date.now() > code.until)) {
    main.innerHTML = steps + '<p class="note">Making a code…</p>';
    try { await newCode(); } catch (e) { main.innerHTML = steps + `<div class="callout bad">${esc(e.message)}</div>`; return; }
    if (route().sub !== 'connect') return;
  }
  const panel = location.origin;
  const enroll = `remodroid enroll --panel ${panel} --code ${code.code}`;
  const both = `${OS[os].cmd.replace(/\| (sudo )?sh$/, '| $1sh -s --')} --panel ${panel} --code ${code.code}`;
  const expired = code.state === 'expired';
  main.innerHTML = `${steps}<div class="flow"><p class="eyebrow">02 — connect</p><h1>Paste this into <em>the router</em></h1>
    ${expired ? '<div class="callout warn">That code expired. <button class="link-btn" type="button" id="again">Make a new one</button></div>' : ''}
    ${cmdBox(enroll, hl(enroll))}
    <div class="term" id="term"><div><span class="p">$</span> ${esc(enroll)}</div>${code.state !== 'pending' && !expired ? `<div class="d">▸ enrolled as ${esc(newId)}</div>` : ''}</div>
    <p class="status" id="state">${code.state === 'online' ? `<b>${esc(newId)}</b> is online` : 'waiting for the agent'}</p>
    <p class="small-print">code <b>${esc(code.code)}</b> · expires in <b id="expires">…</b> · single use</p>
    <details style="margin-top:22px"><summary class="note">Agent not installed yet? Install and enroll in one go</summary>${cmdBox(both, hl(both))}</details>
    ${foot(code.state === 'online' ? '#agents/add/phone' : null)}</div>`;
  tickCode();
  if (expired) $('#again').onclick = () => { code = null; renderAdd('connect'); };
}
async function pollCode() {
  if (!code || code.state === 'expired' || code.state === 'online' || route().sub !== 'connect') return;
  let s;
  try { s = await api('GET', `/api/v1/enroll/codes/${code.code}`); } catch (_) { return; }
  const term = $('#term'), state = $('#state');
  if (s.state === 'used' && code.state === 'pending') {
    code.state = 'used'; newId = s.device;
    if (term) term.innerHTML += `<div class="d">▸ enrolled as ${esc(s.device)}</div>`;
    if (state) state.textContent = 'connecting…';
  }
  if (s.state === 'used' && s.online) {
    code.state = 'online'; clearInterval(codeTimer);
    if (term) term.innerHTML += '<div class="k">✓ bound — this agent is online</div>';
    await loadDevices();
    setTimeout(() => { if (route().sub === 'connect') location.hash = '#agents/add/phone'; }, 1200);
  }
}
function renderPhone(steps, foot) {
  const d = devices.find(x => x.id === newId) || devices.find(x => x.online) || null;
  const [cls, txt] = d ? PHONE_STATE(d) : ['wait', 'waiting for an agent'];
  const ready = cls === 'on';
  main.innerHTML = `${steps}<div class="flow"><p class="eyebrow">03 — phone</p><h1>Plug in <em>the phone</em></h1>
    <p class="lede">Once per phone: turn on USB debugging, connect it to the router and allow the router's key.</p>
    <div class="num"><i>1</i>Turn on USB debugging</div>
    <p class="note">Settings → About phone → tap <b>Build number</b> seven times. Then Settings → System → <b>Developer options</b> → <b>USB debugging</b>.</p>
    <div class="num"><i>2</i>Connect it to the router's USB port</div>
    <p class="note">Use a data cable. A powered hub helps if the router's port is weak.</p>
    <div class="num"><i>3</i>Allow the router</div>
    <p class="note">The phone asks <b>“Allow USB debugging?”</b>. Tick <b>Always allow from this computer</b>, then <b>Allow</b>.</p>
    <div class="callout ${ready ? 'ok' : ''}" style="margin-top:22px"><span class="dot ${cls}"></span>${d ? `<b>${esc(d.name)}</b>: ` : ''}${esc(ready ? 'the phone is ready' : txt)}</div>
    ${foot(ready ? '/?device=' + encodeURIComponent(d.id) : null, 'Open the phone →')}</div>`;
}

/* ---------------- Server ---------------- */
let cfg = null;
async function loadSettings() {
  try { cfg = await api('GET', '/api/v1/settings'); } catch (e) { cfg = { error: e.message }; }
}
async function patch(body) {
  const r = await api('PATCH', '/api/v1/settings', body);
  if (r.relogin) { toast('Log in with the new name'); setTimeout(() => { location.href = '/login?next=/panel.html'; }, 900); return r; }
  if (r.restart_ms) await waitRestart(r, body);
  else cfg = r;
  toast('Saved');
  if (route().tab === 'server') renderServer();
  return r;
}
/* Ports, domain or TURN changed: the server re-execs itself. If the page's own port moved, follow it. */
async function waitRestart(r, body) {
  const b = document.createElement('div');
  b.className = 'busy';
  b.innerHTML = '<div><div class="spin"></div><b>Restarting the server…</b><p class="note">New ports, domain or TURN settings take effect.</p></div>';
  document.body.append(b);
  const port = location.port || (location.protocol === 'https:' ? '443' : '80');
  if (location.protocol === 'http:' && body.http_port && String(body.http_port) !== port) {
    const to = `http://${location.hostname}:${body.http_port}/panel.html#server`;
    b.querySelector('div').innerHTML = `<b>The panel moved</b><p class="note">It now answers on port ${esc(body.http_port)}.</p><a class="btn" href="${esc(to)}">Open ${esc(location.hostname)}:${esc(body.http_port)}</a>`;
    return;
  }
  await new Promise(res => setTimeout(res, r.restart_ms + 700));
  for (let i = 0; i < 20; i++) {
    try { cfg = await api('GET', '/api/v1/settings'); break; } catch (_) { await new Promise(res => setTimeout(res, 800)); }
  }
  b.remove();
}

function accountRows() {
  return [
    item({ act: 'username', html: '<span class="txt"><b>Username</b></span>', val: cfg.username }),
    item({ act: 'password', html: '<span class="txt"><b>Password</b></span>', val: '••••••••' }),
  ];
}
function renderServer() {
  setTop('Server');
  if (!cfg) { main.innerHTML = '<p class="note">Loading…</p>'; loadSettings().then(() => { if (route().tab === 'server') renderServer(); }); return; }
  if (cfg.error) { main.innerHTML = `<div class="callout bad">${esc(cfg.error)}</div>`; return; }
  const worker = cfg.platform === 'worker';
  const builtinOn = (cfg.builtin_ice || []).length > 0;
  let html = '';
  if (!worker) {
    const dom = cfg.domain;
    html += group('Web address', [
      item({ act: 'domain', html: `<span class="txt"><b>Domain</b>${dom ? '' : '<small>Add one for automatic HTTPS (Let\'s Encrypt)</small>'}</span>`, val: dom || 'none' }),
      dom ? item({ html: '<span class="txt"><b>Certificate</b></span>', val: cfg.https ? 'active' : 'requesting…' }) : '',
      dom ? item({ act: 'tls_port', html: '<span class="txt"><b>HTTPS port</b></span>', val: cfg.tls_port }) : '',
      dom ? item({ html: '<span class="txt"><b>Send http:// to https://</b></span>', right: `<input class="sw" type="checkbox" data-k="redirect_http" aria-label="Redirect HTTP to HTTPS" ${cfg.redirect_http ? 'checked' : ''}>` }) : '',
      item({ act: 'http_port', html: '<span class="txt"><b>Web port</b><small>Plain HTTP; Let\'s Encrypt needs 80</small></span>', val: cfg.http_port }),
    ].filter(Boolean), dom ? `Open TCP ${esc(cfg.http_port)} and ${esc(cfg.tls_port)} in the firewall.` : '');
  }
  html += group('Account', accountRows(), 'Changing the name or password asks for the current password.');
  if (!worker) {
    html += group('STUN / TURN on this server', [
      item({ html: '<span class="txt"><b>Built-in STUN/TURN</b><small>Relays the video when a network blocks a direct path</small></span>', right: `<input class="sw" type="checkbox" data-k="turn_enabled" aria-label="Built-in STUN/TURN" ${cfg.turn_enabled ? 'checked' : ''}>` }),
      cfg.turn_enabled ? item({ act: 'turn_port', html: '<span class="txt"><b>Port (UDP)</b></span>', val: cfg.turn_port }) : '',
      cfg.turn_enabled ? item({ act: 'public_ip', html: '<span class="txt"><b>Public IPv4</b></span>', val: cfg.public_ip || 'not set' }) : '',
    ].filter(Boolean), cfg.turn_enabled ? `Open UDP ${esc(cfg.turn_port)} and UDP 49152–49252 (relay ports) in the firewall.` : '');
  } else {
    const t = cfg.turn || {};
    html += group('Cloudflare TURN', [
      item({ html: '<span class="txt"><b>Use Cloudflare TURN</b><small>1,000 GB a month free</small></span>', right: `<input class="sw" type="checkbox" data-k="cf" aria-label="Cloudflare TURN" ${t.cloudflare ? 'checked' : ''}>` }),
      t.cloudflare ? item({ act: 'cf_key', html: '<span class="txt"><b>TURN key ID</b></span>', val: t.key_id || 'not set' }) : '',
      t.cloudflare ? item({ act: 'cf_token', html: '<span class="txt"><b>API token</b></span>', val: t.has_token ? 'saved' : 'not set' }) : '',
    ].filter(Boolean), 'Cloudflare dashboard → Realtime → TURN Server → Create.');
  }
  html += `<h2 class="gtitle">${builtinOn ? 'STUN / TURN servers' : 'Other STUN / TURN servers'}</h2><div id="ice"></div><p class="gfoot">Phones use UDP TURN; add your own coturn here, or a public STUN server such as stun.cloudflare.com.</p>`;
  html += group('About', [
    item({ html: '<span class="txt"><b>Version</b></span>', val: cfg.version || '' }),
    !worker && cfg.config_file ? item({ html: '<span class="txt"><b>Config file</b></span>', val: cfg.config_file }) : '',
  ].filter(Boolean));
  html += `<h2 class="gtitle"></h2><div class="group"><button class="item danger" type="button" data-act="logout">Log out</button></div>`;
  main.innerHTML = html;

  /* this server's own STUN/TURN heads the list; removing both turns it off */
  const builtin = cfg.builtin_ice || [];
  IceList.mount($('#ice'), builtin.map(url => ({ url, builtin: true })).concat(cfg.ice_servers || []), {
    empty: 'None. Add one with just its address.',
    toast,
    onChange: async list => {
      const mine = list.filter(e => e.builtin).map(e => e.url), body = {};
      body.ice_servers = list.filter(e => !e.builtin).map(e => ({ url: e.url, username: e.username || '', credential: e.credential || '', was: e.was || e.url }));
      if (mine.length !== builtin.length) body.builtin_ice = mine;
      await patch(body); /* re-renders the list from the saved settings */
    },
  });
  main.querySelectorAll('input.sw').forEach(el => el.onchange = async () => {
    const k = el.dataset.k;
    try {
      if (k === 'turn_enabled' && el.checked && !cfg.public_ip) { el.checked = false; editServer('turn_on'); return; }
      if (k === 'cf') {
        if (el.checked && !(cfg.turn && cfg.turn.has_token)) { el.checked = false; editServer('cf_key'); return; }
        await patch({ turn: el.checked ? { cloudflare: true } : { cloudflare: false } });
        return;
      }
      await patch({ [k]: el.checked });
    } catch (e) { el.checked = !el.checked; toast(e.message); }
  });
  main.querySelectorAll('[data-act]').forEach(b => b.onclick = () => editServer(b.dataset.act));
}
function editServer(what) {
  const cur = { k: 'current_password', label: 'Current password', type: 'password', value: '' };
  const S = {
    domain: () => sheet({ title: 'Domain', fields: [
      { k: 'domain', label: 'Domain', value: cfg.domain, placeholder: 'remodroid.example.com', hint: 'Point its DNS A record at this server first. Leave empty for plain HTTP.' },
      { k: 'email', label: 'Email for Let\'s Encrypt (optional)', type: 'email', value: cfg.email, placeholder: 'you@example.com' }],
      note: 'The server restarts and requests a certificate.', save: v => patch({ domain: v.domain, email: v.email }) }),
    tls_port: () => sheet({ title: 'HTTPS port', fields: [{ k: 'tls_port', label: 'HTTPS port', type: 'number', value: cfg.tls_port, hint: 'Usually 443.' }], save: v => patch(v) }),
    http_port: () => sheet({ title: 'Web port', fields: [{ k: 'http_port', label: 'Web port', type: 'number', value: cfg.http_port, hint: 'With a domain keep 80: Let\'s Encrypt checks it.' }], save: v => patch(v) }),
    username: () => sheet({ title: 'Username', fields: [{ k: 'username', label: 'Username', value: cfg.username }, cur], note: 'You\'ll log in again with the new name.', save: v => patch(v) }),
    password: () => sheet({ title: 'Password', fields: [cur, { k: 'password', label: 'New password', type: 'password', hint: 'At least 8 characters.' }, { k: 'repeat', label: 'Repeat the new password', type: 'password' }],
      save: v => { if (v.password !== v.repeat) throw new Error('The new passwords don\'t match'); return patch({ current_password: v.current_password, password: v.password }); } }),
    turn_port: () => sheet({ title: 'STUN/TURN port', fields: [{ k: 'turn_port', label: 'Port (UDP)', type: 'number', value: cfg.turn_port, hint: 'Usually 3478.' }], save: v => patch(v) }),
    public_ip: () => sheet({ title: 'Public IPv4', fields: [{ k: 'public_ip', label: 'Public IPv4 address', value: cfg.public_ip || cfg.host_hint, placeholder: '203.0.113.10', hint: 'On AWS/GCP this is the elastic/external IP, not the private one.' }], save: v => patch(v) }),
    turn_on: () => sheet({ title: 'Built-in STUN/TURN', saveLabel: 'Turn on', fields: [
      { k: 'public_ip', label: 'Public IPv4 address', value: cfg.public_ip || (/^\d+\.\d+\.\d+\.\d+$/.test(cfg.host_hint || '') ? cfg.host_hint : ''), placeholder: '203.0.113.10' },
      { k: 'turn_port', label: 'Port (UDP)', type: 'number', value: cfg.turn_port || 3478 }],
      save: v => patch({ turn_enabled: true, public_ip: v.public_ip, turn_port: v.turn_port }) }),
    cf_key: () => sheet({ title: 'Cloudflare TURN', fields: [
      { k: 'key_id', label: 'TURN key ID', value: (cfg.turn && cfg.turn.key_id) || '' },
      { k: 'api_token', label: 'API token', type: 'password', placeholder: cfg.turn && cfg.turn.has_token ? 'unchanged' : '' }],
      save: v => patch({ turn: { cloudflare: true, key_id: v.key_id, api_token: v.api_token } }) }),
    cf_token: () => S.cf_key(),
    logout: async () => { try { await api('POST', '/api/v1/logout'); } catch (_) {} location.href = '/'; },
  };
  if (S[what]) S[what]();
}

/* ---------------- routing ---------------- */
function route() {
  const h = decodeURIComponent(location.hash.replace(/^#/, '')) || 'devices';
  const [tab, id, sub] = h.split('/');
  if (tab === 'agents' && id === 'add') return { tab, add: true, sub: sub || 'install' };
  return { tab: ['devices', 'agents', 'server'].includes(tab) ? tab : 'devices', id };
}
function show() {
  const r = route();
  document.querySelectorAll('.tabbar a').forEach(a => a.setAttribute('aria-current', a.dataset.tab === r.tab ? 'page' : 'false'));
  closeSheet();
  if (r.tab === 'devices') renderDevices();
  else if (r.tab === 'agents' && r.add) renderAdd(r.sub);
  else if (r.tab === 'agents' && r.id) renderAgent(r.id);
  else if (r.tab === 'agents') renderAgents();
  else renderServer();
}
window.addEventListener('hashchange', () => { if (route().id !== agentCfgFor) agentCfgFor = null; show(); window.scrollTo(0, 0); });

/* live-ish: lists refresh every few seconds while you look at them (not while editing) */
setInterval(async () => {
  const r = route();
  if (document.hidden || document.querySelector('.psheet') || r.tab === 'server') return;
  if (r.add && r.sub === 'connect') { pollCode(); return; }
  await loadDevices();
  if (r.add && r.sub === 'phone') show();
  else if (!r.add && !r.id) show();
}, 3000);

(async () => {
  if (location.hash === '#connect') location.replace('#agents/add/connect'); /* old agent.html links */
  await loadDevices();
  if (!location.hash && !devices.length) location.replace('#agents/add');
  show();
})();
})();
