/* mirror.js: Page mode's applier (browser/dom-mirror.md §10). It rebuilds the remote browser's page inside
 * a sandboxed frame from the recorder's snapshot (browser/rd-mirror/recorder.js) and patches it with each
 * difference, by node number.
 *
 *   const m = new Mirror(frame, send);     frame: an <iframe>; send(msg): to rd-mirror ({k:'res.get', u})
 *   await m.ready;
 *   m.on(msg);                             a 'doc' or a 'diff' from the recorder
 *   m.resource(url, blob | null);          the answer to a res.get
 *
 * Two locks keep the site off this device: the frame can't run scripts (sandbox without allow-scripts; in
 * Safari, where that would silence the viewer's own listeners too, its Content-Security-Policy, which has
 * no script-src) and can't fetch (the policy allows blob: and data: only). So even a <script> or an address
 * the recorder let through would do nothing here. Pictures, fonts and unreadable stylesheets are asked for
 * by their address and arrive as bytes. */
'use strict';
class Mirror {
  constructor(frame, send) {
    this.frame = frame;
    this.send = send;
    this.nodes = new Map();      /* number → node */
    this.ids = new WeakMap();    /* node → number */
    this.res = new Map();        /* address → blob: URL, '' (asked for), or null (there is none) */
    this.waits = new Map();      /* address → the callbacks that want it */
    this.css = new Map();        /* <style> → { raw, base }: its rules before the addresses are swapped */
    this.dirty = new Set();      /* <style>s to write again, once a frame */
    this.uses = new WeakMap();   /* <use> → the address it stands for, while its file of icons is on its way */
    this.sprites = new Map();    /* a file of icons' address → { pre, node }: its copy in this document */
    this.view = { w: 0, h: 0 };
    const load = sandbox => new Promise(r => {
      frame.setAttribute('sandbox', sandbox);
      frame.addEventListener('load', () => r(), { once: true });
      frame.srcdoc = '<!doctype html><html><head><meta http-equiv="Content-Security-Policy" content="' + Mirror.CSP +
        '"></head><body></body></html>';
    });
    /* Safari runs nobody's listeners in a frame that may not run scripts, the viewer's own included: a tap,
     * a key, a scroll in the mirror would reach no one. There the frame is allowed scripts, and the policy
     * alone refuses every one of the page's (inline, handlers, javascript: addresses, eval). */
    this.ready = load('allow-same-origin').then(() => {
      let heard = false;
      const d = frame.contentDocument, h = () => { heard = true; };
      d.addEventListener('rd-probe', h);
      d.dispatchEvent(new Event('rd-probe'));
      d.removeEventListener('rd-probe', h);
      if (!heard) return load('allow-same-origin allow-scripts');
    });
  }

  on(m) {
    if (m.k === 'doc') this.doc(m);
    else if (m.k === 'diff') for (const o of m.o) {
      try { this.op(o); } catch (e) { console.warn('mirror: ' + o.k, e); }
    }
    this.keep();
  }

  /* A file of icons (an SVG whose parts <use> elements point at by id). A frame that can't fetch can't follow
   * such an address, so the file's elements are copied into this document, out of sight, their ids given a
   * prefix of their own (the page's ids stay the page's). → the prefix, or '' if there is no such file. */
  icons(file) {
    let s = this.sprites.get(file);
    if (s) return s.pre;
    const blob = this.blobs && this.blobs.get(file);
    if (!blob) return Promise.resolve('');
    const pre = 'rd-i' + (this.sprites.size + 1) + '-';
    this.sprites.set(file, s = { node: null, pre: blob.text().then(text => {
      const d = this.frame.contentDocument, src = new DOMParser().parseFromString(text, 'image/svg+xml').documentElement;
      if (!src || src.localName !== 'svg') return '';
      const box = d.importNode(src, true);
      for (const e of [box, ...box.querySelectorAll('*')]) {
        if (Mirror.NEVER.has(e.localName.toLowerCase()) || e.localName === 'foreignObject') { e.remove(); continue; }
        for (const a of Array.from(e.attributes)) {
          if (/^on/i.test(a.name)) e.removeAttribute(a.name);
          else if (a.name === 'id') e.setAttribute('id', pre + a.value);
          else if (a.localName === 'href') a.value[0] === '#' ? e.setAttribute(a.name, '#' + pre + a.value.slice(1)) : e.removeAttribute(a.name);
          else if (a.value.indexOf('url(') >= 0) e.setAttribute(a.name, a.value.replace(/url\(\s*["']?#([^)"']+)["']?\s*\)/g, 'url(#' + pre + '$1)'));
        }
      }
      box.setAttribute('style', 'position:absolute;width:0;height:0;overflow:hidden');
      box.setAttribute('aria-hidden', 'true');
      s.node = box;
      this.keep();
      return pre;
    }).catch(() => '') });
    return s.pre;
  }

