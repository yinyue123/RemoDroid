#!/usr/bin/env python3
"""Draws the install site's screen pictures, wizard/demo/*.svg: the cloud firewall, the RemoDroid Server window,
Android's USB debugging, the panel's Add → Computer and pairing a computer. Run it after changing one:
  python3 doc/demos.py"""
import os
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'wizard', 'demo')
STYLE = '''<style>
svg{--bg:#F7F4EE;--card:#FFFDF9;--card2:#EFE9DF;--line:#D6CDBE;--ink:#1E1C19;--muted:#7C746A;--accent:#B4553A;--soft:#F3E2D9;--ok:#3C7A46;--oksoft:#E1EEDD;--term:#1F1E1B}
@media (prefers-color-scheme:dark){svg{--bg:#16140F;--card:#1C1A15;--card2:#26221B;--line:#3A352B;--ink:#EEE8DC;--muted:#A39A8C;--accent:#E0805F;--soft:#3A2419;--ok:#86C37A;--oksoft:#1E2E1C;--term:#0E0D0B}}
text{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,sans-serif;font-size:13px;fill:var(--ink)}
.m{fill:var(--muted)}.a{fill:var(--accent)}.ok{fill:var(--ok)}.b{font-weight:600}.mono{font-family:ui-monospace,"SF Mono",Menlo,Consolas,monospace}
.card{fill:var(--card);stroke:var(--line);stroke-width:1.5}.c2{fill:var(--card2)}.ln{stroke:var(--line);stroke-width:1.2;fill:none}
.hl{fill:var(--soft);stroke:var(--accent);stroke-width:1.8}.w{fill:#fff}
</style>'''
def svg(w, h, body, label):
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" role="img" aria-label="{label}">\n{STYLE}\n{body}\n</svg>\n'
def t(x, y, s, cls='', size=None, anchor=None):
    a = f' class="{cls}"' if cls else ''
    a += f' font-size="{size}"' if size else ''
    a += f' text-anchor="{anchor}"' if anchor else ''
    return f'<text x="{x}" y="{y}"{a}>{s}</text>'
def r(x, y, w, h, rx=8, cls='card', extra=''):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" class="{cls}"{extra}/>'
def tap(x, y, n=None):
    g = f'<g transform="translate({x} {y})"><circle r="17" fill="none" stroke="var(--accent)" stroke-width="2" opacity=".55"/><circle r="10" fill="var(--accent)" opacity=".22"/></g>'
    return g + (badge(x + 22, y - 20, n) if n else '')
def badge(x, y, n):
    return f'<g transform="translate({x} {y})"><circle r="11" fill="var(--accent)"/>{t(0, 4.5, n, "w b", 12, "middle")}</g>'
def cursor(x, y):
    return f'<path transform="translate({x} {y})" d="M0 0v17l4.6-4.2 3.1 7 2.8-1.2-3.1-6.9h6.3z" fill="#fff" stroke="#1E1C19" stroke-width="1.3" stroke-linejoin="round"/>'
def arrow(x1, y, x2):
    return f'<path d="M{x1} {y}H{x2 - 2}" stroke="var(--accent)" stroke-width="2" fill="none" stroke-dasharray="4 5"/><path d="M{x2 - 9} {y - 6}l8 6-8 6" stroke="var(--accent)" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>'
def toggle(x, y, on=True):
    return (f'<rect x="{x}" y="{y}" width="34" height="20" rx="10" fill="{"var(--accent)" if on else "var(--line)"}"/>'
            f'<circle cx="{x + (24 if on else 10)}" cy="{y + 10}" r="7.5" fill="#fff"/>')
def check(x, y, on=True):
    return (f'<rect x="{x}" y="{y}" width="16" height="16" rx="3.5" fill="{"var(--accent)" if on else "none"}" stroke="{"var(--accent)" if on else "var(--muted)"}" stroke-width="1.6"/>'
            + (f'<path d="M{x + 3.5} {y + 8.5}l3 3 6-6.5" stroke="#fff" stroke-width="2" fill="none" stroke-linecap="round" stroke-linejoin="round"/>' if on else ''))
def field(x, y, w, val, cls='mono', h=28, hl=False):
    return r(x, y, w, h, 5, 'hl' if hl else 'card') + t(x + 9, y + h / 2 + 4.5, val, cls, 12.5)
