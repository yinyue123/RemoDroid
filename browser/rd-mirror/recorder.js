/* recorder.js: Page mode's recorder (browser/dom-mirror.md §5, §6). rd-mirror evaluates this file in an
 * isolated world of the tab in front; the page's own scripts can't see it. It numbers the page's nodes,
 * sends them once (a snapshot) and then only what changes, as patches by node number:
 *
 *   rd.start()   → begins again: the next message is a snapshot
 *   rd.next()    → a promise of a JSON string: the messages since the last call ([] after 20 s of nothing)
 *   rd.poll()    → the same string now, without waiting
 *
 *   {k:'doc', url, title, w, h, sx, sy, n: <html>, as: [css…]}
 *   {k:'diff', o: [ {k:'add', p, n:[node…]}            new children at the end of p
 *                   {k:'kids', p, c:[id | node…]}      p's children are now exactly these, in this order
 *                   {k:'attr', i, n, v | null}
 *                   {k:'text', i, x}
 *                   {k:'css', i, x}                    a stylesheet's rules, as text
 *                   {k:'prop', i, p:{value, checked, sel}}
 *                   {k:'shadow', i, sr}                a shadow root attached to a node already sent
 *                   {k:'gone', i:[id…]} ]              these nodes (and all inside them) no longer exist
 *   {k:'err', what, e}                                 the recorder failed; start() again
 *   {k:'scroll', i, top, left, a, off}                 the page scrolled by itself (i: the scroller, 0 = the
 *                                                      window; a, off: an element in it and how far down it is)
 *
 *   rd.op(json)  → a JSON string {t: [[action, finger, x, y, delay ms]…]}: what the viewer did, by element.
 *                  t are the touches rd-mirror has the agent make (x, y: 0…1 across the window; action 0 down,
 *                  1 up, 2 move, 3 cancel), so the page gets real, trusted touch events
 *     {o:'tap', i, fx, fy, hold}             a tap on node i, at that fraction of its box
 *     {o:'touch', ph, id, i, fx, fy, dx, dy} a finger down on a node, then moved by dx, dy from there
 *     {o:'view', i, a, off, offx, top, left, lead}   where the viewer is looking: scroller i shows node a
 *                                            off pixels below its top edge (lead: ahead by so much)
 *     {o:'set', i, value | sel}              a value picked on the viewer's own control
 *
 *   a node: {i, x} text, or {i, t: tag, ns: 1 svg | 2 mathml, a: {attributes}, c: [children],
 *            x: css (a <style>), u: url (a stylesheet this world may not read), p: {live properties},
 *            s: [scrollLeft, scrollTop], sr: {i, c, as} (an open shadow root)}
 *
 * Never sent: scripts, event-handler attributes, javascript: URLs, <base>, <meta>, where a link or a form
 * goes. Every address of a picture, a font or a stylesheet is made absolute; the viewer never fetches it,
 * it asks rd-mirror for it by that address. */
