/* pagemode.js: Page mode for the remote browser (browser/dom-mirror.md §8–§10). Instead of the video it
 * shows the page's own elements, rebuilt by mirror.js in a frame that can't run scripts or fetch, under a
 * bar of its own (address, back, forward, tabs, users, cookies), and sends what the person does back by
 * element: a tap, where they are looking, what they type, what they pick.
 *
 *   PageMode.init({ dom(), ctrl(t, d), key(hid), toast(msg), video() })   app.js's side
 *   PageMode.offered(yes)    this device has it (caps.mirror)
 *   PageMode.choose(mode)    'auto': the page, and the picture by itself while the page can't show something
 *                            (a playing video, a prompt of the browser's, a page too big); 'page': the page
 *                            only, never the picture unless chosen; 'video': the picture
 *   PageMode.set(on)         ask for the page, or for the video again
 *   PageMode.state(d)        the agent's mirror.state
 *   PageMode.data(buf)       a "dom" channel message: a kind byte, a "more follows" byte, the bytes
 *   PageMode.end()           the session ended
 *
 * "dom" from the agent: 'J' deflated JSON (an array of doc / diff / bar / list / res.none), 'R' a resource
 * (u16 address length, the address, u8 type length, the MIME type, the bytes). To it: JSON text. */
'use strict';
(() => {
const CSS = `
#pm{position:fixed;inset:0;z-index:19;display:flex;flex-direction:column;background:#101114;color:#e9eaee;
  font:15px/1.2 -apple-system,system-ui,sans-serif;padding-top:env(safe-area-inset-top,0px)}
#pm[hidden]{display:none}
#pmBar{flex:none;height:44px;display:flex;align-items:center;gap:2px;padding:0 4px;background:#1b1d22;border-bottom:1px solid #2c2f37}
#pmBar button{flex:none;width:38px;height:38px;border:0;border-radius:9px;background:none;color:inherit;font-size:19px;padding:0}
#pmBar button:active,#pmMenu button:active,#pmList button:active{background:#30343d}
#pmBar button.on{color:#ffd24a}
#pmAddr{flex:1;min-width:0;height:34px;border:0;border-radius:9px;padding:0 10px;background:#2a2d35;color:inherit;font:inherit;
  text-overflow:ellipsis}
#pmAddr:focus{outline:2px solid #4c8dff}
#pmView{flex:1;position:relative;overflow:hidden;background:#101114}
#pmFrame{position:absolute;left:0;top:0;border:0;transform-origin:0 0;background:#fff}
#pmFrame.old{opacity:.45}
#pmMenu,#pmList{position:absolute;z-index:2;right:6px;top:calc(env(safe-area-inset-top,0px) + 48px);width:min(300px,calc(100% - 12px));
  max-height:calc(100% - 60px - env(safe-area-inset-top,0px));overflow:auto;-webkit-overflow-scrolling:touch;border-radius:14px;background:#22252c;
  box-shadow:0 10px 40px rgba(0,0,0,.5);padding:6px}
#pmList{left:6px;width:auto}
#pmMenu button,#pmList button{display:block;width:100%;min-height:42px;border:0;border-radius:9px;background:none;color:inherit;
  font:inherit;text-align:left;padding:6px 12px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
#pmMenu button.cur{color:#7fb0ff}
#pmMenu hr{border:0;border-top:1px solid #343842;margin:4px 6px}
#pmList h3{margin:6px 12px;font-size:13px;font-weight:600;color:#9aa0ad}
#pmList small{display:block;color:#9aa0ad;overflow:hidden;text-overflow:ellipsis}
#pmList .row{display:flex}#pmList .row button:last-child{flex:none;width:44px;text-align:center}
#pmNote{position:absolute;z-index:1;left:50%;bottom:14px;transform:translateX(-50%);max-width:90%;padding:8px 14px;border-radius:12px;
  background:rgba(20,22,26,.9);color:#fff;font-size:13px;pointer-events:none}
`;
const TEXTY = new Set(['text', 'search', 'email', 'url', 'tel', 'password', 'number', '']);
let api = null, root = null, frame = null, mirror = null, bar = null, wanted = false, on = false, mode = 'video';
const MODES = { auto: 'Auto', page: 'Page', video: 'Video' };
let parts = [], chain = Promise.resolve(), doc = null, confirmAct = '', confirmAt = 0;
/* why the picture is shown for now although Page mode is on (dom-mirror.md §7): what can't be sent as elements */
const WHY = { media: 'A video is playing', windows: 'The browser is asking something', big: 'This page is too big for Page mode' };
const video = new Set();
let videoNow = false;
const STAY = { media: 'A video is playing on the browser', windows: 'The browser is asking something',
               big: 'A big page: it may take a moment' };
function fallback(reason, yes) {
  if (mode === 'page') { /* never the picture by itself: say what can't be seen, and how to see it */
    if (yes && reason === 'big') { send({ k: 'full' }); note(STAY.big); return; }
    if (yes) api.toast(STAY[reason] + ' · choose Video in the menu to see it');
    return;
  }
  yes ? video.add(reason) : video.delete(reason);
  const v = video.size > 0;
  if (v === videoNow || !on) return;
  videoNow = v;
  api.ctrl('mirror.video', { on: v });
  root.hidden = v;
  if (v) api.toast(WHY[reason] + ' · Video for now');
}
const el = (tag, props, kids) => {
  const e = Object.assign(document.createElement(tag), props || {});
  for (const k of kids || []) e.append(k);
  return e;
};
const send = m => {
  const c = api.dom();
  if (c && c.readyState === 'open') c.send(JSON.stringify(m));
};
const cmd = (rd, more) => send(Object.assign({ rd }, more || {}));

/* ---------------- the bar ---------------- */
function build() {
  document.head.append(el('style', { textContent: CSS }));
  const b = (label, title, fn) => el('button', { type: 'button', textContent: label, title, onclick: fn });
  const addr = el('input', { id: 'pmAddr', type: 'text', inputMode: 'url', enterKeyHint: 'go', autocapitalize: 'off', autocomplete: 'off',
    spellcheck: false, placeholder: 'Search or enter address' });
  addr.setAttribute('autocorrect', 'off');
  addr.onfocus = () => { addr.value = (bar && bar.url) || ''; addr.select(); closePanels(); };
  addr.onblur = () => showBar();
  addr.onkeydown = e => {
    if (e.key === 'Enter') { const t = addr.value.trim(); addr.blur(); if (t) cmd('go', { text: t }); }
    else if (e.key === 'Escape') addr.blur();
  };
  frame = el('iframe', { id: 'pmFrame', title: 'The remote page' });
  root = el('div', { id: 'pm', hidden: true }, [
    el('div', { id: 'pmBar' }, [
      b('◀', 'Back', () => cmd('back')), b('▶', 'Forward', () => cmd('forward')), b('⟳', 'Reload', () => cmd('reload')), addr,
      Object.assign(b('☆', 'Favourite', () => cmd('fav.toggle')), { id: 'pmFav' }), b('⋯', 'Menu', () => toggleMenu())]),
    el('div', { id: 'pmView' }, [frame, el('div', { id: 'pmNote', hidden: true })]),
    el('div', { id: 'pmMenu', hidden: true }), el('div', { id: 'pmList', hidden: true })]);
  document.body.append(root);
  mirror = new Mirror(frame, send);
  window.__pmMirror = mirror; /* for tests and for looking at it from the console */
  mirror.hold = holdProp;
  mirror.ready.then(listen);
  new ResizeObserver(fit).observe(root.querySelector('#pmView'));
}
function showBar() {
  const a = root.querySelector('#pmAddr'), f = root.querySelector('#pmFav');
  if (!bar) return;
  if (document.activeElement !== a) a.value = bar.note || bar.title || bar.url.replace(/^https?:\/\//, '') || (bar.running ? '' : 'The browser is stopped');
  f.textContent = bar.fav ? '★' : '☆';
  f.classList.toggle('on', !!bar.fav);
  if (!root.querySelector('#pmMenu').hidden) drawMenu();
}
function closePanels() {
  root.querySelector('#pmMenu').hidden = true;
  root.querySelector('#pmList').hidden = true;
}
function toggleMenu() {
  const m = root.querySelector('#pmMenu'), open = m.hidden;
  closePanels();
  if (open) { drawMenu(); m.hidden = false; }
}
/* the ones that throw something away are asked twice, as on the bar itself */
function twice(act, label, fn) {
  return item(confirmAct === act && Date.now() - confirmAt < 4000 ? 'Tap again: ' + label : label, () => {
    if (confirmAct === act && Date.now() - confirmAt < 4000) { confirmAct = ''; closePanels(); fn(); }
    else { confirmAct = act; confirmAt = Date.now(); drawMenu(); }
  }, true);
}
function item(label, fn, stay) {
  return el('button', { type: 'button', textContent: label, onclick: () => { if (!stay) closePanels(); fn(); } });
}
function drawMenu() {
  const m = root.querySelector('#pmMenu'), s = bar || { tabs: [], users: [] };
  m.textContent = '';
  m.append(item('Home', () => cmd('home')), item('New tab', () => cmd('tab.new')));
  if (s.tabs.length > 1) {
    m.append(el('hr'));
    for (const t of s.tabs) m.append(Object.assign(item(t.title || t.url, () => cmd('tab.select', { id: t.id })), { className: t.id === s.front ? 'cur' : '' }));
    m.append(item('Close tab', () => cmd('tab.close')));
  }
  m.append(el('hr'), item('Favourites', () => cmd('list', { kind: 'favourites' })), item('History', () => cmd('list', { kind: 'history' })),
    item('User: ' + (s.user || ''), () => users()), el('hr'),
    twice('cookies', 'Clear cookies', () => cmd('clear.cookies')), twice('all', 'Clear history & data', () => cmd('clear.all')),
    twice('id', 'New identity', () => cmd('identity.new')), item('Restart browser', () => cmd('restart')),
    item(s.running ? 'Stop browser' : 'Start browser', () => cmd(s.running ? 'stop' : 'start')), el('hr'),
    item('Proxy: ' + (s.proxy === 1 ? 'ok' : s.proxy === 0 ? 'down' : s.proxy === -2 ? 'none set' : '…'), () => {}, true),
    ...Object.keys(MODES).map(k => Object.assign(item((k === mode ? '● ' : '○ ') + MODES[k] + ' mode', () => PageMode.choose(k)),
      { className: k === mode ? 'cur' : '' })));
}
function users() {
  const l = root.querySelector('#pmList');
  l.textContent = '';
  l.append(el('h3', { textContent: 'Users' }));
  for (const u of (bar && bar.users) || []) l.append(Object.assign(item(u, () => cmd('user', { name: u })), { className: u === bar.user ? 'cur' : '' }));
  l.hidden = false;
}
function list(m) {
  const l = root.querySelector('#pmList'), hist = m.kind === 'history';
  closePanels();
  l.textContent = '';
  l.append(el('h3', { textContent: hist ? 'History' : 'Favourites' }));
  let day = '';
  for (const it of m.items) {
    if (hist) {
      const d = new Date(it.t * 1000).toLocaleDateString(undefined, { weekday: 'long', month: 'short', day: 'numeric' });
      if (d !== day) l.append(el('h3', { textContent: day = d }));
    }
    const open = el('button', { type: 'button', onclick: () => { closePanels(); cmd('go', { text: it.url }); } },
      [it.title || it.url, el('small', { textContent: it.url.replace(/^https?:\/\//, '') })]);
    l.append(hist ? open : el('div', { className: 'row' }, [open,
      el('button', { type: 'button', textContent: '✕', title: 'Remove', onclick: () => { cmd('fav.delete', { url: it.url }); cmd('list', { kind: 'favourites' }); } })]));
  }
  if (!m.items.length) l.append(el('h3', { textContent: 'Nothing here yet' }));
  l.hidden = false;
}
function note(text) {
  const n = root.querySelector('#pmNote');
  n.textContent = text || '';
  n.hidden = !text;
}

/* The frame has the real page's window size in CSS pixels, so the site lays out as it does there; it is
 * then scaled to fit, as the video is. */
function fit() {
  const v = root.querySelector('#pmView');
  if (!doc || !v.clientWidth) return;
  const s = Math.min(v.clientWidth / doc.w, v.clientHeight / doc.h);
  frame.style.width = doc.w + 'px';
  frame.style.height = doc.h + 'px';
  frame.style.transform = `scale(${s})`;
  frame.style.left = Math.max(0, (v.clientWidth - doc.w * s) / 2) + 'px';
  /* a page shaped unlike this screen (the browser in desktop mode on a phone) leaves room around it: the
   * page's own background there, not a white band */
  try {
    const d = frame.contentDocument, w = frame.contentWindow, clear = c => !c || c === 'transparent' || /rgba\(.*,\s*0\)$/.test(c);
    let bg = d.body && w.getComputedStyle(d.body).backgroundColor;
    if (clear(bg) && d.documentElement) bg = w.getComputedStyle(d.documentElement).backgroundColor;
    v.style.background = clear(bg) ? '' : bg;
  } catch (e) {}
}

/* ---------------- what arrives ---------------- */
async function inflate(u8) {
  const ds = new DecompressionStream('deflate-raw'), w = ds.writable.getWriter();
  w.write(u8); w.close();
  return new Response(ds.readable).text();
}
let dim = 0;
function undim() {
  clearTimeout(dim);
  if (!bar || bar.running) frame.classList.remove('old');
}
function message(m) {
  PageMode.log.push(m.k === 'doc' ? 'doc ' + m.url : m.k === 'diff' ? 'diff ' + m.o.length : m.k);
  if (PageMode.log.length > 40) PageMode.log.shift();
  switch (m.k) {
  case 'doc':
    fallback('media', false);
    fallback('big', !!m.big);
    if (m.big) break;
    doc = { w: m.w, h: m.h };
    view.sent = '';
    mirror.on(m);
    fit();
    frame.classList.remove('old');
    note('');
    setTimeout(() => sendView(true), 0); /* where this viewer is, to the page that has just been sent */
    break;
  case 'diff':
    mirror.on(m);
    undim();
    richCaret();
    if (!fit.later) fit.later = setTimeout(() => { fit.later = 0; fit(); }, 500); /* the page's colours arrive late */
    break;
  case 'bar': {
    const nav = bar && (bar.front !== m.front || bar.url !== m.url);
    bar = m;
    /* the old page, dimmed, until the new one's snapshot. A page that only changes its address (a chat opened
     * from a list, by script) sends no snapshot: its next difference, or a moment, ends the dimming */
    if (nav) { frame.classList.add('old'); clearTimeout(dim); dim = setTimeout(undim, 2500); }
    if (!m.running) { note('The browser is stopped'); frame.classList.add('old'); }
    showBar();
    fallback('windows', m.windows > 1); /* a prompt, a list, a download: only the picture can show it */
    break;
  }
  case 'list': list(m); break;
  case 'res.none': mirror.resource(m.u, null); break;
  case 'scroll': pageScrolled(m); break;
  case 'media': fallback('media', !!m.playing); break;
  case 'dialog': setTimeout(() => dialog(m), 30); break;
  }
}
function data(buf) {
  const u8 = new Uint8Array(buf);
  if (u8.length < 2) return;
  PageMode.log.push('raw ' + String.fromCharCode(u8[0]) + u8[1] + ' ' + u8.length);
  parts.push(u8.subarray(2));
  if (u8[1]) return; /* more follows */
  const kind = u8[0], all = parts.length === 1 ? parts[0] : new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  if (parts.length > 1) { let o = 0; for (const p of parts) { all.set(p, o); o += p.length; } }
  parts = [];
  if (kind === 0x52) { /* 'R' */
    const ul = all[0] << 8 | all[1], u = new TextDecoder().decode(all.subarray(2, 2 + ul)), ml = all[2 + ul];
    const type = new TextDecoder().decode(all.subarray(3 + ul, 3 + ul + ml));
    chain = chain.then(() => mirror.resource(u, new Blob([all.subarray(3 + ul + ml)], { type })));
  } else if (kind === 0x4a) /* 'J': in the order sent, though inflating takes a moment */
    chain = chain.then(() => inflate(all)).then(t => { for (const m of JSON.parse(t)) message(m); }).catch(e => console.warn('page mode', e));
}

/* alert, confirm, prompt: the real page waits; this device's own dialog asks, and the answer goes back */
function dialog(m) {
  let accept = true, text;
  const host = (m.url || '').replace(/^https?:\/\/([^/]+).*/, '$1');
  const msg = (host ? host + ' says:\n\n' : '') + (m.message || '');
  if (m.type === 'confirm') accept = window.confirm(msg);
  else if (m.type === 'prompt') { text = window.prompt(msg, m.defaultPrompt || ''); accept = text !== null; }
  else window.alert(msg);
  send(accept && text !== undefined ? { k: 'dialog.answer', accept, text } : { k: 'dialog.answer', accept });
}

/* ---------------- what the person does ---------------- */
const idOf = n => {
  for (; n; n = n.parentNode || n.host) { const i = mirror.ids.get(n); if (i) return { i, n }; }
  return null;
};
const where = (target, x, y) => {
  const t = idOf(target);
  if (!t || t.n.nodeType !== 1) return null;
  const r = t.n.getBoundingClientRect();
  return { i: t.i, fx: r.width ? (x - r.left) / r.width : 0.5, fy: r.height ? (y - r.top) / r.height : 0.5 };
};
const op = m => send(Object.assign({ k: 'op' }, m));
const ptr = new Map(); /* pointerId → the gesture: where it began, whether it became a drag sent as a touch */
const view = { sent: '', raf: 0, last: 0, moved: new Set(), y: 0, t: 0, v: 0 };
const typing = { el: null, value: '', at: 0, held: null };
/* a box the page made editable itself (contenteditable: a chat's message box, an editor), or null */
const rich = t => {
  for (; t; t = t.parentNode) if (t.nodeType === 1) return t.isContentEditable && !isTexty(t) ? t : null;
  return null;
};
const richly = { at: 0 }; /* when the last key went to such a box */
function richText(text) {
  if (!text) return;
  richly.at = performance.now();
  /* several lines are pasted there in one piece: as keys, each new line would be an Enter (a chat sends on it) */
  if (/[\r\n]/.test(text)) api.ctrl('clip.set', { text, paste: true });
  else api.ctrl('text', { text, keys: true });
}
function richKey(hid, mods) { richly.at = performance.now(); api.key(hid, mods); }
/* The caret in such a box is this device's own, and the text under it is replaced by the real page's. After a
 * difference that follows a key, a caret left on an element (the first letter of a line, a line made again)
 * goes to the end of it; one inside a text is kept in place by mirror.js. */
function richCaret() {
  if (performance.now() - richly.at > 1500) return;
  const d = frame.contentDocument, s = d.getSelection(), box = rich(d.activeElement);
  if (!box || !s || !s.rangeCount || !s.isCollapsed) return;
  let a = s.anchorNode;
  if (!a || a.nodeType !== 1 || !box.contains(a) || !a.textContent) return;
  if (a === box && s.anchorOffset > 0 && a.childNodes[s.anchorOffset - 1]) a = a.childNodes[s.anchorOffset - 1];
  s.selectAllChildren(a);
  s.collapseToEnd();
}

function listen() {
  const d = frame.contentDocument, w = frame.contentWindow;
  /* nothing in the mirror goes anywhere by itself: links, forms, checkboxes all happen on the real page */
  d.addEventListener('click', e => e.preventDefault(), true);
  d.addEventListener('submit', e => e.preventDefault(), true);
  d.addEventListener('contextmenu', e => e.preventDefault(), true);
  d.addEventListener('dragstart', e => e.preventDefault(), true);
  d.addEventListener('pointerdown', e => {
    if (e.pointerType === 'mouse' && e.button !== 0) return;
    closePanels();
    const at = where(e.target, e.clientX, e.clientY);
    if (at) ptr.set(e.pointerId, { at, x: e.clientX, y: e.clientY, t: performance.now(), drag: false, n: ptr.size });
  }, true);
  d.addEventListener('pointermove', e => {
    const p = ptr.get(e.pointerId);
    if (!p) return;
    const dx = e.clientX - p.x, dy = e.clientY - p.y;
    if (p.drag) { op({ o: 'touch', ph: 'move', id: p.n, dx, dy }); return; }
    /* It moved. If the browser takes it as a scroll, it cancels the pointer (below) and nothing was sent.
     * If it doesn't within a moment, nothing here scrolls that way: the page gets it as a finger. */
    if (Math.hypot(dx, dy) > 10 && performance.now() - p.t > 90 && performance.now() - view.last > 150) {
      p.drag = true;
      op(Object.assign({ o: 'touch', ph: 'down', id: p.n }, p.at));
      op({ o: 'touch', ph: 'move', id: p.n, dx, dy });
    }
  }, true);
  const lift = cancelled => e => {
    const p = ptr.get(e.pointerId);
    if (!p) return;
    ptr.delete(e.pointerId);
    if (p.drag) op({ o: 'touch', ph: cancelled ? 'cancel' : 'up', id: p.n, dx: e.clientX - p.x, dy: e.clientY - p.y });
    else if (!cancelled && Math.hypot(e.clientX - p.x, e.clientY - p.y) <= 10 && performance.now() - view.last > 150)
      op(Object.assign({ o: 'tap', hold: Math.round(performance.now() - p.t) }, p.at));
  };
  d.addEventListener('pointerup', lift(false), true);
  d.addEventListener('pointercancel', lift(true), true);

  /* where the person is looking: every scroller that moved, once a frame, and once more when it stops */
  d.addEventListener('scroll', e => {
    view.last = performance.now();
    view.moved.add(e.target === d ? d.scrollingElement : e.target);
    if (!view.raf) view.raf = w.requestAnimationFrame(() => { view.raf = 0; sendView(false); });
    clearTimeout(view.stop);
    view.stop = setTimeout(() => sendView(true), 140);
  }, true);

  /* typing: the field in the mirror takes the keyboard natively; what changed in it goes as keys */
  d.addEventListener('focusin', e => {
    const t = e.target;
    typing.el = isTexty(t) ? t : null;
    typing.value = typing.el ? t.value : '';
  }, true);
  d.addEventListener('focusout', () => { applyHeld(); typing.el = null; }, true);
  d.addEventListener('input', e => {
    const t = e.target;
    if (t !== typing.el) return;
    const a = typing.value, b = t.value;
    let pre = 0, suf = 0;
    while (pre < a.length && pre < b.length && a[pre] === b[pre]) pre++;
    while (suf < a.length - pre && suf < b.length - pre && a[a.length - 1 - suf] === b[b.length - 1 - suf]) suf++;
    const gone = Array.from(a.slice(pre, a.length - suf)).length, added = b.slice(pre, b.length - suf);
    for (let k = 0; k < gone; k++) api.key(0x2a); /* Backspace */
    if (added) api.ctrl('text', { text: added, keys: true });
    typing.value = b;
    typing.at = performance.now();
  }, true);
  /* A box the page made editable itself is not an <input>: nothing is typed into the mirror's copy, which
   * the real page would never hear of. What the keyboard was about to do goes to the real box as keys, and
   * what that makes of it comes back as differences. */
  d.addEventListener('beforeinput', e => {
    if (!rich(e.target)) return;
    const k = e.inputType;
    if (/Composition/.test(k)) return; /* an input method at work can't be stopped: its result is sent when it ends */
    e.preventDefault();
    if (k === 'insertParagraph') richKey(0x28, 0);
    else if (k === 'insertLineBreak') richKey(0x28, 2); /* Shift+Enter */
    else if (/^delete/.test(k)) richKey(/Forward$/.test(k) ? 0x4c : 0x2a, 0);
    else if (/^insert/.test(k)) {
      const text = e.data != null ? e.data : e.dataTransfer ? e.dataTransfer.getData('text/plain') : '';
      if (text && k === 'insertReplacementText') { /* a correction: the word it stands for goes first */
        let n = 0;
        try {
          const t = e.getTargetRanges()[0], r = d.createRange();
          r.setStart(t.startContainer, t.startOffset);
          r.setEnd(t.endContainer, t.endOffset);
          n = Array.from(r.toString()).length;
        } catch (x) {}
        for (let i = 0; i < n; i++) richKey(0x2a, 0);
      }
      richText(text);
    }
  }, true);
  d.addEventListener('compositionend', e => { if (rich(e.target)) richText(e.data); }, true);
  d.addEventListener('keydown', e => {
    if (!typing.el && rich(e.target) && !e.isComposing && e.keyCode !== 229) {
      const mods = (e.ctrlKey ? 1 : 0) | (e.shiftKey ? 2 : 0) | (e.altKey ? 4 : 0) | (e.metaKey ? 8 : 0);
      const key = { Enter: 0x28, Backspace: 0x2a, Delete: 0x4c }[e.key];
      if (key) { e.preventDefault(); richKey(key, mods); return; }
    }
    const hid = { Enter: 0x28, Tab: 0x2b, Escape: 0x29, ArrowLeft: 0x50, ArrowRight: 0x4f, ArrowUp: 0x52, ArrowDown: 0x51 }[e.key];
    if (!hid) return;
    if (e.key === 'Enter' && typing.el && typing.el.localName === 'textarea') return; /* a new line: the input event has it */
    if (e.key === 'Enter' || e.key === 'Tab') e.preventDefault();
    api.key(hid);
  }, true);
  /* a select, a date, a colour, a slider: this device's own picker, and the value goes over */
  d.addEventListener('change', e => {
    const t = e.target, at = idOf(t);
    if (!at || isTexty(t)) return;
    if (t.localName === 'select') op({ o: 'set', i: at.i, sel: Array.from(t.selectedOptions, o => o.index) });
    else if (t.type !== 'checkbox' && t.type !== 'radio' && t.type !== 'file') op({ o: 'set', i: at.i, value: t.value });
  }, true);
}
const isTexty = t => t && (t.localName === 'textarea' || (t.localName === 'input' && TEXTY.has((t.getAttribute('type') || '').toLowerCase())));

/* The real field's value comes back as a difference. While the person is typing in it, theirs stays; if the
 * page's is still different 300 ms after the last key (a page that reformats what is typed), it wins. */
function holdProp(n, p) {
  if (n !== typing.el || !('value' in p)) return false;
  typing.held = p;
  clearTimeout(typing.timer);
  typing.timer = setTimeout(applyHeld, Math.max(0, 300 - (performance.now() - typing.at)));
  return true;
}
function applyHeld() {
  const p = typing.held, t = typing.el;
  typing.held = null;
  if (!p || !t) return;
  if (performance.now() - typing.at < 300) { typing.timer = setTimeout(applyHeld, 300); typing.held = p; return; }
  if (t.value !== p.value) { t.value = p.value; typing.value = p.value; }
}

/* An anchor for a scroller: an element in the middle of what it shows that scrolls with it, and how far
 * below the scroller's top edge it is. The real page puts the same element at the same place, so the two
 * agree on what is on screen even where their text wraps differently. */
function anchor(sc) {
  const d = frame.contentDocument, isDoc = sc === d.scrollingElement;
  const box = isDoc ? { left: 0, top: 0, width: doc.w, height: doc.h } : sc.getBoundingClientRect();
  for (const fy of [0.5, 0.3, 0.7, 0.15, 0.85]) {
    for (const e of d.elementsFromPoint(box.left + box.width / 2, box.top + box.height * fy)) {
      if (e === sc || !sc.contains(e) || !mirror.ids.get(e)) continue;
      let fixed = false;
      for (let a = e; a && a !== sc && a.nodeType === 1; a = a.parentNode) {
        const pos = frame.contentWindow.getComputedStyle(a).position;
        if (pos === 'fixed' || pos === 'sticky') { fixed = true; break; }
      }
      if (fixed) continue;
      const r = e.getBoundingClientRect();
      return { a: mirror.ids.get(e), off: Math.round(r.top - box.top), offx: Math.round(r.left - box.left) };
    }
  }
  return {};
}
function sendView(stopped) {
  if (!on || !doc || !frame.contentDocument.scrollingElement) return;
  const d = frame.contentDocument, main = d.scrollingElement, now = performance.now();
  if (stopped) view.moved.add(main);
  for (const sc of view.moved) {
    if (!sc.isConnected) continue;
    const isDoc = sc === main, i = isDoc ? 0 : mirror.ids.get(sc);
    if (i === undefined) continue;
    const m = Object.assign({ o: 'view', i, top: Math.round(sc.scrollTop), left: Math.round(sc.scrollLeft) }, anchor(sc));
    if (isDoc) { /* how fast and which way: the real page is put ahead of a scroll that is still going */
      const dt = now - view.t;
      view.v = dt > 0 && dt < 200 ? (sc.scrollTop - view.y) / dt * 1000 : 0;
      view.y = sc.scrollTop; view.t = now;
      if (!stopped) m.lead = Math.round(Math.max(-doc.h, Math.min(doc.h, view.v * 0.25)));
    }
    const s = JSON.stringify(m);
    if (isDoc && s === view.sent) continue;
    if (isDoc) view.sent = s;
    op(m);
  }
  view.moved.clear();
}
/* the real page moved by itself (a "back to top", an anchor link): follow, unless the person is scrolling */
function pageScrolled(m) {
  if (ptr.size || performance.now() - view.last < 400) return;
  const d = frame.contentDocument, sc = m.i ? mirror.nodes.get(m.i) : d.scrollingElement;
  if (!sc || !sc.scrollTo) return;
  const a = m.a && mirror.nodes.get(m.a);
  if (a && a.getBoundingClientRect) {
    const top = m.i ? sc.getBoundingClientRect().top : 0;
    sc.scrollBy(0, a.getBoundingClientRect().top - top - m.off);
  } else sc.scrollTo(m.left, m.top);
  view.last = 0; /* that scroll event is not the person's */
  view.quiet = performance.now();
}

/* ---------------- on and off ---------------- */
const PageMode = {
  init(a) { api = a; },
  offered(yes) { PageMode.can = !!yes && typeof DecompressionStream === 'function'; if (!yes) PageMode.end(); },
  choose(m) {
    if (!MODES[m]) return;
    mode = m;
    if (api.saveMode) api.saveMode(m);
    PageMode.set(m !== 'video');
    if (api.changed) api.changed(on);
  },
  get mode() { return mode; },
  set mode(m) { if (MODES[m]) mode = m; },
  set(want) {
    wanted = !!want;
    if (want && !PageMode.can) return;
    if (!root) build();
    api.ctrl('mirror.set', { on: wanted });
    if (!want) PageMode.state({ on: false });
  },
  state(d) {
    const was = on;
    on = !!d.on;
    if (on && !root) build();
    if (!on || !was) { video.clear(); videoNow = false; }
    if (root) root.hidden = !on || videoNow;
    document.documentElement.classList.toggle('page-mode', on);
    if (on && !was) { /* the agent opens "dom" as it switches: say hello once it is open */
      parts = [];
      chain = mirror.ready;
      note('Loading the page…');
      let tries = 0;
      const hello = () => {
        const c = api.dom();
        if (!on) return;
        /* pw: how many of this screen's pixels the page is across; no picture is sent wider */
        if (c && c.readyState === 'open') send({ k: 'hello', pw: Math.round(root.clientWidth * Math.min(3, window.devicePixelRatio || 1)) });
        else if (++tries < 100) setTimeout(hello, 50);
        else if (mode === 'page') { note('Page mode is not answering · trying again'); tries = 0; setTimeout(hello, 2000); }
        else PageMode.set(false);
      };
      chain.then(hello);
    }
    if (!on && was && d.why) {
      if (mode === 'page' && wanted) { /* it stopped on the browser's side: ask again, and stay on the page */
        if (root) root.hidden = false;
        note(d.why + ' · trying again');
        setTimeout(() => { if (mode === 'page' && wanted && !on) api.ctrl('mirror.set', { on: true }); }, 3000);
      } else api.toast(d.why + ' · Video mode');
    }
    if (api.changed) api.changed(on);
  },
  data,
  end() { if (on) PageMode.state({ on: false }); },
  get on() { return on; },
  get wanted() { return wanted; },
  can: false,
  log: [] /* the last messages, by kind: for whoever is finding out why the page isn't what it should be */
};
window.PageMode = PageMode;
})();