def phone(x, y, w=230, h=400):
    return (r(x, y, w, h, 30, 'card', ' stroke-width="2.5"') + f'<rect x="{x + w / 2 - 28}" y="{y + 10}" width="56" height="7" rx="3.5" fill="var(--line)"/>')

# ---------- 1. the cloud firewall ----------
def firewall():
    W, H = 700, 330
    b = r(1, 1, W - 2, H - 2, 14)
    b += f'<path d="M1 15a14 14 0 0 1 14-14h{W - 30}a14 14 0 0 1 14 14v31H1z" class="c2"/>'
    b += '<g transform="translate(20 12)" fill="none" stroke="var(--accent)" stroke-width="1.8" stroke-linejoin="round"><path d="M11 1l9 3.5v6c0 5.6-3.8 9.6-9 11.5-5.2-1.9-9-5.9-9-11.5v-6z"/><path d="M7 11l3 3 5-5.5" stroke-linecap="round"/></g>'
    b += t(50, 29, 'Security group', 'b', 14) + t(168, 29, '›  Inbound rules', 'm', 14)
    cols = [(24, 'Protocol'), (130, 'Port'), (290, 'Source'), (440, 'For'), (610, 'Action')]
    b += ''.join(t(x, 74, s.upper(), 'm b', 11) for x, s in cols)
    rows = [('TCP', '5580', 'the panel (no domain)'), ('TCP', '80 · 443', 'HTTPS (with a domain)'), ('UDP', '3478', 'STUN / TURN'), ('UDP', '49152-49252', 'video relay')]
    for i, (p, port, why) in enumerate(rows):
        y = 88 + i * 46
        b += f'<path d="M14 {y}H{W - 14}" class="ln"/>'
        b += r(24, y + 11, 46, 24, 12, 'c2') + t(47, y + 27.5, p, 'mono b', 12, 'middle')
        b += t(130, y + 28, port, 'mono b a', 15) + t(290, y + 28, '0.0.0.0/0', 'mono m', 13) + t(440, y + 28, why, 'm', 12.5)
        b += f'<rect x="610" y="{y + 11}" width="66" height="24" rx="12" fill="var(--oksoft)"/>' + t(643, y + 27.5, '✓ Allow', 'ok b', 12, 'middle')
    y = 88 + 4 * 46
    b += f'<path d="M14 {y}H{W - 14}" class="ln"/>'
    b += r(24, y + 18, 118, 34, 17, 'hl') + t(83, y + 40, '+ Add rule', 'a b', 13.5, 'middle') + cursor(132, y + 40)
    return svg(W, H, b, 'Cloud firewall inbound rules: TCP 5580, TCP 80 and 443, UDP 3478, UDP 49152-49252, from anywhere')

# ---------- 2. RemoDroidServer.exe ----------
def winserver():
    W, H = 700, 520
    X = 1
    b = f'<rect x="1" y="1" width="{W - 2}" height="{H - 2}" rx="9" class="card"/>'
    b += f'<path d="M1 10a9 9 0 0 1 9-9h{W - 20}a9 9 0 0 1 9 9v26H1z" class="c2"/>'
    b += '<rect x="14" y="10" width="16" height="16" rx="4" fill="#13171C"/><rect x="19" y="12.5" width="6" height="11" rx="1.5" fill="none" stroke="#6CC8EE" stroke-width="1.3"/>'
    b += t(38, 23, 'RemoDroid Server', '', 12.5)
    b += f'<path d="M{W - 112} 18h10M{W - 64} 13l10 10M{W - 54} 13l-10 10" stroke="var(--muted)" stroke-width="1.3"/>'
    b += t(24, 72, 'RemoDroid Server', 'b', 20)
    b += '<circle cx="30" cy="94" r="5" fill="var(--ok)"/>' + t(42, 98.5, 'Running', 'ok b', 13)
    x = 24
    for s, w in [('Open panel', 96), ('Stop', 56), ('Restart', 70), ('Show log', 82)]:
        b += r(x, 112, w, 28, 5) + t(x + w / 2, 130.5, s, '', 12.5, 'middle'); x += w + 8
    def sec(y, n, title):
        return f'<path d="M24 {y}H{W - 24}" class="ln"/>' + badge(34, y + 24, n) + t(54, y + 29, title, 'b', 14)
    def row(y, label, x, w, val, lx=54):
        return t(lx, y + 18, label, 'm', 12.5) + field(x, y, w, val)
    y = 160
    b += sec(y, 1, 'Administrator (signs in to the web panel)')
    b += row(y + 46, 'Username', 140, 170, 'admin') + row(y + 46, 'Password', 440, 170, '••••••••', 360)
    y = 260
    b += sec(y, 2, 'Web panel and HTTPS')
    b += row(y + 46, 'Web port', 140, 80, '5580') + row(y + 46, 'Domain', 440, 236, 'rd.example.com', 360)
    b += t(54, y + 100, 'Domain is optional: it gets free HTTPS from Let’s Encrypt.', 'm', 12)
    y = 380
    b += sec(y, 3, 'STUN / TURN relay')
    b += check(54, y + 46) + t(78, y + 59, 'Built-in STUN/TURN server (UDP)', '', 12.5)
    b += row(y + 40, 'Port', 360 + 80, 80, '3478', 360 + 40 - 40) if False else ''
    b += t(330, y + 59, 'Public IPv4', 'm', 12.5) + field(410, y + 41, 140, '203.0.113.10')
    b += r(W - 178, H - 52, 154, 34, 6, 'hl') + t(W - 101, H - 30, 'Save and restart', 'a b', 13.5, 'middle') + badge(W - 178, H - 52, 4) + cursor(W - 60, H - 36)
    return svg(W, H, b, 'The RemoDroid Server window: administrator login, web port and domain, STUN/TURN, then Save and restart')