(() => {
  if (window.rd) return 1;
  const SKIP = new Set(['script', 'noscript', 'base', 'meta', 'template', 'source', 'track', 'param']);
  const DROP = new Set(['nonce', 'integrity', 'srcdoc', 'autofocus', 'srcset', 'sizes', 'loading', 'ping', 'action',
    'formaction', 'target', 'download', 'data', 'crossorigin', 'referrerpolicy', 'fetchpriority', 'decoding']);
  const SVG = 'http://www.w3.org/2000/svg', MATH = 'http://www.w3.org/1998/Math/MathML';
  let ids = new WeakMap(), nodes = new Map(), kids = new Map(), sheets = new Map(), props = new WeakMap();
  let seq = 0, out = [], wake = null, pending = [], timer = 0, tick = 0, polled = 0, fresh = null;
  const observers = [], late = new Set();
  let turns = 0;

  const abs = (u, base) => {
    if (!u || u[0] === '#') return u;
    try {
      const a = new URL(u, base || document.baseURI);
      return /^(https?|data|blob):$/.test(a.protocol) ? a.href : '';
    } catch (e) { return ''; }
  };
  const cssUrls = (s, base) => s.indexOf('url(') < 0 ? s
    : s.replace(/url\(\s*(?:"([^"]*)"|'([^']*)'|([^)\s]*))\s*\)/g, (m, a, b, c) => 'url("' + abs(a || b || c || '', base) + '")');

  /* a stylesheet's rules as they are now (those added with insertRule too); null when this world may not read it */
  const cssText = (sheet, depth) => {
    let rules;
    try { rules = sheet.cssRules; } catch (e) { return null; }
    if (sheet.disabled) return '';
    const base = sheet.href || document.baseURI;
    let s = '';
    for (const r of rules) {
      if (r.type === 3) { /* @import */
        const t = r.styleSheet && depth < 6 ? cssText(r.styleSheet, depth + 1) : null;
        const m = r.media && r.media.mediaText;
        if (t === null) s += '@import url("' + abs(r.href, base) + '")' + (m ? ' ' + m : '') + ';\n';
        else s += m ? '@media ' + m + '{' + t + '}\n' : t;
      } else s += cssUrls(r.cssText, base) + '\n';
    }
    return s;
  };
  const adopted = root => {
    const a = [];
    try { for (const s of root.adoptedStyleSheets || []) a.push(cssText(s, 0) || ''); } catch (e) {}
    return a;
  };
  /* cheap to compute each quarter second: it changes when rules are added or removed from script */
  const sheetSig = sheet => {
    try { return sheet.cssRules.length + (sheet.disabled ? 'd' : ''); } catch (e) { return 'x'; }
  };

  const idOf = n => {
    let i = ids.get(n);
    if (!i || nodes.get(i) !== n) {
      i = ++seq;
      ids.set(n, i);
      nodes.set(i, n);
      if (fresh) fresh.add(i);
    }
    return i;
  };
  const known = n => {
    const i = n && ids.get(n);
    return i && nodes.get(i) === n ? i : 0;
  };

  /* what an attribute is sent as; null = not sent */
  const attr = (el, tag, name, value) => {
    if (name.startsWith('on') || DROP.has(name)) return null;
    if (/^\s*javascript:/i.test(value)) return null;
    if (name === 'href' || name === 'xlink:href') {
      if (tag === 'a' || tag === 'area') return '#';   /* a link stays a link (:link, the cursor); where to isn't sent */
      if (value[0] === '#') return value;               /* <use>, gradients, filters within the document */
      return tag === 'image' ? abs(value) : null;
    }
    if (name === 'src') {
      if (tag === 'img') return el.currentSrc || abs(value);
      return tag === 'input' ? abs(value) : null;       /* frames, video, audio, embed: no address */
    }
    if (name === 'poster' || name === 'background') return abs(value);
    if (name === 'style') return cssUrls(value);
    return value;
  };

  const live = el => {
    const tag = el.localName;
    if (tag === 'input') return el.type === 'checkbox' || el.type === 'radio' ? { checked: el.checked } : { value: el.value };
    if (tag === 'textarea') return { value: el.value };
    if (tag === 'select') return { sel: Array.from(el.selectedOptions, o => o.index) };
    return null;
  };
  const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

  const children = (parent, pid) => {
    const c = [], list = [];
    for (let n = parent.firstChild; n; n = n.nextSibling) {
      const o = ser(n);
      if (o) { c.push(o); list.push(o.i); }
    }
    kids.set(pid, list);
    return c;
  };

  const ser = n => {
    if (n.nodeType === 3) return { i: idOf(n), x: n.data };
    if (n.nodeType !== 1) return null;
    const html = n.namespaceURI !== SVG && n.namespaceURI !== MATH;
    const tag = n.localName;
    if (SKIP.has(tag)) return null;
    if (tag === 'link' && html) { /* a stylesheet travels as its rules; every other <link> stays here */
      if (!n.sheet) return null;
      const o = { i: idOf(n), t: 'style', a: {} }, x = cssText(n.sheet, 0);
      if (n.media) o.a.media = n.media;
      if (x === null) o.u = n.sheet.href; else o.x = x;
      sheets.set(n, sheetSig(n.sheet));
      return o;
    }
    const o = { i: idOf(n), t: html ? tag : n.tagName, a: {} };
    if (!html) o.ns = n.namespaceURI === SVG ? 1 : 2;
    for (const { name, value } of n.attributes) {
      const v = attr(n, tag, name, value);
      if (v !== null) o.a[name] = v;
    }
    if (tag === 'img' && n.currentSrc) o.a.src = n.currentSrc; /* the one the page chose from srcset / <picture> */
    if (tag === 'style' && html) {
      const x = n.sheet ? cssText(n.sheet, 0) : null;
      o.x = x === null ? cssUrls(n.textContent) : x;
      if (n.sheet) sheets.set(n, sheetSig(n.sheet));
      return o;
    }
    if (tag === 'textarea' || tag === 'iframe' || tag === 'frame' || tag === 'object' || tag === 'embed' || tag === 'canvas' ||
        tag === 'video' || tag === 'audio') {
      /* nothing inside is sent: the value, a screenshot or another recorder stands for it (no kids entry) */
    } else o.c = children(n, o.i);
    const p = live(n);
    if (p) { o.p = p; props.set(n, p); }
    if (n.scrollTop || n.scrollLeft) o.s = [n.scrollLeft, n.scrollTop];
    const sr = n.shadowRoot;
    if (sr) {
      const si = idOf(sr);
      o.sr = { i: si, c: children(sr, si), as: adopted(sr) };
      watch(sr);
    }
    return o;
  };

  const watch = root => {
    if (observers.some(o => o.root === root)) return;
    const mo = new MutationObserver(recs => {
      for (const r of recs) pending.push(r);
      if (!timer) timer = setTimeout(flush, 16);
    });
    mo.observe(root, { childList: true, attributes: true, characterData: true, subtree: true });
    observers.push({ root, mo });
  };

  const push = m => {
    out.push(m);
    if (wake) { const w = wake; wake = null; w(take()); }
  };
  const take = () => { polled = Date.now(); return JSON.stringify(out.splice(0)); };

  const forget = id => {
    const list = kids.get(id);
    if (list) for (const c of list) forget(c);
    const n = nodes.get(id);
    if (n && n.shadowRoot) { const s = known(n.shadowRoot); if (s) forget(s); }
    kids.delete(id);
    nodes.delete(id);
  };

  const cssOp = (el, ops) => {
    if (!known(el) || !el.sheet) return;
    sheets.set(el, sheetSig(el.sheet));
    const x = cssText(el.sheet, 0);
    ops.push(x === null ? { k: 'css', i: ids.get(el), u: el.sheet.href } : { k: 'css', i: ids.get(el), x });
  };

  /* an error in here must not end the mirror silently: it is reported, and rd-mirror starts again */
  const guard = (what, fn) => () => {
    try { fn(); } catch (e) { fresh = null; push({ k: 'err', what, e: String(e && e.stack || e) }); }
  };
  const flush = guard('flush', () => {
    timer = 0;
    const recs = pending;
    pending = [];
    const parents = new Set(), at = new Map(), texts = new Set(), css = new Set(), ops = [], lost = new Set();
    fresh = new Set();
    for (const p of late) parents.add(p);
    late.clear();
    for (const r of recs) {
      const t = r.target;
      if (r.type === 'childList') {
        if (sheets.has(t)) css.add(t); /* a <style>'s text was replaced */
        else parents.add(t);
      } else if (r.type === 'attributes') {
        if (t.localName === 'link' && !known(t)) { if (t.parentNode) parents.add(t.parentNode); continue; } /* may be a stylesheet now */
        if (t.localName === 'link' && r.attributeName !== 'media') { css.add(t); continue; }
        let s = at.get(t);
        if (!s) at.set(t, s = new Set());
        s.add(r.attributeName);
      } else if (t.parentNode && sheets.has(t.parentNode)) css.add(t.parentNode);
      else texts.add(t);
    }
    for (const p of parents) {
      const pid = known(p);
      if (!pid || fresh.has(pid) || !p.isConnected || !kids.has(pid)) continue;
      const old = kids.get(pid);
      const c = [], list = [];
      let added = 0;
      for (let n = p.firstChild; n; n = n.nextSibling) {
        const i = known(n);
        if (i) { c.push(i); list.push(i); continue; }
        const o = ser(n);
        if (o) { c.push(o); list.push(o.i); added++; }
      }
      const has = new Set(list);
      for (const i of old) if (!has.has(i)) lost.add(i);
      let prefix = list.length >= old.length;
      for (let k = 0; prefix && k < old.length; k++) prefix = old[k] === list[k];
      kids.set(pid, list);
      if (prefix && added === list.length - old.length) { if (added) ops.push({ k: 'add', p: pid, n: c.slice(old.length) }); }
      else ops.push({ k: 'kids', p: pid, c });
    }
    for (const [el, names] of at) {
      const i = known(el);
      if (!i || fresh.has(i)) continue;
      const tag = el.localName;
      if (tag === 'link') { if (names.has('media')) ops.push({ k: 'attr', i, n: 'media', v: el.media || null }); continue; }
      for (const name of names) {
        const v = el.hasAttribute(name) ? attr(el, tag, name, el.getAttribute(name)) : null;
        if (v === null && el.hasAttribute(name)) continue; /* one that is never sent */
        ops.push({ k: 'attr', i, n: name, v });
      }
      if (tag === 'img' && (names.has('src') || names.has('srcset')) && el.currentSrc) ops.push({ k: 'attr', i, n: 'src', v: el.currentSrc });
    }
    for (const t of texts) {
      const i = known(t);
      if (i && !fresh.has(i)) ops.push({ k: 'text', i, x: t.data });
    }
    for (const el of css) if (!fresh.has(known(el))) cssOp(el, ops);
    const gone = [];
    for (const i of lost) {
      const n = nodes.get(i);
      if (!n) continue;
      const p = n.parentNode;
      if (n.isConnected && known(p) && (kids.get(ids.get(p)) || []).includes(i)) continue; /* moved, not removed */
      forget(i);
      gone.push(i);
    }
    if (gone.length) ops.push({ k: 'gone', i: gone });
    fresh = null;
    if (ops.length) push({ k: 'diff', o: ops });
  });

  /* what no mutation reports: rules added from script, and what the user or the page put into the form fields */
  const compare = guard('compare', () => {
    if (Date.now() - polled > 30000) return stop(); /* nobody is listening any more */
    const ops = [];
    for (const [el, sig] of sheets) {
      if (!el.isConnected || !known(el)) { sheets.delete(el); continue; }
      if (el.sheet && sheetSig(el.sheet) !== sig) cssOp(el, ops);
    }
    if (++turns % 4 === 0) { /* once a second: a shadow root attached to an element that was sent without one */
      const roots = observers.map(o => o.root);
      for (const root of roots) for (const el of root.querySelectorAll('*')) {
        const sr = el.shadowRoot, i = sr && !known(sr) && known(el);
        if (!i || !kids.has(i)) continue;
        const si = idOf(sr);
        ops.push({ k: 'shadow', i, sr: { i: si, c: children(sr, si), as: adopted(sr) } });
        watch(sr);
      }
    }
    for (const el of document.querySelectorAll('input,textarea,select')) {
      const i = known(el);
      if (!i) continue;
      const p = live(el);
      if (!same(p, props.get(el))) { props.set(el, p); ops.push({ k: 'prop', i, p }); }
    }
    if (ops.length) push({ k: 'diff', o: ops });
  });
  const onLoad = e => {
    const el = e.target, i = el && known(el);
    if (!el || el.nodeType !== 1) return;
    /* a picture's address is chosen late (srcset, lazy loading) */
    if (i && el.localName === 'img' && el.currentSrc) push({ k: 'diff', o: [{ k: 'attr', i, n: 'src', v: el.currentSrc }] });
    /* a stylesheet that has arrived: its <link> wasn't sent before, its parent's children are looked at again */
    if (el.localName === 'link' && el.sheet && el.parentNode) {
      if (i) pending.push({ type: 'attributes', target: el, attributeName: 'href' }); else late.add(el.parentNode);
      if (!timer) timer = setTimeout(flush, 16);
    }
  };

  const stop = () => {
    /* a waiter from before (a page back from the back/forward cache still has the one its last poll left,
     * and nobody is listening to it any more) must not be given what comes next */
    if (wake) { const w = wake; wake = null; w('[]'); }
    for (const o of observers) o.mo.disconnect();
    observers.length = 0;
    clearInterval(tick);
    clearTimeout(timer);
    tick = timer = 0;
    document.removeEventListener('load', onLoad, true);
    document.removeEventListener('scroll', onScroll, true);
    pending = [];
    out = [];
  };

  const start = () => {
    stop();
    ids = new WeakMap(); nodes = new Map(); kids = new Map(); sheets = new Map(); props = new WeakMap();
    polled = Date.now();
    const root = document.documentElement;
    const doc = { k: 'doc', url: location.href, title: document.title, w: innerWidth, h: innerHeight, sx: scrollX, sy: scrollY,
               quirks: document.compatMode === 'BackCompat', n: root ? ser(root) : null, as: adopted(document) };
    watch(document);
    tick = setInterval(compare, 250);
    document.addEventListener('load', onLoad, true);
    document.addEventListener('scroll', onScroll, true);
    push(doc); /* whoever waits in next() gets it at once */
    return 1;
  };

  /* ---------------- the viewer's operations ---------------- */
  const fingers = new Map(); /* finger → where it went down, in the window */
  let ours = 0;              /* until when a scroll is one we made */
  const frac = (x, y) => [Math.min(0.999, Math.max(0.001, x / innerWidth)), Math.min(0.999, Math.max(0.001, y / innerHeight))];
  /* a place in a node's box → a place in the window, the node scrolled into view if it has to be */
  const place = m => {
    const el = nodes.get(m.i);
    if (!el || el.nodeType !== 1 || !el.isConnected) return null;
    const at = () => { const r = el.getBoundingClientRect(); return [r.left + m.fx * r.width, r.top + m.fy * r.height]; };
    let p = at();
    if (p[0] < 0 || p[1] < 0 || p[0] > innerWidth || p[1] > innerHeight) {
      ours = Date.now() + 300;
      el.scrollIntoView({ block: 'center', inline: 'nearest', behavior: 'instant' });
      p = at();
    }
    return p;
  };
  const scrollerBox = (sc, isDoc) => isDoc ? { top: 0, left: 0, width: innerWidth, height: innerHeight } : sc.getBoundingClientRect();
  const op = json => {
    const m = JSON.parse(json), t = [];
    if (m.o === 'tap') {
      const p = place(m);
      if (p) { const f = frac(p[0], p[1]); t.push([0, 0, f[0], f[1], 0], [1, 0, f[0], f[1], Math.min(900, Math.max(40, m.hold | 0))]); }
    } else if (m.o === 'touch') {
      const id = m.id & 7;
      if (m.ph === 'down') {
        const p = place(m);
        if (p) { fingers.set(id, p); const f = frac(p[0], p[1]); t.push([0, id, f[0], f[1], 0]); }
      } else if (fingers.has(id)) {
        const b = fingers.get(id), f = frac(b[0] + (m.dx || 0), b[1] + (m.dy || 0));
        t.push([m.ph === 'move' ? 2 : m.ph === 'up' ? 1 : 3, id, f[0], f[1], 0]);
        if (m.ph !== 'move') fingers.delete(id);
      }
    } else if (m.o === 'view') {
      const isDoc = !m.i, sc = isDoc ? document.scrollingElement : nodes.get(m.i), a = m.a && nodes.get(m.a);
      if (sc && sc.nodeType === 1) {
        ours = Date.now() + 250;
        if (a && a.nodeType === 1 && a.isConnected) {
          const box = scrollerBox(sc, isDoc), r = a.getBoundingClientRect();
          const dy = r.top - box.top - m.off + (m.lead || 0), dx = r.left - box.left - (m.offx || 0);
          if (Math.abs(dy) >= 1 || Math.abs(dx) >= 1) sc.scrollBy({ left: dx, top: dy, behavior: 'instant' });
        } else sc.scrollTo({ left: m.left || 0, top: (m.top || 0) + (m.lead || 0), behavior: 'instant' });
      }
    } else if (m.o === 'set') {
      const el = nodes.get(m.i);
      if (el && el.nodeType === 1 && el.isConnected) {
        if (m.sel && el.options) for (const o of el.options) o.selected = m.sel.includes(o.index);
        else if ('value' in m) el.value = m.value;
        props.set(el, live(el)); /* ours: not sent back as the page's */
        el.dispatchEvent(new Event('input', { bubbles: true }));
        el.dispatchEvent(new Event('change', { bubbles: true }));
      }
    }
    return JSON.stringify({ t });
  };
  /* the page scrolled by itself (a "back to top", an anchor link, a script): the viewer follows */
  let scrolled = null, scrollTimer = 0;
  const onScroll = e => {
    if (Date.now() < ours) return;
    (scrolled = scrolled || new Set()).add(e.target === document ? document.scrollingElement : e.target);
    if (!scrollTimer) scrollTimer = setTimeout(guard('scroll', () => {
      scrollTimer = 0;
      const set = scrolled;
      scrolled = null;
      if (Date.now() < ours) return;
      for (const sc of set) {
        if (!sc || sc.nodeType !== 1 || !sc.isConnected) continue;
        const isDoc = sc === document.scrollingElement, i = isDoc ? 0 : known(sc);
        if (i === 0 && !isDoc) continue;
        const m = { k: 'scroll', i, top: Math.round(sc.scrollTop), left: Math.round(sc.scrollLeft) };
        const box = scrollerBox(sc, isDoc);
        for (const e2 of document.elementsFromPoint(box.left + box.width / 2, box.top + box.height / 2)) {
          if (e2 === sc || !sc.contains(e2) || !known(e2)) continue;
          const pos = getComputedStyle(e2).position;
          if (pos === 'fixed' || pos === 'sticky') continue;
          m.a = known(e2);
          m.off = Math.round(e2.getBoundingClientRect().top - box.top);
          break;
        }
        push(m);
      }
    }), 60);
  };

  window.rd = {
    op,
    start,
    stop,
    poll: () => take(), /* what there is, without waiting */
    next: () => new Promise(r => {
      if (wake) { const w = wake; wake = null; w('[]'); } /* one waiter: an earlier call gets nothing, never the news */
      if (out.length) return r(take());
      polled = Date.now();
      wake = r;
      setTimeout(() => { if (wake === r) { wake = null; r(take()); } }, 20000);
    })
  };
  return 1;
})()
