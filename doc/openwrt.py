#!/usr/bin/env python3
"""Router list for doc/openwrt.md, from OpenWrt's own build metadata (profiles.json of every target).

  doc/openwrt.py --doc doc/openwrt.md     rewrites the tables between the <!-- targets --> and <!-- routers --> markers
  doc/openwrt.py                          prints them

Keeps the routers whose image ships USB drivers (the phone needs a USB port), maps each one's package
architecture to the RemoDroid download, and finds the first release (from 21.02, the first with the
LuCI page) that builds it. Downloads are cached in $CACHE (default /tmp/openwrt-profiles).
"""
import json
import os
import re
import sys
import urllib.error
import urllib.request

RELEASES = ['21.02.7', '22.03.7', '23.05.6', '24.10.8']  # the last of each opkg (ipk) release line
BASE = 'https://downloads.openwrt.org/releases/'
CACHE = os.environ.get('CACHE', '/tmp/openwrt-profiles')

# package architecture → (group heading, what to download)
IPK = {
    'aarch64_cortex-a53': 'ARM64', 'aarch64_cortex-a72': 'ARM64', 'aarch64_generic': 'ARM64',
    'arm_cortex-a7_neon-vfpv4': 'ARMv7', 'arm_cortex-a9_vfpv3-d16': 'ARMv7', 'arm_cortex-a15_neon-vfpv4': 'ARMv7',
    'mipsel_24kc': 'MIPS LE', 'mips_24kc': 'MIPS BE', 'x86_64': 'x86_64', 'i386_pentium4': 'x86',
}


def tarball(arch):
    """Architectures without an ipk whose CPU still runs a static RemoDroid binary."""
    if arch.startswith('aarch64_'):
        return 'ARM64'
    if arch.startswith('arm_cortex-a') and ('vfp' in arch or 'neon' in arch):
        return 'ARMv7'  # hard-float build: needs a VFP unit
    if arch in ('mipsel_74kc', 'mipsel_24kc_24kf'):
        return 'MIPS LE'
    if arch in ('mips_74kc', 'mips_24kc_24kf'):
        return 'MIPS BE'
    return None


def get(url, name):
    path = os.path.join(CACHE, name)
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        for attempt in range(3):
            try:
                data = urllib.request.urlopen(url, timeout=60).read()
                break
            except urllib.error.HTTPError as e:
                if e.code == 404:  # a subtarget without profiles.json
                    data = b''
                    break
            except Exception:
                pass
        else:
            sys.exit(f'cannot download {url}')
        open(path, 'wb').write(data)
    return open(path, 'rb').read().decode()


def subdirs(url, name):
    return [d for d in re.findall(r'href="([a-z0-9_-]+)/"', get(url, name)) if d not in ('packages', 'kmods')]


def profiles(release):
    """{(target, profile id): (arch, target default packages, profile)} for every image of a release."""
    out = {}
    for target in subdirs(f'{BASE}{release}/targets/', f'{release}.html'):
        for sub in subdirs(f'{BASE}{release}/targets/{target}/', f'{release}-{target}.html'):
            raw = get(f'{BASE}{release}/targets/{target}/{sub}/profiles.json', f'{release}-{target}-{sub}.json')
            if not raw.strip():
                continue
            j = json.loads(raw)
            for pid, p in j.get('profiles', {}).items():
                out[f'{target}/{sub}', pid] = (j.get('arch_packages', ''), j.get('default_packages', []), p)
    return out


# targets whose kernel has the USB drivers built in, so their images carry no kmod-usb package
BUILTIN_USB = ('mvebu/', 'x86/', 'bcm27xx/', 'rockchip/')


def has_usb(target, defaults, p):
    dev = p.get('device_packages', [])
    if any(x.startswith('kmod-usb') for x in dev) or target.startswith(BUILTIN_USB):
        return True
    return any(x in ('kmod-usb2', 'kmod-usb3') and '-' + x not in dev for x in defaults)


def name(t):
    return ' '.join(x for x in (t.get('vendor'), t.get('model'), t.get('variant')) if x) or t.get('title', '?')


def download(arch):
    return f'`{arch}`' if arch in IPK else f'{tarball(arch)} `.tar.gz` (`{arch}`, untested)'


def tables():
    seen = {}  # profile id → first release
    for rel in RELEASES:
        for (_, pid) in profiles(rel):
            seen.setdefault(pid, rel.rsplit('.', 1)[0])
    rows, targets = {}, {}
    for (target, pid), (arch, defaults, p) in profiles(RELEASES[-1]).items():
        group = IPK.get(arch) or tarball(arch)
        if not group or not has_usb(target, defaults, p):
            continue
        targets.setdefault((target, arch), set()).add(pid)
        for t in p.get('titles', []):
            rows.setdefault(group, set()).add((name(t), target, download(arch), seen[pid]))

    by_target = ['| Target platform | Download | Routers with USB |', '|---|---|---|']
    by_target += [f'| `{t}` | {download(a)} | {len(ids)} |' for (t, a), ids in sorted(targets.items())]

    routers = [f'{sum(len(v) for v in rows.values())} routers with a USB port in OpenWrt {RELEASES[-1]}. *Since* is the '
               'first OpenWrt release (21.02 or later) with an image for it. Use your browser\'s find (Ctrl+F) '
               'after opening a group.', '']
    for group in ('ARM64', 'ARMv7', 'MIPS LE', 'MIPS BE', 'x86_64', 'x86'):
        devs = sorted(rows.get(group, ()), key=lambda r: r[0].lower())
        if devs:
            routers += ['<details>', f'<summary><b>{group}</b>: {len(devs)} {"router" if len(devs) == 1 else "routers"}</summary>', '',
                        '| Router | Target | Download | Since |', '|---|---|---|---|']
            routers += [f'| {n} | `{t}` | {a} | {s} |' for n, t, a, s in devs]
            routers += ['', '</details>', '']
    return {'targets': '\n'.join(by_target), 'routers': '\n'.join(routers).rstrip()}


def main():
    md = tables()
    if '--doc' not in sys.argv:
        print('\n\n'.join(md.values()))
        return
    path = sys.argv[sys.argv.index('--doc') + 1]
    text = open(path).read()
    for key, body in md.items():
        begin, end = f'<!-- {key} -->', f'<!-- /{key} -->'
        if begin not in text or end not in text:
            sys.exit(f'{path} has no {begin} … {end} markers')
        text = re.sub(re.escape(begin) + '.*?' + re.escape(end), lambda _: f'{begin}\n{body}\n{end}', text, flags=re.S)
    open(path, 'w').write(text)

if __name__ == '__main__':
    main()