# ---------- 3. Android: developer options, USB debugging ----------
def android_debug():
    W, H = 700, 430
    b = ''
    # phone 1: About phone, tap Build number 7 times
    px = 10
    b += phone(px, 10)
    b += t(px + 22, 62, '←', 'm', 16) + t(px + 48, 62, 'About phone', 'b', 16)
    items = [('Device name', 'My phone'), ('Model', 'Pixel 7'), ('Android version', '14'), ('Build number', 'UQ1A.240105')]
    for i, (k, v) in enumerate(items):
        y = 84 + i * 56
        if k == 'Build number':
            b += f'<rect x="{px + 10}" y="{y}" width="210" height="52" rx="10" class="hl"/>'
        b += t(px + 22, y + 22, k, 'b' if k == 'Build number' else '', 13.5) + t(px + 22, y + 40, v, 'm', 12)
    b += tap(px + 170, 84 + 3 * 56 + 26)
    b += f'<g transform="translate({px + 150} {84 + 3 * 56 - 22})"><rect width="54" height="26" rx="13" fill="var(--accent)"/>{t(27, 18, "× 7", "w b", 14, "middle")}</g>'
    b += f'<rect x="{px + 22}" y="{330}" width="186" height="34" rx="17" fill="var(--term)"/>' + t(px + 115, 351, 'You are now a developer!', 'w', 11.5, 'middle')
    b += badge(px + 8, 18, 1)
    # arrow with the path to developer options
    b += arrow(255, 210, 445)
    b += t(350, 196, 'Settings › System ›', 'm', 12, 'middle')
    # phone 2: Developer options, USB debugging on
    px = 460
    b += phone(px, 10)
    b += t(px + 22, 62, '←', 'm', 16) + t(px + 48, 62, 'Developer options', 'b', 16)
    b += f'<rect x="{px + 10}" y="80" width="210" height="44" rx="12" class="c2"/>' + t(px + 22, 107, 'Use developer options', '', 11.5) + toggle(px + 176, 92)
    rows = [('Stay awake', False), ('USB debugging', True), ('Wireless debugging', False)]
    for i, (k, on) in enumerate(rows):
        y = 140 + i * 58
        if on:
            b += f'<rect x="{px + 10}" y="{y}" width="210" height="52" rx="10" class="hl"/>'
        b += t(px + 22, y + 31, k, 'b' if on else '', 13.5) + toggle(px + 176, y + 16, on)
    b += tap(px + 193, 140 + 58 + 26)
    b += badge(px + 8, 18, 2)
    return svg(W, H, b, 'Android: tap Build number seven times, then turn on USB debugging in Developer options')

