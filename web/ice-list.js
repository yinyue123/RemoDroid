/* The "other STUN/TURN servers" list, shared by wizard.html and panel.html.
 * Adding a server needs only its address: STUN on port 3478 unless you change
 * the type, port or login. Editing keeps whatever you don't touch, including a
 * TURN password the browser never sees (the server fills it back in from the
 * entry's old URL, sent as "was").
 *
 *   IceList.mount(box, entries, { onChange(entries) → Promise|void })
 *   entries: [{ url: "turn:203.0.113.5:3478?transport=udp", username, credential, has_credential }] */
(() => {
'use strict';
const esc = s => String(s ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const DEF_PORT = { stun: 3478, turn: 3478, turns: 5349 };

function parse(url) {
  const m = /^(stun|turns?):(\[[^\]]+\]|[^:?]+)(?::(\d+))?(?:\?transport=(udp|tcp))?$/i.exec(String(url || '').trim());
  if (!m) return { type: 'stun', host: String(url || ''), port: 3478, transport: 'udp' };
  const type = m[1].toLowerCase();
  return { type, host: m[2], port: m[3] ? +m[3] : DEF_PORT[type], transport: (m[4] || (type === 'turns' ? 'tcp' : 'udp')).toLowerCase() };
}
function build(p) {
  const host = /:/.test(p.host) && !/^\[/.test(p.host) ? `[${p.host}]` : p.host; /* IPv6 */
  const base = `${p.type}:${host}:${p.port || DEF_PORT[p.type]}`;
  return p.type === 'stun' ? base : `${base}?transport=${p.type === 'turns' ? 'tcp' : p.transport || 'udp'}`;
}
function label(e) {
  const p = parse(e.url);
  return { title: `${p.host}:${p.port}`, sub: p.type === 'stun' ? 'STUN' : `${p.type.toUpperCase()} · ${p.transport.toUpperCase()}${e.username ? ' · ' + e.username : ''}` };
}

function mount(box, initial, opts) {
  opts = opts || {};
  let list = (initial || []).map(e => ({ ...e }));
  let open = -2; /* -2 none, -1 new, i = editing entry i */
  let busy = false;

  async function commit(next, msg) {
    if (busy) return false;
    busy = true;
    try {
      const r = opts.onChange ? await opts.onChange(next.map(e => ({ ...e }))) : null;
      list = Array.isArray(r) ? r : next;
      open = -2;
      render();
      if (msg && opts.toast) opts.toast(msg);
      return true;
    } catch (e) {
      const err = box.querySelector('.ice-err');
      if (err) { err.textContent = e.message || String(e); err.hidden = false; }
      return false;
    } finally { busy = false; }
  }

  function editor(i) {
    const e = i >= 0 ? list[i] : { url: '', username: '', credential: '' };
    const p = i >= 0 ? parse(e.url) : { type: 'stun', host: '', port: 3478, transport: 'udp' };
    const kept = i >= 0 && (e.has_credential || e.credential);
    return `<div class="ice-edit" data-type="${p.type}">
      <div class="field"><label for="ice-host">Address</label>
        <input id="ice-host" type="text" inputmode="url" autocomplete="off" autocapitalize="off" spellcheck="false" placeholder="203.0.113.10 or stun.example.com" value="${esc(p.host)}"></div>
      <div class="ice-more">
        <div class="seg ice-type" role="group" aria-label="Type">${['stun', 'turn', 'turns'].map(t => `<button type="button" data-t="${t}" aria-selected="${p.type === t}">${t.toUpperCase()}</button>`).join('')}</div>
        <div class="row2">
          <div class="field"><label for="ice-port">Port</label><input id="ice-port" type="number" min="1" max="65535" value="${esc(p.port)}"></div>
          <div class="field turn-only"><label for="ice-tr">Transport</label><select id="ice-tr"><option value="udp" ${p.transport === 'udp' ? 'selected' : ''}>UDP</option><option value="tcp" ${p.transport === 'tcp' ? 'selected' : ''}>TCP</option></select></div>
        </div>
        <div class="row2 turn-only">
          <div class="field"><label for="ice-user">Username</label><input id="ice-user" type="text" autocomplete="off" autocapitalize="off" spellcheck="false" value="${esc(e.username)}"></div>
          <div class="field"><label for="ice-pass">Password</label><input id="ice-pass" type="password" autocomplete="new-password" placeholder="${kept ? 'unchanged' : ''}"></div>
        </div>
      </div>
      <p class="ice-err callout bad" hidden></p>
      <div class="ice-btns">
        <button class="btn small" type="button" data-ice="save">${i >= 0 ? 'Save' : 'Add'}</button>
        <button class="btn small ghost" type="button" data-ice="cancel">Cancel</button>
        ${i >= 0 ? '<span class="spacer"></span><button class="btn small danger" type="button" data-ice="delete">Remove</button>' : ''}
      </div>
    </div>`;
  }

  function render() {
    box.innerHTML = `<div class="ice-list">${list.map((e, i) => {
      if (open === i) return `<div class="ice-row open">${editor(i)}</div>`;
      const l = label(e);
      return `<button class="ice-row" type="button" data-i="${i}"><span class="ice-ic ${parse(e.url).type}">${parse(e.url).type === 'stun' ? 'S' : 'T'}</span>
        <span class="ice-txt"><b>${esc(l.title)}</b><small>${esc(l.sub)}</small></span><span class="chev">›</span></button>`;
    }).join('')}${!list.length && open !== -1 ? `<p class="ice-empty">${esc(opts.empty || 'None yet.')}</p>` : ''}
    ${open === -1 ? `<div class="ice-row open">${editor(-1)}</div>` : `<button class="ice-add" type="button" data-ice="add">+ Add a server</button>`}</div>`;
    box.querySelectorAll('.ice-row[data-i]').forEach(b => b.onclick = () => { open = +b.dataset.i; render(); focusHost(); });
    const add = box.querySelector('[data-ice="add"]');
    if (add) add.onclick = () => { open = -1; render(); focusHost(); };
    const ed = box.querySelector('.ice-edit');
    if (!ed) return;
    ed.querySelectorAll('.ice-type button').forEach(b => b.onclick = () => {
      const was = ed.dataset.type;
      ed.dataset.type = b.dataset.t;
      ed.querySelectorAll('.ice-type button').forEach(x => x.setAttribute('aria-selected', String(x === b)));
      const port = ed.querySelector('#ice-port');
      if (+port.value === DEF_PORT[was]) port.value = DEF_PORT[b.dataset.t];
      if (b.dataset.t === 'turns') ed.querySelector('#ice-tr').value = 'tcp';
    });
    ed.querySelector('[data-ice="cancel"]').onclick = () => { open = -2; render(); };
    const del = ed.querySelector('[data-ice="delete"]');
    if (del) del.onclick = () => commit(list.filter((_, k) => k !== open), 'Server removed');
    ed.querySelector('#ice-host').onkeydown = ev => { if (ev.key === 'Enter') ed.querySelector('[data-ice="save"]').click(); };
    ed.querySelector('[data-ice="save"]').onclick = () => {
      const err = ed.querySelector('.ice-err');
      const host = ed.querySelector('#ice-host').value.trim().replace(/^(stun|turns?):\/*/i, '');
      const type = ed.dataset.type, port = +ed.querySelector('#ice-port').value || DEF_PORT[type];
      if (!/^(\[[0-9a-f:.]+\]|[0-9a-f:]*:[0-9a-f:.]+|[A-Za-z0-9.-]+)$/i.test(host)) { err.textContent = 'Enter an IP address or host name, e.g. 203.0.113.10'; err.hidden = false; return; }
      if (port < 1 || port > 65535) { err.textContent = 'The port must be 1-65535'; err.hidden = false; return; }
      const prev = open >= 0 ? list[open] : null;
      const url = build({ type, host, port, transport: ed.querySelector('#ice-tr').value });
      const entry = { url, username: '', credential: '' };
      if (type !== 'stun') {
        entry.username = ed.querySelector('#ice-user').value.trim();
        const pw = ed.querySelector('#ice-pass').value;
        if (pw) entry.credential = pw;
        else if (prev && entry.username === prev.username) { /* keep the old password */
          entry.credential = prev.credential || '';
          entry.has_credential = prev.has_credential;
          entry.was = prev.was || prev.url;
        }
        if (/[\s,]/.test(entry.username + entry.credential)) { err.textContent = 'Username and password can\'t contain spaces or commas'; err.hidden = false; return; }
      }
      const next = list.slice();
      if (open >= 0) next[open] = entry; else next.push(entry);
      commit(next, open >= 0 ? 'Server saved' : 'Server added');
    };
  }
  function focusHost() { const h = box.querySelector('#ice-host'); if (h) h.focus(); }
  render();
  return { get: () => list.map(e => ({ ...e })), set: l => { list = l.map(e => ({ ...e })); open = -2; render(); } };
}

/* "url user cred, url, …" ⇄ entries (the setup wizard's ice_extra string) */
function fromLine(line) {
  return String(line || '').split(/[,\n]/).map(x => x.trim()).filter(Boolean).map(x => {
    const [url, username = '', credential = ''] = x.split(/\s+/);
    return { url, username, credential };
  });
}
function toLine(list) {
  return list.map(e => e.username && e.credential ? `${e.url} ${e.username} ${e.credential}` : e.url).join(', ');
}

window.IceList = { mount, parse, build, label, fromLine, toLine };
})();
