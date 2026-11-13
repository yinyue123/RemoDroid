#!/usr/bin/env python3
"""Install the Actions-built browser on sx through hqnode's panel relay.

Host settings live in /etc/rd-browser-sx/browser.conf. A first pairing also
    needs panel-login.json there, with panel, user and password; it is removed
after pairing succeeds. Existing containers and /data are preserved.
"""

import http.cookiejar
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import subprocess
import sys
import time
import urllib.request


STATE = Path('/etc/rd-browser-sx')
NAME = 'browser-sx'


def run(*args, capture=False):
    return subprocess.run(args, check=True, text=True, capture_output=capture).stdout


def api(method, path, body=None):
    token = Path('/etc/hqnode/cli-token').read_text().strip()
    args = ['curl', '-fsS', '--noproxy', '*', '--max-time', '2700',
            '--unix-socket', '/run/hqnode.sock', '-H', f'Authorization: Bearer {token}',
            '-H', 'Content-Type: application/json', '-X', method, 'http://localhost' + path]
    if body is not None:
        args += ['--data-binary', '@-']
    result = subprocess.run(args, input=json.dumps(body) if body is not None else None,
                            text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(f'hqnode {method} {path}: {result.stderr.strip()}')
    return json.loads(result.stdout) if result.stdout.strip() else {}


def inside(*args, capture=False):
    return run('hqnode', 'exec', NAME, *args, capture=capture)


def pair():
    login = STATE / 'panel-login.json'
    if not login.exists():
        inside('sh', '-ec', 'grep -q "^device_secret *=" /data/remodroid/desktop.ini')
        return
    settings = json.loads(login.read_text())
    panel = settings['panel'].rstrip('/')
    opener = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()))

    def post(path, body):
        request = urllib.request.Request(panel + path, data=json.dumps(body).encode(),
                                         headers={'Content-Type': 'application/json'}, method='POST')
        with opener.open(request, timeout=30) as response:
            return json.load(response)

    post('/login', {'user': settings['user'], 'password': settings['password']})
    code = post('/api/v1/enroll/codes', {'name': NAME})['code']
    inside('systemctl', 'stop', 'remodroid-desktopd')
    # Capture the pairing response so the one-time credential never enters the CI log.
    inside('runuser', '-u', 'rdagent', '--', '/usr/bin/remodroid-desktopd',
           '-c', '/data/remodroid/desktop.ini', 'pair', '--panel', panel,
           '--code', code, '--name', NAME, capture=True)
    login.unlink()
    print('RemoDroid pairing complete', flush=True)


def main():
    if os.geteuid() != 0 or os.uname().machine != 'x86_64':
        raise RuntimeError('this installer needs the amd64 sx host as root')
    settings = STATE / 'browser.conf'
    if not settings.is_file():
        raise RuntimeError(f'missing {settings}')
    archive = Path(sys.argv[1]).resolve()
    upload = Path(api('GET', '/v1/images/sideload')['dir'])
    upload.mkdir(parents=True, exist_ok=True)
    destination = upload / 'remote-browser-amd64.tar'
    shutil.copyfile(archive, destination.with_suffix('.tar.part'))
    destination.with_suffix('.tar.part').replace(destination)
    machine = api('GET', '/v1/portal/machine')['machine']
    if machine['name'] != 'sx':
        raise RuntimeError('refusing to deploy to a different hqnode machine')
    existing = next((c for c in machine.get('containers', []) if c['name'] == NAME), None)
    if existing is None:
        result = api('POST', '/v1/portal/containers', {
            'name': NAME, 'ssh_username': 'browser', 'ssh_password': secrets.token_urlsafe(32),
            'archive': destination.name, 'ports': [22],
            'limits': {'cpu_cores': 2, 'mem_bytes': 2 * 1024**3, 'swap_bytes': 1024**3,
                       'disk_bytes': 10 * 1024**3, 'data_bytes': 10 * 1024**3,
                       'net_quota_bytes': 1024**4}})
        container = result.get('container', result)
        cid = container['cid']
    else:
        cid = existing['cid']
        # Rerunning a failed first installation resumes configuration. Updating an
        # existing working image uses the panel's reinstall action separately.
    data = Path('/var/hqnode/containers') / cid / 'data'
    if not data.is_dir():
        raise RuntimeError(f'missing container data mount {data}')
    conf = data / 'browser.conf'
    shutil.copyfile(settings, conf)
    owner = data.stat()
    os.chown(conf, owner.st_uid, owner.st_gid)
    conf.chmod(0o644)

    # The same container-scoped egress policy as oect; no other host traffic is changed.
    shutil.copyfile('rd-killswitch', '/usr/local/sbin/rd-killswitch')
    Path('/usr/local/sbin/rd-killswitch').chmod(0o755)
    shutil.copyfile('rd-killswitch.service', '/etc/systemd/system/rd-killswitch.service')
    panel_addresses = {item[4][0] for item in socket.getaddrinfo('rm.11100.top', 443, type=socket.SOCK_STREAM)}
    rules = [f'cid = {cid}', 'scope = container', 'allow = tcp 127.0.0.1 1084',
             'allow = udp 127.0.0.1', 'allow = udp public 3478', 'allow = tcp public 3478']
    rules += [f'allow = tcp {address} 443' for address in sorted(panel_addresses)]
    for line in Path('/etc/resolv.conf').read_text().splitlines():
        fields = line.split()
        if fields and fields[0] == 'nameserver':
            rules += [f'allow = udp {fields[1]} 53', f'allow = tcp {fields[1]} 53']
    killconf = Path('/etc/rd-killswitch.conf')
    killconf.write_text('\n'.join(rules) + '\n')
    killconf.chmod(0o600)
    run('systemctl', 'daemon-reload')
    run('/usr/local/sbin/rd-killswitch', 'apply')
    run('systemctl', 'enable', '--now', 'rd-killswitch')
    pair()
    inside('systemctl', 'restart', 'rd-xorg', 'rd-video', 'rd-shell', 'rd-mirror', 'remodroid-desktopd')
    services = ['rd-xorg', 'rd-shell', 'rd-mirror', 'rd-pipewire', 'rd-pipewire-pulse', 'remodroid-desktopd']
    for _ in range(30):
        try:
            status = inside('systemctl', 'is-active', *services, capture=True).splitlines()
            if status == ['active'] * len(services):
                break
        except subprocess.CalledProcessError:
            pass
        time.sleep(2)
    else:
        raise RuntimeError('browser services did not become active')
    inside('sh', '-ec', 'DISPLAY=:0 XAUTHORITY=/run/rd-xorg.auth xdpyinfo | grep dimensions')
    inside('runuser', '-u', 'browser', '--', '/opt/cloakbrowser/chrome', '--version')
    print(f'Installed {NAME} ({cid}); all browser services active', flush=True)


if __name__ == '__main__':
    main()
