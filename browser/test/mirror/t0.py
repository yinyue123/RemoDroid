# Step 0 of browser/dom-mirror.md: the DevTools calls Page mode rests on, with and without Runtime.enable.
#   python3 -I t0.py [browser binary]      (default google-chrome; writes its test pages to a temp directory)
import json, os, subprocess, sys, time, tempfile, threading, http.server, functools, base64

def pages(d):
    import struct, zlib
    raw = b''.join(b'\0' + b''.join(bytes([(x * 7) % 256, (y * 5) % 256, 90]) for x in range(300)) for y in range(200))
    ch = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    open(d + '/pic.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', 300, 200, 8, 2, 0, 0, 0)) +
                                     ch(b'IDAT', zlib.compress(raw)) + ch(b'IEND', b''))
    open(d + '/x.css', 'w').write('.x{color:rgb(1,2,3);background:url(pic.png)}')      # served cross-origin (port 8472)
    open(d + '/same.css', 'w').write('.same{color:red}')
    open(d + '/index.html', 'w').write("""<!doctype html><title>t0</title>
<link rel=stylesheet href="same.css"><link rel=stylesheet href="http://127.0.0.1:8472/x.css">
<style id=s>.a{color:blue}</style>
<div id=box class=x style="width:300px;height:200px">hello</div><img id=im src="http://127.0.0.1:8472/pic.png">
<ul id=list></ul><div id=probe>no</div>
<script>
document.getElementById('s').sheet.insertRule('.injected{color:green}',0);
let n=0; setInterval(()=>{const li=document.createElement('li');li.textContent='item '+(n++);list.appendChild(li);},300);
window.pageSecret=1;
function probe(){const e=new Error();Object.defineProperty(e,'stack',{get(){document.getElementById('probe').textContent='DETECTED';return ''}});console.debug(e);}
setInterval(probe,200);
</script>""")
BROWSER = sys.argv[1] if len(sys.argv) > 1 else 'google-chrome'
WWW = tempfile.mkdtemp(); pages(WWW)
class Q(http.server.SimpleHTTPRequestHandler):
    def log_message(self,*a): pass
for p in (8471, 8472):
    s = http.server.ThreadingHTTPServer(('127.0.0.1', p), functools.partial(Q, directory=WWW)); threading.Thread(target=s.serve_forever, daemon=True).start()
to_r, to_w = os.pipe(); from_r, from_w = os.pipe()
prof = tempfile.mkdtemp()
def pre():
    a = os.dup(to_r); b = os.dup(from_w); a2 = os.dup(a); b2 = os.dup(b)
    os.dup2(a2, 3); os.dup2(b2, 4)
p = subprocess.Popen([BROWSER, '--headless=new', '--no-sandbox', '--remote-debugging-pipe', '--user-data-dir=' + prof,
                      '--no-first-run', '--disable-gpu', '--no-proxy-server', 'about:blank'], preexec_fn=pre, close_fds=False, stderr=subprocess.DEVNULL)
os.close(to_r); os.close(from_w)
buf = b''; nid = 0; events = []
def read():
    global buf
    while b'\0' not in buf:
        d = os.read(from_r, 1 << 16)
        if not d: raise SystemExit('pipe closed')
        buf += d
    m, buf = buf.split(b'\0', 1); return json.loads(m)
def call(method, params=None, sid=None, timeout=20):
    global nid; nid += 1; m = {'id': nid, 'method': method, 'params': params or {}}
    if sid: m['sessionId'] = sid
    os.write(to_w, json.dumps(m).encode() + b'\0')
    while True:
        r = read()
        if r.get('id') == nid:
            return r.get('result') if 'error' not in r else {'ERROR': r['error']['message']}
        events.append(r)
def ev(ctx, sid, expr, awaitp=False):
    r = call('Runtime.evaluate', {'contextId': ctx, 'expression': expr, 'awaitPromise': awaitp, 'returnByValue': True}, sid)
    if 'ERROR' in r: return r
    return r.get('exceptionDetails', {}).get('exception', {}).get('description') or r['result'].get('value')