# ---------- 4. Android: allow the router ----------
def android_allow():
    W, H = 700, 330
    b = ''
    # the router with a cable to the phone
    b += '<g transform="translate(30 120) scale(3.6)" fill="none" stroke="var(--muted)" stroke-width="1.4" stroke-linecap="round" stroke-linejoin="round"><rect x="3" y="13" width="18" height="7" rx="2"/><path d="M6.5 13V9M17.5 13V9M7 16.5h.01M10 16.5h.01M10 7a3 3 0 0 1 4 0M8 5a6 6 0 0 1 8 0"/></g>'
    b += '<path d="M118 192C180 192 170 262 240 262H262" fill="none" stroke="var(--muted)" stroke-width="3" stroke-linecap="round"/>'
    b += '<g transform="translate(160 214)"><rect width="34" height="18" rx="4" fill="var(--card)" stroke="var(--muted)" stroke-width="1.4"/>' + t(17, 13, 'USB', 'm b', 9.5, 'middle') + '</g>'
    # the phone with the dialog
    px, py, pw, ph = 262, 6, 300, 318
    b += r(px, py, pw, ph, 30, 'card', ' stroke-width="2.5"')
    b += f'<rect x="{px}" y="{py}" width="{pw}" height="{ph}" rx="30" fill="#000" opacity=".22"/>'
    dx, dy, dw = px + 18, 52, pw - 36
    b += r(dx, dy, dw, 226, 18, 'card')
    b += t(dx + 18, dy + 34, 'Allow USB debugging?', 'b', 16)
    b += t(dx + 18, dy + 60, "The computer's RSA key fingerprint is:", 'm', 11.5) + t(dx + 18, dy + 78, '3A:9F:41:0C:…:D2', 'mono m', 11.5)
    b += f'<rect x="{dx + 10}" y="{dy + 96}" width="{dw - 20}" height="40" rx="8" class="hl"/>' + check(dx + 20, dy + 108) + t(dx + 46, dy + 121, 'Always allow from this computer', '', 12.5)
    b += tap(dx + 28, dy + 116) + badge(dx + 10, dy + 96, 1)
    b += t(dx + dw - 128, dy + 190, 'Cancel', 'm b', 14, 'middle')
    b += f'<rect x="{dx + dw - 82}" y="{dy + 168}" width="66" height="34" rx="17" fill="var(--accent)"/>' + t(dx + dw - 49, dy + 190, 'Allow', 'w b', 14, 'middle')
    b += tap(dx + dw - 49, dy + 185) + badge(dx + dw - 16, dy + 166, 2)
    # what you get
    b += arrow(578, 166, 616)
    b += f'<g transform="translate(624 132)"><rect width="66" height="66" rx="18" fill="var(--oksoft)" stroke="var(--ok)" stroke-width="1.8"/><path d="M20 34l9 9 18-20" stroke="var(--ok)" stroke-width="3.5" fill="none" stroke-linecap="round" stroke-linejoin="round"/></g>'
    b += t(657, 220, 'Ready', 'ok b', 13, 'middle')
    return svg(W, H, b, 'On the phone: tick Always allow from this computer, then Allow')

# ---------- 5. the panel: Agents → Add → Computer → Copy ----------
def panel_add():
    W, H = 700, 300
    b = r(1, 1, W - 2, H - 2, 14)
    b += f'<path d="M1 15a14 14 0 0 1 14-14h{W - 30}a14 14 0 0 1 14 14v25H1z" class="c2"/>'
    b += ''.join(f'<circle cx="{20 + i * 16}" cy="21" r="5" fill="var(--line)"/>' for i in range(3))
    b += r(80, 9, 300, 24, 12, 'card') + t(96, 25.5, '🔒 your panel  /  Agents', 'm', 12)
    # tabs and the Add button
    b += t(24, 78, 'Agents', 'b', 20)
    b += r(W - 112, 56, 88, 34, 17, 'hl') + t(W - 68, 78, '+ Add', 'a b', 14, 'middle') + badge(W - 112, 56, 1)
    # kind
    b += f'<rect x="24" y="108" width="330" height="40" rx="12" class="c2"/>'
    b += t(108, 133, 'Router with a phone', 'm', 13, 'middle')
    b += f'<rect x="196" y="112" width="154" height="32" rx="9" class="hl"/>' + t(273, 133, 'Computer', 'a b', 13.5, 'middle') + badge(352, 112, 2)
    # the line with copy
    b += f'<rect x="24" y="168" width="{W - 48}" height="50" rx="10" fill="var(--term)"/>'
    b += t(40, 198, 'remodroid-desktopd pair --panel https://rd.example.com --code 4F7Q2K', 'mono', 12.5).replace('<text', '<text fill="#E8E3D8" style="fill:#E8E3D8"', 1)
    b += f'<rect x="{W - 104}" y="176" width="72" height="34" rx="8" fill="var(--accent)"/>' + t(W - 68, 198, 'Copy', 'w b', 13.5, 'middle') + badge(W - 104, 176, 3) + cursor(W - 42, 202)
    b += '<g transform="translate(26 236)" fill="none" stroke="var(--muted)" stroke-width="1.5" stroke-linecap="round"><circle cx="9" cy="9" r="8"/><path d="M9 4.5V9l3 2"/></g>' + t(48, 250, 'valid for 10 minutes, once', 'm', 12.5)
    return svg(W, H, b, 'In the panel: Agents, Add, Computer, then Copy the pairing line')

