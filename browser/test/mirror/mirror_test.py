# The recorder and the applier against each other (browser/dom-mirror.md T1, T3): a real page in one tab,
# its mirror in another, and after every change the two trees compared node for node.
#   python3 -I mirror_test.py [browser binary] [--shots dir]     (default google-chrome)
import base64, zlib, functools, http.server, json, os, subprocess, sys, tempfile, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
argv = sys.argv[1:]
SHOTS = None
if '--shots' in argv:
    i = argv.index('--shots'); SHOTS = argv[i + 1]; del argv[i:i + 2]
URL = None
if '--url' in argv:   # one real page instead of the test page: sizes, a tree comparison and both screenshots
    i = argv.index('--url'); URL = argv[i + 1]; del argv[i:i + 2]
BROWSER = argv[0] if argv else 'google-chrome'

class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
    def end_headers(self):
        self.send_header('Cache-Control', 'max-age=600')
        super().end_headers()
viewer = tempfile.mkdtemp()
for src, dst in ((HERE + '/pages/host.html', 'host.html'), (REPO + '/web/mirror.js', 'mirror.js')):
    open(os.path.join(viewer, dst), 'wb').write(open(src, 'rb').read())
for port, d in ((8471, HERE + '/pages'), (8472, HERE + '/pages/x'), (8473, viewer)):
    s = http.server.ThreadingHTTPServer(('127.0.0.1', port), functools.partial(Quiet, directory=d))
    threading.Thread(target=s.serve_forever, daemon=True).start()

to_r, to_w = os.pipe(); from_r, from_w = os.pipe()
def pre():  # DevTools on fds 3 and 4; the pipes may already sit on those numbers, so copy them away first
    a = os.dup(os.dup(os.dup(to_r))); b = os.dup(os.dup(os.dup(from_w)))
    os.dup2(a, 3); os.dup2(b, 4)
