#!/usr/bin/env python3
"""Download grids (CPU x platform, one for the server and one for the agent) from a release's files.

  doc/downloads.py 0.3.4 packages/            Markdown for the release notes (CI, release job)
  gh release view v0.3.4 --json assets -q '.assets[].name' | doc/downloads.py 0.3.4 - --readme README.md
                                                    rewrites the grids between the README's markers
"""
import os
import re
import sys

REPO = 'yinyue123/RemoDroid'
BADGE = f'https://raw.githubusercontent.com/{REPO}/main/doc/badges/'
COLS = [('deb', 'Debian · Ubuntu'), ('rpm', 'CentOS · Fedora'), ('apk', 'Alpine'), ('ipk', 'OpenWrt'), ('tar', 'Any Linux')]
ROWS = [  # cpu, label, deb, rpm, apk, opkg arches, tarball
    ('x86_64', '**x86_64**<br><sub>Intel / AMD 64-bit</sub>', 'amd64', 'x86_64', 'x86_64', ['x86_64'], 'x86_64'),
    ('aarch64', '**ARM64**<br><sub>aarch64</sub>', 'arm64', 'aarch64', 'aarch64',
     ['aarch64_generic', 'aarch64_cortex-a53', 'aarch64_cortex-a72'], 'aarch64'),
    ('armv7', '**ARMv7**<br><sub>32-bit ARM</sub>', 'armhf', 'armv7hl', 'armv7',
     ['arm_cortex-a7_neon-vfpv4', 'arm_cortex-a9_vfpv3-d16', 'arm_cortex-a15_neon-vfpv4'], 'armv7'),
    ('x86', '**x86**<br><sub>32-bit</sub>', 'i386', 'i686', 'x86', ['i386_pentium4'], 'x86'),
    ('mipsel', '**MIPS LE**<br><sub>mipsel</sub>', None, None, None, ['mipsel_24kc'], 'mipsel'),
    ('mips', '**MIPS BE**<br><sub>mips</sub>', None, None, None, ['mips_24kc'], 'mips'),
]
IPK_BADGE = {'aarch64_generic': 'ipk-generic', 'aarch64_cortex-a53': 'ipk-a53', 'aarch64_cortex-a72': 'ipk-a72',
             'arm_cortex-a7_neon-vfpv4': 'ipk-a7', 'arm_cortex-a9_vfpv3-d16': 'ipk-a9', 'arm_cortex-a15_neon-vfpv4': 'ipk-a15'}
BEGIN, END = '<!-- downloads -->', '<!-- /downloads -->'


def grid(part, names, ver):
    url = f'https://github.com/{REPO}/releases/download/v{ver}/'
    btn = lambda name, badge, alt: f'[<img src="{BADGE}{badge}.svg" alt="{alt}" height="24">]({url}{name})'
    expect = lambda n: n if n in names else None
    out = [f'| {part.title()} | ' + ' | '.join(c[1] for c in COLS) + ' |', '|:--|' + ':-:|' * len(COLS)]
    for cpu, label, deb, rpm, apk, ipks, tar in ROWS:
        cells = []
        for kind, _ in COLS:
            if kind == 'deb':
                n = deb and expect(f'remodroid-{part}_{ver}-1_{deb}.deb')
                cells.append(btn(n, 'deb', f'{part} .deb {cpu}') if n else '—')
            elif kind == 'rpm':
                n = rpm and expect(f'remodroid-{part}-{ver}-1.{rpm}.rpm')
                cells.append(btn(n, 'rpm', f'{part} .rpm {cpu}') if n else '—')
            elif kind == 'apk':
                n = apk and expect(f'remodroid-{part}_{ver}-r1_{apk}.apk')
                cells.append(btn(n, 'apk', f'{part} .apk {cpu}') if n else '—')
            elif kind == 'ipk':
                bs = [btn(n, IPK_BADGE.get(a, 'ipk'), f'{part} .ipk {a}')
                      for a in ipks if (n := expect(f'remodroid-{part}_{ver}-1_{a}.ipk'))]
                cells.append('<br>'.join(bs) or '—')
            else:
                n = expect(f'remodroid-{ver}-linux-{tar}.tar.gz')
                cells.append(btn(n, 'tar', f'server + agent .tar.gz {cpu}') if n else '—')
        out.append(f'| {label} | ' + ' | '.join(cells) + ' |')
    return '\n'.join(out)


def markdown(names, ver):
    merlin = [f'[Merlin {arch}](https://github.com/{REPO}/releases/download/v{ver}/{name})'
              for arch in ('armv7', 'aarch64')
              if (name := f'remodroid-{ver}-merlin-{arch}.tar.gz') in names]
    merlin_note = ('Koolshare / KoolCenter offline installers: ' + ' · '.join(merlin)
                   + '. RT-AC86U: choose armv7. These are agent-only plugins, not OpenWrt packages.') if merlin else ''
    return '\n'.join([
        f'### Downloads (v{ver})', '',
        'Pick the row for the machine\'s CPU (`uname -m`: `x86_64`, `aarch64` = ARM64, `armv7l` = ARMv7, '
        '`i686` = x86) and the column for its system. OpenWrt: take the build that `opkg print-architecture` '
        'lists. *Any Linux* holds both the server and the agent.', '',
        grid('server', names, ver), '',
        grid('agent', names, ver), '', merlin_note, '',
        f'Checksums: [SHA256SUMS](https://github.com/{REPO}/releases/download/v{ver}/SHA256SUMS)',
    ])


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ver, src = sys.argv[1].lstrip('v'), sys.argv[2]
    names = set(sys.stdin.read().split() if src == '-' else os.listdir(src))
    md = markdown(names, ver)
    if '--readme' in sys.argv:
        path = sys.argv[sys.argv.index('--readme') + 1]
        text = open(path).read()
        if BEGIN not in text or END not in text:
            sys.exit(f'{path} has no {BEGIN} … {END} markers')
        text = re.sub(re.escape(BEGIN) + '.*?' + re.escape(END), lambda _: f'{BEGIN}\n{md}\n{END}', text, flags=re.S)
        open(path, 'w').write(text)
    else:
        print(md)


if __name__ == '__main__':
    main()