# ---------- 6. the computer: tray icon → Pair with a panel… → paste → Pair ----------
def pair():
    W, H = 720, 360
    b = ''
    # the taskbar / menu bar with the tray icon and its menu
    b += f'<rect x="0" y="0" width="250" height="36" rx="8" class="c2"/>'
    b += t(236, 23, '10:42', 'm', 12.5, 'end')
    b += '<g transform="translate(140 7)"><rect width="22" height="22" rx="5" fill="#13171C"/><rect x="7.5" y="3.5" width="7" height="15" rx="2" fill="none" stroke="#6CC8EE" stroke-width="1.5"/></g>'
    b += tap(151, 18) + badge(170, 0, 1)
    b += r(40, 46, 210, 132, 10, 'card')
    b += t(56, 72, 'RemoDroid — not paired', 'm', 12)
    b += f'<path d="M50 84H240" class="ln"/>'
    b += f'<rect x="48" y="92" width="194" height="30" rx="6" class="hl"/>' + t(58, 112, 'Pair with a panel…', 'a b', 13) + badge(242, 92, 2)
    b += t(58, 145, 'Open panel', '', 13) + t(58, 168, 'Quit', '', 13)
    b += arrow(256, 108, 296)
    # the pairing window
    x0, w = 300, 418
    b += r(x0, 1, w, 300, 10)
    b += f'<path d="M{x0} 11a10 10 0 0 1 10-10h{w - 20}a10 10 0 0 1 10 10v22H{x0}z" class="c2"/>'
    b += t(x0 + 14, 22, 'RemoDroid', '', 12)
    b += t(x0 + 18, 64, 'Pair this computer with your panel', 'b', 15)
    b += t(x0 + 18, 96, 'Pairing command', 'm', 12)
    b += f'<rect x="{x0 + 18}" y="104" width="{w - 36}" height="34" rx="6" class="hl"/>'
    b += t(x0 + 28, 126, 'remodroid-desktopd pair --panel https://rd.examp…', 'mono', 11.5)
    b += f'<g transform="translate({x0 + w - 104} 146)"><rect width="86" height="28" rx="6" fill="var(--term)"/>{t(43, 19, "Ctrl/⌘ + V", "w b", 12, "middle")}</g>' + badge(x0 + w - 104, 146, 3)
    b += t(x0 + 18, 196, 'Name', 'm', 12) + field(x0 + 18, 204, 200, 'my-laptop')
    b += f'<rect x="{x0 + w - 104}" y="250" width="86" height="34" rx="6" fill="var(--accent)"/>' + t(x0 + w - 61, 272, 'Pair', 'w b', 14, 'middle') + badge(x0 + w - 104, 250, 4) + cursor(x0 + w - 30, 274)
    # the result
    b += f'<rect x="{x0}" y="314" width="{w}" height="40" rx="10" fill="var(--oksoft)" stroke="var(--ok)" stroke-width="1.5"/>'
    b += f'<path d="M{x0 + 18} 334l6 6 12-13" stroke="var(--ok)" stroke-width="2.6" fill="none" stroke-linecap="round" stroke-linejoin="round"/>' + t(x0 + 46, 339, 'Paired as “my-laptop”.', 'ok b', 13.5)
    b = f'<g transform="translate(0 16)">{b}</g>'
    return svg(W, H + 16, b, 'On the computer: click the RemoDroid icon, Pair with a panel, paste the line, Pair')

for name, f in [('firewall', firewall), ('winserver', winserver), ('android-debug', android_debug), ('android-allow', android_allow), ('panel-add', panel_add), ('pair', pair)]:
    open(os.path.join(OUT, name + '.svg'), 'w').write(f())

