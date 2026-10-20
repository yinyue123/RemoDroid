#!/usr/bin/env python3
"""Download grids (CPU x platform: the server, the router agent, and the desktop agents) from a release's files.

  doc/downloads.py 0.3.4 packages/            Markdown for the release notes (CI, release job)
  gh release view v0.3.4 --json assets -q '.assets[].name' | doc/downloads.py 0.3.4 - --readme README.md
                                                    rewrites the server, agent and desktop grids between
                                                    the README's <!-- downloads:PART --> markers
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


HINT = ('Pick the row for the machine\'s CPU (`uname -m`: `x86_64`, `aarch64` = ARM64, `armv7l` = ARMv7, '
        '`i686` = x86) and the column for its system. OpenWrt: take the build that `opkg print-architecture` '
        'lists. *Any Linux* holds both the server and the agent.')


def merlin_note(names, ver):
    merlin = [f'[Merlin {arch}](https://github.com/{REPO}/releases/download/v{ver}/{name})'
              for arch in ('armv7', 'aarch64')
              if (name := f'remodroid-{ver}-merlin-{arch}.tar.gz') in names]
    return ('Koolshare / KoolCenter offline installers: ' + ' · '.join(merlin)
            + '. RT-AC86U and RT-AX88U: choose aarch64. These are agent-only plugins, not OpenWrt packages.') if merlin else ''


def desktop_grid(names, ver):
    """The remote desktop agents: one row per OS, one column per CPU (packaging/desktop/package.sh)."""
    url = f'https://github.com/{REPO}/releases/download/v{ver}/'
    btn = lambda name, badge, alt: f'[<img src="{BADGE}{badge}.svg" alt="{alt}" height="24">]({url}{name})'
    def cell(*files):
        bs = [btn(n, b, alt) for n, b, alt in files if n in names]
        return '<br>'.join(bs) or '—'
    linux = lambda deb, rpm, tar: cell((f'remodroid-desktop_{ver}-1_{deb}.deb', 'deb', f'desktop .deb {tar}'),
                                       (f'remodroid-desktop-{ver}-1.{rpm}.rpm', 'rpm', f'desktop .rpm {tar}'),
                                       (f'remodroid-desktop-{ver}-linux-{tar}.tar.gz', 'tar', f'desktop .tar.gz {tar}'))
    mac = cell((f'RemoDroid-{ver}-macos.zip', 'zip', 'macOS .zip (Apple silicon and Intel)'))
    return '\n'.join([
        '| Computer | **x86_64**<br><sub>Intel / AMD</sub> | **ARM64**<br><sub>Apple silicon, Snapdragon…</sub> |', '|:--|:-:|:-:|',
        f'| **Windows** 10 1809+, 11 | {cell((f"RemoDroid-{ver}-windows-x86_64.zip", "zip", "Windows .zip x86_64"))} | '
        f'{cell((f"RemoDroid-{ver}-windows-arm64.zip", "zip", "Windows .zip ARM64"))} |',
        f'| **macOS** 13+ | {mac} | {mac} |',
        f'| **Linux** (X11 session) | {linux("amd64", "x86_64", "x86_64")} | {linux("arm64", "aarch64", "aarch64")} |',
    ])


DESKTOP_HINT = ('Remote desktop for the computer itself: install it there, then pair it by pasting the line from the panel '
                '(Agents → Add → Computer). The macOS build runs on Apple silicon and Intel Macs.')


def checksums(ver):
    return f'Checksums: [SHA256SUMS](https://github.com/{REPO}/releases/download/v{ver}/SHA256SUMS)'


def markdown(names, ver):
    return '\n'.join([
        f'### Downloads (v{ver})', '', HINT, '',
        grid('server', names, ver), '',
        grid('agent', names, ver), '', merlin_note(names, ver), '',
        '#### Remote desktop (Windows, macOS, Linux)', '', DESKTOP_HINT, '', desktop_grid(names, ver), '',
        checksums(ver),
    ])


def section(part, names, ver):
    """One part's grid for its own install step in the README."""
    if part == 'desktop':
        return '\n'.join([f'#### Download RemoDroid for your computer (v{ver})', '', DESKTOP_HINT, '',
                          desktop_grid(names, ver), '', checksums(ver)])
    note = merlin_note(names, ver) if part == 'agent' else ''
    return '\n'.join([f'#### Download the {part} (v{ver})', '', HINT, '', grid(part, names, ver), '']
                      + ([note, ''] if note else []) + [checksums(ver)])


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    ver, src = sys.argv[1].lstrip('v'), sys.argv[2]
    names = set(sys.stdin.read().split() if src == '-' else os.listdir(src))
    if '--readme' in sys.argv:
        path = sys.argv[sys.argv.index('--readme') + 1]
        text = open(path).read()
        for part in ('server', 'agent', 'desktop'):
            begin, end = f'<!-- downloads:{part} -->', f'<!-- /downloads:{part} -->'
            if begin not in text or end not in text:
                if part == 'desktop': # translations may not have the section yet
                    continue
                sys.exit(f'{path} has no {begin} … {end} markers')
            md = section(part, names, ver)
            text = re.sub(re.escape(begin) + '.*?' + re.escape(end), lambda _: f'{begin}\n{md}\n{end}', text, flags=re.S)
        open(path, 'w').write(text)
    else:
        print(markdown(names, ver))

if __name__ == '__main__':
    main()