  /* the copies stay in the document through a new snapshot and whatever the page does to its own elements */
  keep() {
    const root = this.frame.contentDocument.documentElement;
    if (root) for (const s of this.sprites.values()) if (s.node && s.node.parentNode !== root) root.appendChild(s.node);
  }

  doc(m) {
    const d = this.frame.contentDocument;
    this.nodes.clear();
    this.ids = new WeakMap();
    this.css.clear();
    this.dirty.clear();
    this.view = { w: m.w, h: m.h };
    this.url = m.url;
    while (d.firstChild) d.removeChild(d.firstChild);
    d.appendChild(d.implementation.createDocumentType('html', '', ''));
    const root = m.n ? this.build(m.n) : d.createElement('html');
    d.appendChild(root);
    for (const x of m.as || []) this.sheet(d.head || root, x);
    this.scrolls(m.n);
    if (d.scrollingElement) d.scrollingElement.scrollTo(m.sx || 0, m.sy || 0);
    this.frame.style.width = m.w + 'px';
    this.frame.style.height = m.h + 'px';
  }

  /* a stylesheet that isn't an element on the real page (adoptedStyleSheets) */
  sheet(parent, text) {
    const s = parent.ownerDocument.createElement('style');
    this.css.set(s, { raw: text, base: this.url });
    s.textContent = this.cssOut(s);
    parent.appendChild(s);
  }

  build(o) {
    const d = this.frame.contentDocument;
    let n;
    if (o.t === undefined) n = d.createTextNode(o.x);
    else {
      const tag = Mirror.NEVER.has(String(o.t).toLowerCase()) ? 'rd-none' : o.t;
      try { n = d.createElementNS(Mirror.NS[o.ns | 0], tag); } catch (e) { n = d.createElement('rd-none'); }
      for (const k in o.a) this.attr(n, k, o.a[k]);
      if (o.t === 'style') this.setCss(n, o);
      if (o.c) for (const c of o.c) n.appendChild(this.build(c));
      if (o.sr) this.shadow(n, o.sr);
      if (o.p) this.prop(n, o.p);
    }
    this.nodes.set(o.i, n);
    this.ids.set(n, o.i);
    return n;
  }

  shadow(n, o) {
    try {
      const sr = n.shadowRoot || n.attachShadow({ mode: 'open' });
      while (sr.firstChild) { this.forget(sr.firstChild); sr.removeChild(sr.firstChild); }
      this.nodes.set(o.i, sr);
      this.ids.set(sr, o.i);
      for (const c of o.c) sr.appendChild(this.build(c));
      for (const x of o.as || []) this.sheet(sr, x);
    } catch (e) {} /* an element that can't have one here */
  }

  /* scroll positions can only be set once the nodes are in the document and have a size */
  scrolls(o) {
    if (!o || o.t === undefined) return;
    if (o.s) { const n = this.nodes.get(o.i); if (n && n.scrollTo) n.scrollTo(o.s[0], o.s[1]); }
    if (o.c) for (const c of o.c) this.scrolls(c);
    if (o.sr) for (const c of o.sr.c) this.scrolls(c);
  }