proc = subprocess.Popen([BROWSER, '--headless=new', '--no-sandbox', '--remote-debugging-pipe', '--user-data-dir=' + tempfile.mkdtemp(),
                         '--no-first-run', '--disable-gpu', '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
                         '--disable-backgrounding-occluded-windows', '--window-size=440,900', 'about:blank'],
                        preexec_fn=pre, close_fds=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
os.close(to_r); os.close(from_w)
buf = b''; nid = 0; events = []
def read():
    global buf
    while b'\0' not in buf:
        d = os.read(from_r, 1 << 16)
        if not d: raise SystemExit('the browser closed the pipe')
        buf += d
    m, buf = buf.split(b'\0', 1)
    return json.loads(m)
def call(method, params=None, sid=None):
    global nid
    nid += 1
    m = {'id': nid, 'method': method, 'params': params or {}}
    if sid: m['sessionId'] = sid
    os.write(to_w, json.dumps(m).encode() + b'\0')
    while True:
        r = read()
        if r.get('id') == nid:
            if 'error' in r: raise RuntimeError(method + ': ' + r['error']['message'])
            return r['result']
        events.append(r)
def ev(sid, expr, ctx=None, wait=False):
    p = {'expression': expr, 'awaitPromise': wait, 'returnByValue': True}
    if ctx: p['contextId'] = ctx
    r = call('Runtime.evaluate', p, sid)
    if 'exceptionDetails' in r:
        raise RuntimeError(r['exceptionDetails'].get('exception', {}).get('description', str(r['exceptionDetails'])))
    return r['result'].get('value')
def tab(url):
    t = call('Target.createTarget', {'url': 'about:blank'})['targetId']
    sid = call('Target.attachToTarget', {'targetId': t, 'flatten': True})['sessionId']
    call('Page.enable', {}, sid)
    call('Page.navigate', {'url': url}, sid)
    for _ in range(300):
        time.sleep(0.05)
        if ev(sid, 'document.readyState') == 'complete': break
    return t, sid

SIG = r'''(doc => {
  const SKIP = new Set(['script', 'noscript', 'base', 'meta', 'template', 'source', 'track', 'param']);
  const SEAL = new Set(['textarea', 'iframe', 'frame', 'object', 'embed', 'canvas', 'video', 'audio', 'style']);
  const NOT = /^(on.*|href|xlink:href|src|srcset|sizes|style|poster|background|nonce|integrity|srcdoc|autofocus|loading|ping|action|formaction|target|download|data|crossorigin|referrerpolicy|fetchpriority|decoding)$/;
  const out = [];
  const walk = (n, d) => {
    for (let c = n.firstChild; c; c = c.nextSibling) {
      if (c.nodeType === 3) { out.push(d + ' #' + c.data); continue; }
      if (c.nodeType !== 1) continue;
      const tag = c.localName;
      if (SKIP.has(tag)) continue;
      if (tag === 'link') { if (c.sheet) out.push(d + ' style'); continue; }
      let s = d + ' ' + tag;
      if (!(tag === 'style' && !c.id && !c.attributes.length))
        s += ' ' + Array.from(c.attributes).filter(a => !NOT.test(a.name)).map(a => a.name + '=' + a.value).sort().join(',');
      if ('value' in c && tag !== 'li' && tag !== 'option') s += ' value=' + c.value;
      if (c.checked !== undefined) s += ' checked=' + c.checked;
      out.push(s.trimEnd());
      if (!SEAL.has(tag)) walk(c, d + 1);
      if (c.shadowRoot) { out.push(d + ' (shadow)'); walk(c.shadowRoot, d + 1); }
    }
  };
  walk(doc, 0);
  return out;
})'''
STYLE = r'''(doc => {
  const o = {}, w = doc.defaultView;
  for (const id of ['h', 'p', 'lnk', 'bg', 'kept']) {
    const e = doc.getElementById(id);
    if (e) o[id] = w.getComputedStyle(e).color + ' ' + w.getComputedStyle(e).backgroundImage.replace(/url\("(\w+):[^"]*"\)/, 'url($1)');
  }
  const em = doc.querySelector('em'); if (em) o.em = w.getComputedStyle(em).color;
  const inner = doc.getElementById('host') && doc.getElementById('host').shadowRoot;
  if (inner) o.inner = w.getComputedStyle(inner.querySelector('span')).color;
  const im = doc.getElementById('im'); if (im) o.im = im.naturalWidth + 'x' + im.naturalHeight;
  return o;
})'''

print(call('Browser.getVersion')['product'])
ta, A = tab(URL or 'http://127.0.0.1:8471/page.html')
tb, B = tab('http://127.0.0.1:8473/host.html')
call('Network.enable', {}, B)
ctx = call('Page.createIsolatedWorld', {'frameId': ta, 'worldName': 'rd'}, A)['executionContextId']
ev(A, open(REPO + '/browser/rd-mirror/recorder.js').read(), ctx)
ev(B, 'M.ready.then(() => 1)', wait=True)
ev(A, 'rd.start()', ctx)
mimes = {}
sent = {'bytes': 0, 'msgs': 0, 'res': 0}
def pump():
    for _ in range(40):
        time.sleep(0.06)
        s = ev(A, 'rd.poll()', ctx)
        busy = s != '[]'
        if busy:
            sent['bytes'] += len(s); sent['msgs'] += 1; sent['z'] = sent.get('z', 0) + len(zlib.compress(s.encode(), 6))
            for m in json.loads(s):
                if m['k'] == 'err': print('   recorder error in', m['what'], m['e'][:600])
            ev(B, 'feed(%s)' % json.dumps(s))
        for m in json.loads(ev(B, 'JSON.stringify(Q.splice(0))')):
            busy = True
            if not mimes:
                tree = call('Page.getResourceTree', {}, A)['frameTree']
            else:
                tree = None
            if m['u'] not in mimes:
                for r in call('Page.getResourceTree', {}, A)['frameTree'].get('resources', []): mimes[r['url']] = r['mimeType']
            try:
                if m['u'] not in mimes: raise RuntimeError('not a resource of the page')
                r = call('Page.getResourceContent', {'frameId': ta, 'url': m['u']}, A)
                b64 = r['content'] if r['base64Encoded'] else base64.b64encode(r['content'].encode()).decode()
                sent['res'] += len(b64) * 3 // 4
                ev(B, 'give(%s, %s, %s)' % (json.dumps(m['u']), json.dumps(b64), json.dumps(mimes[m['u']])))
            except RuntimeError as e:
                print('   no resource', m['u'], '-', e)
                ev(B, 'give(%s, null)' % json.dumps(m['u']))
        if not busy: return
fails = 0
def check(name):
    global fails
    for _ in range(8):   # the mirror may be up to a second behind: a shadow root on a node already sent is looked for once a second
        pump()
        a = ev(A, SIG + '(document)', ctx)
        b = ev(B, SIG + '(document.getElementById("f").contentDocument)')
        if a == b: break
        time.sleep(0.2)
    if a == b:
        print('ok   %-28s %d nodes' % (name, len(a)))
        return
    fails += 1
    print('FAIL %-28s real %d, mirror %d' % (name, len(a), len(b)))
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else '(none)'; y = b[i] if i < len(b) else '(none)'
        if x != y:
            print('     first difference at line %d:\n       real   %r\n       mirror %r' % (i, x[:120], y[:120]))
            break
def shot(sid, name):
    if SHOTS:
        os.makedirs(SHOTS, exist_ok=True)
        call('Page.bringToFront', {}, sid)   # a tab behind another never gets a frame to capture
        time.sleep(0.2)
        open(os.path.join(SHOTS, name), 'wb').write(base64.b64decode(call('Page.captureScreenshot', {'format': 'png'}, sid)['data']))

check('snapshot')
if URL:
    time.sleep(2); check('2 s later')
    shot(A, 'real.png'); shot(B, 'mirror.png')
    print('sent: %d messages, %d bytes of JSON (%d deflated), %d bytes of resources' % (sent['msgs'], sent['bytes'], sent.get('z', 0), sent['res']))
    call('Browser.close'); proc.wait(timeout=10)
    sys.exit(1 if fails else 0)
shot(A, 'real-0.png'); shot(B, 'mirror-0.png')
n = ev(A, 'steps.length')
for i in range(n):
    ev(A, 'steps[%d](), 1' % i)
    time.sleep(0.25)
    check('step %d' % i)
time.sleep(0.5); pump()
sa = ev(A, STYLE + '(document)', ctx); sb = ev(B, STYLE + '(document.getElementById("f").contentDocument)')
for k in sa:
    ok = sa[k].replace('url(http)', 'url(blob)') == sb.get(k)
    fails += not ok
    print('%s style %-6s real %-40s mirror %s' % ('ok  ' if ok else 'FAIL', k, sa[k], sb.get(k)))
shot(A, 'real-end.png'); shot(B, 'mirror-end.png')

# T3: the viewer's tab never asked the network for anything of the page's, and nothing of the page ran there
leaks = [e['params']['request']['url'] for e in events if e.get('method') == 'Network.requestWillBeSent' and e.get('sessionId') == B
         and not e['params']['request']['url'].startswith(('http://127.0.0.1:8473/', 'blob:', 'data:', 'about:'))]
bad = ev(B, '''(() => { const d = document.getElementById("f").contentDocument, w = document.getElementById("f").contentWindow;
  return { scripts: d.querySelectorAll("script").length, handlers: d.querySelectorAll("[onclick],[onload]").length,
           evil: typeof w.evil, secret: typeof w.pageSecret, loaded: typeof w.loaded,
           links: Array.from(d.querySelectorAll("a"), a => a.getAttribute("href")).filter(h => h && h !== "#"),
           forms: Array.from(d.querySelectorAll("form[action]")).length,
           http: d.documentElement.outerHTML.replace(/blob:http[^"') &]+/g, "").match(/https?:\\/\\/[^"') ]+/g) || [] }; })()''')
print('requests from the viewer for the page:', leaks or 'none')
print('in the mirror:', bad)
fails += bool(leaks) + bool(bad['scripts'] or bad['handlers'] or bad['links'] or bad['forms'] or bad['http']) + (bad['evil'] != 'undefined')
print('sent: %d messages, %d bytes of JSON (%d deflated), %d bytes of resources' % (sent['msgs'], sent['bytes'], sent.get('z', 0), sent['res']))
call('Browser.close'); proc.wait(timeout=10)
print('FAILED: %d' % fails if fails else 'all passed')
sys.exit(1 if fails else 0)