def run(with_runtime):
    print('=== Runtime.enable:', with_runtime)
    t = call('Target.createTarget', {'url': 'about:blank'})['targetId']
    sid = call('Target.attachToTarget', {'targetId': t, 'flatten': True})['sessionId']
    print('Page.enable', call('Page.enable', {}, sid))
    if with_runtime: call('Runtime.enable', {}, sid)
    r = call('Page.addScriptToEvaluateOnNewDocument', {'source': 'window.__early = (window.__early||0)+1; document.documentElement && document.documentElement.setAttribute("data-early","1")', 'worldName': 'rdearly'}, sid)
    print('addScriptToEvaluateOnNewDocument ->', r)
    call('Page.navigate', {'url': 'http://127.0.0.1:8471/index.html'}, sid); time.sleep(1.5)
    w = call('Page.createIsolatedWorld', {'frameId': t, 'worldName': 'rd'}, sid); print('createIsolatedWorld', w)
    ctx = w['executionContextId']
    print('early script ran (attr set by isolated-world script):', ev(ctx, sid, 'document.documentElement.getAttribute("data-early")'))
    print('isolated: sees page var?', ev(ctx, sid, 'typeof window.pageSecret'), '| sees DOM:', ev(ctx, sid, 'document.getElementById("box").textContent'))
    t0 = time.time(); v = ev(ctx, sid, 'new Promise(r => setTimeout(() => r("late"), 700))', True); print('awaitPromise ->', v, 'after %.2fs' % (time.time() - t0))
    ev(ctx, sid, 'window.q=[]; window.wake=null; new MutationObserver(ms => { for (const m of ms) for (const n of m.addedNodes) q.push(n.textContent); if (wake) { wake(q.splice(0)); wake=null; } }).observe(document, {childList:true, subtree:true}); window.next = () => new Promise(r => { if (q.length) r(q.splice(0)); else wake = r; }); 1')
    for i in range(3):
        t0 = time.time(); print('  long poll %d -> %s after %.2fs' % (i, ev(ctx, sid, 'next()', True), time.time() - t0))
    print('same-origin <style> rules incl. insertRule:', ev(ctx, sid, '[...document.getElementById("s").sheet.cssRules].map(r=>r.cssText)'))
    print('cross-origin sheet rules:', ev(ctx, sid, '[...document.styleSheets].map(s => { try { return s.href + " " + s.cssRules.length } catch (e) { return s.href + " " + e.name } })'))
    for u in ('http://127.0.0.1:8472/x.css', 'http://127.0.0.1:8472/pic.png'):
        r = call('Page.getResourceContent', {'frameId': t, 'url': u}, sid)
        print('getResourceContent', u, '->', r if 'ERROR' in r else ('base64=%s len=%d %r' % (r['base64Encoded'], len(r['content']), r['content'][:40])))
    tree = call('Page.getResourceTree', {}, sid); print('resource tree:', [(x['type'], x['url'].split('/')[-1]) for x in tree['frameTree'].get('resources', [])])
    s = call('Page.captureScreenshot', {'format': 'webp', 'quality': 60, 'clip': {'x': 8, 'y': 8, 'width': 300, 'height': 200, 'scale': 1}}, sid)
    print('captureScreenshot clip webp ->', s if 'ERROR' in s else '%d bytes, magic %r' % (len(base64.b64decode(s['data'])), base64.b64decode(s['data'])[:12]))
    ev(ctx, sid, 'window.scrollTo(0, 50); 1')
    time.sleep(0.5); print('console-getter probe says:', ev(ctx, sid, 'document.getElementById("probe").textContent'))
    call('Target.closeTarget', {'targetId': t})
print(call('Browser.getVersion')['product'])
run(False); run(True)
call('Browser.close'); p.wait(timeout=10)