  attr(n, name, v) {
    if (/^on/i.test(name) || name === 'srcdoc' || name === 'sandbox') return;
    const tag = n.localName;
    if (name === 'src' && tag !== 'img' && tag !== 'input') return;
    try {
      if (v === null) n.removeAttribute(name);
      else if (name === 'src' || name === 'poster' || name === 'background' || ((name === 'href' || name === 'xlink:href') && tag === 'image')) {
        n.removeAttribute(name); /* until the bytes are here */
        this.want(v, u => { if (u) n.setAttribute(name, u); });
      } else if ((name === 'href' || name === 'xlink:href') && tag === 'use' && /^https?:/i.test(v)) {
        /* an icon in a file of icons: the file is fetched as bytes, put into this document, and pointed at there */
        const at = v.indexOf('#'), file = at < 0 ? v : v.slice(0, at);
        n.removeAttribute(name);
        this.uses.set(n, v);
        this.want(file, () => this.icons(file).then(pre => {
          if (pre && this.uses.get(n) === v) n.setAttribute(name, '#' + pre + (at < 0 ? '' : v.slice(at + 1)));
        }), true);
      } else if (name === 'href' || name === 'xlink:href') {
        this.uses.delete(n);
        n.setAttribute(name, v[0] === '#' ? v : '#');
      } else if (name === 'style') n.setAttribute(name, this.swap(v, this.url, () => {
        if (n.isConnected) n.setAttribute(name, this.swap(v, this.url, null));
      }));
      else n.setAttribute(name, v);
    } catch (e) {} /* a name this document won't take */
  }

  prop(n, p) {
    if (this.hold && this.hold(n, p)) return; /* the person is typing in it (pagemode.js) */
    if ('value' in p && n.value !== p.value) n.value = p.value;
    if ('checked' in p) n.checked = p.checked;
    if (p.sel && n.options) for (const o of n.options) o.selected = p.sel.includes(o.index);
  }

  /* an address → cb(blob: URL or '') now if it is here, else later; asks for it once. load: the browser may
   * load it again if it kept no copy for the page (a file of icons; never a picture the page hasn't loaded) */
  want(u, cb, load) {
    if (!u) return cb('');
    if (/^data:/i.test(u)) return cb(u);
    if (!/^https?:/i.test(u)) return cb('');
    const have = this.res.get(u);
    if (have || have === null) return cb(have || '');
    let w = this.waits.get(u);
    if (!w) this.waits.set(u, w = []);
    w.push(cb);
    if (have === undefined) { this.res.set(u, ''); this.send(load ? { k: 'res.get', u, load: true } : { k: 'res.get', u }); }
  }

  resource(u, blob) {
    const url = blob ? URL.createObjectURL(blob) : null;
    this.res.set(u, url);
    this.blobs = this.blobs || new Map();
    if (blob) this.blobs.set(u, blob);
    const w = this.waits.get(u) || [];
    this.waits.delete(u);
    for (const cb of w) cb(url || '');
  }

  /* CSS text with every url(…) swapped for the blob: it stands for; later() is called when one arrives */
  swap(text, base, later) {
    if (text.indexOf('url(') < 0) return text;
    return text.replace(/url\(\s*(?:"([^"]*)"|'([^']*)'|([^)\s]*))\s*\)/g, (m, a, b, c) => {
      let u = a || b || c || '';
      if (u[0] === '#') return m;
      try { u = new URL(u, base).href; } catch (e) { return 'url("data:,")'; }
      if (/^data:/i.test(u)) return 'url("' + u + '")';
      let got = this.res.get(u);
      if (got === undefined || (got === '' && later)) this.want(u, () => { if (later) later(); });
      return 'url("' + (got || 'data:,') + '")';
    });
  }

  setCss(n, o) {
    if (o.u) { /* a stylesheet the recorder may not read: its text is fetched like a picture */
      this.css.set(n, { raw: '', base: o.u });
      this.want(o.u, () => {
        const b = this.blobs && this.blobs.get(o.u);
        if (b) b.text().then(t => { const c = this.css.get(n); if (c && c.base === o.u) { c.raw = t; this.restyle(n); } });
      });
    } else this.css.set(n, { raw: o.x || '', base: this.url });
    n.textContent = this.cssOut(n);
  }

  cssOut(n) {
    const c = this.css.get(n);
    /* @import can't be followed here (no network): each is fetched as bytes and put in its place */
    const raw = c.raw.replace(/@import\s+(?:url\(\s*)?["']?([^"')\s;]+)["']?\s*\)?([^;]*);/g, (m, u, media) => {
      let a;
      try { a = new URL(u, c.base).href; } catch (e) { return ''; }
      c.imports = c.imports || new Map();
      if (!c.imports.has(a)) {
        c.imports.set(a, '');
        this.want(a, () => {
          const b = this.blobs && this.blobs.get(a);
          if (b) b.text().then(t => { c.imports.set(a, this.rebase(t, a)); this.restyle(n); });
        });
      }
      const t = c.imports.get(a);
      return media.trim() && t ? '@media ' + media.trim() + '{' + t + '}' : t;
    });
    return this.swap(raw, c.base, () => this.restyle(n));
  }

  /* a fetched stylesheet's addresses are relative to it, not to the page */
  rebase(text, base) {
    return text.replace(/url\(\s*(?:"([^"]*)"|'([^']*)'|([^)\s]*))\s*\)/g, (m, a, b, c) => {
      const u = a || b || c || '';
      if (u[0] === '#' || /^data:/i.test(u)) return m;
      try { return 'url("' + new URL(u, base).href + '")'; } catch (e) { return 'url("data:,")'; }
    });
  }

  restyle(n) {
    if (!this.dirty.size) (this.frame.contentWindow || window).requestAnimationFrame(() => {
      for (const s of this.dirty) if (this.css.has(s)) s.textContent = this.cssOut(s);
      this.dirty.clear();
    });
    this.dirty.add(n);
  }

  forget(n) {
    const i = this.ids.get(n);
    if (i) this.nodes.delete(i);
    this.css.delete(n);
    if (n.shadowRoot) this.forget(n.shadowRoot);
    for (let c = n.firstChild; c; c = c.nextSibling) this.forget(c);
  }

  op(o) {
    const n = this.nodes.get(o.k === 'add' || o.k === 'kids' ? o.p : o.i);
    switch (o.k) {
    case 'add':
      if (n) for (const c of o.n) n.appendChild(this.build(c));
      break;
    case 'kids': {
      if (!n) break;
      let cur = n.firstChild;
      for (const c of o.c) {
        const node = typeof c === 'number' ? this.nodes.get(c) : this.build(c);
        if (!node) continue;
        if (node === cur) cur = cur.nextSibling;
        else n.insertBefore(node, cur);
      }
      while (cur) { const next = cur.nextSibling; n.removeChild(cur); cur = next; } /* moved elsewhere, or in 'gone' */
      break;
    }
    case 'attr': if (n) this.attr(n, o.n, o.v); break;
    case 'text': {
      if (!n) break;
      /* a caret in this text (a box the person is typing in, pagemode.js) stays as far from its end as it was */
      const s = n.ownerDocument.getSelection(), end = s && s.isCollapsed && s.anchorNode === n ? n.data.length - s.anchorOffset : -1;
      n.data = o.x;
      if (end >= 0) s.collapse(n, Math.max(0, n.data.length - end));
      break;
    }
    case 'css': if (n) this.setCss(n, o); break;
    case 'prop': if (n) this.prop(n, o.p); break;
    case 'shadow': if (n) this.shadow(n, o.sr); break;
    case 'gone':
      for (const i of o.i) {
        const g = this.nodes.get(i);
        if (!g) continue;
        if (g.parentNode) g.parentNode.removeChild(g);
        this.forget(g);
      }
      break;
    }
  }
}
Mirror.NS = ['http://www.w3.org/1999/xhtml', 'http://www.w3.org/2000/svg', 'http://www.w3.org/1998/Math/MathML'];
Mirror.NEVER = new Set(['script', 'base', 'meta', 'link', 'object', 'embed', 'applet', 'noscript', 'frame', 'frameset']);
Mirror.CSP = "default-src 'none'; style-src 'unsafe-inline'; img-src blob: data:; font-src blob: data:; media-src 'none'; " +
  "form-action 'none'; base-uri 'none'";
if (typeof window !== 'undefined') window.Mirror = Mirror;
