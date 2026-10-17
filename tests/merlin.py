#!/usr/bin/env python3
"""Merlin packaging tests. Lifecycle checks run in an isolated Linux tree, never /koolshare."""
import base64
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DEPLOY = ROOT / 'packaging/merlin/deploy'


def run(*args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, **kwargs)


class MerlinTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='remodroid-merlin-test-')
        self.work = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def build(self, arch='armv7'):
        bins, phone = self.work / 'bin', self.work / 'phone'
        bins.mkdir(); phone.mkdir()
        (bins / 'remodroid-agent').write_text('#!/bin/sh\nexit 0\n')
        (bins / 'remodroid-server').write_text('must not ship in the Merlin agent package')
        (phone / 'remodroid-server.jar').write_bytes(b'fixture jar')
        for abi in ('arm64-v8a', 'armeabi-v7a'):
            (phone / abi).mkdir()
            (phone / abi / 'libremodroid.so').write_bytes(b'fixture lib')
        env = dict(os.environ, BIN=str(bins), PHONE=str(phone), OUT=str(self.work / 'out'))
        result = run('sh', str(ROOT / 'packaging/merlin/build.sh'), '0.3.4', arch, env=env)
        self.assertEqual(result.returncode, 0, result.stderr)
        return self.work / f'out/remodroid-0.3.4-merlin-{arch}.tar.gz'

    def test_archive_contract(self):
        archive = self.build()
        with tarfile.open(archive) as tf:
            members = {x.name: x for x in tf.getmembers()}
            for name in ('install.sh', 'scripts/remodroid_control.sh', 'scripts/uninstall_remodroid.sh',
                         'webs/Module_remodroid.asp', 'res/remodroid.js',
                         'init.d/V99remodroid.sh', 'init.d/T01remodroid.sh',
                         'remodroid/phone/arm64-v8a/libremodroid.so',
                         'remodroid/phone/armeabi-v7a/libremodroid.so'):
                self.assertIn('remodroid/' + name, members)
            self.assertTrue(members['remodroid/install.sh'].mode & 0o100)
            # The armv7 package targets 32-bit ARM routers, not HND (that is aarch64).
            self.assertEqual(tf.extractfile('remodroid/.valid').read().strip(), b'arm')
            self.assertTrue(all(n == 'remodroid' or n.startswith('remodroid/') for n in members))
            self.assertFalse(any('..' in Path(n).parts or '/._' in n for n in members))
            self.assertNotIn('remodroid/remodroid/agent.ini', members)
            self.assertNotIn('remodroid/remodroid/bin/remodroid-server', members)
            self.assertFalse(any('/web/' in name for name in members))
            config = tf.extractfile('remodroid/remodroid/agent.ini.example').read().decode()
            self.assertIn('/jffs/configs/remodroid/adbkey', config)
            self.assertIn('device_secret = ', config)

    def test_aarch64_agent_package(self):
        with tarfile.open(self.build('aarch64')) as tf:
            self.assertEqual(tf.extractfile('remodroid/remodroid/arch').read().strip(), b'aarch64')
            self.assertEqual(tf.extractfile('remodroid/.valid').read().strip(), b'hnd')
            self.assertIn('remodroid/remodroid/bin/remodroid-agent', tf.getnames())
            self.assertFalse(any(name.endswith('/remodroid-server') for name in tf.getnames()))

    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('jq'),
                         'arch-guard check requires Linux and jq')
    def test_arch_mismatch_refused(self):
        # The RT-AC86U regression: an armv7 package must be refused on a 64-bit
        # router instead of installing and crash-looping at runtime.
        stage = self.work / 'stage'; stage.mkdir()
        with tarfile.open(self.build('armv7')) as tf:
            tf.extractall(stage, filter='data')
        package = stage / 'remodroid'
        fs = self.work / 'root'; fs.mkdir()
        ks = fs / 'koolshare'; ks.mkdir()
        for directory in ('scripts', 'webs', 'res', 'init.d', 'bin'):
            (ks / directory).mkdir()
        (ks / 'scripts/base.sh').write_text('# fixture\n')
        (ks / 'bin/jq').symlink_to(shutil.which('jq'))
        (fs / 'jffs').mkdir(); (fs / 'tmp').mkdir()
        for f in package.rglob('*'):
            if f.is_file() and f.suffix in ('.sh', '.awk', '.example'):
                text = re.sub(r'/koolshare|/jffs|/tmp/remodroid',
                              lambda m: str(fs) + m[0], f.read_text())
                f.write_text(text)
        env = dict(os.environ, REMODROID_DEV_ARCH='aarch64')
        result = run('sh', str(package / 'install.sh'), env=env)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('aarch64', result.stdout + result.stderr)
        # A refused install must not have written anything into the tree.
        self.assertFalse((ks / 'remodroid').exists())
        self.assertFalse((ks / 'scripts/remodroid_control.sh').exists())

    def test_normalize_validation(self):
        original = (ROOT / 'deploy/agent.ini.example').read_text()
        result = run('awk', '-f', str(DEPLOY / 'remodroid/normalize.awk'), input=original)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('files_dir = /koolshare/remodroid/phone', result.stdout)
        self.assertNotIn('/usr/share/remodroid', result.stdout)
        self.assertIn('socket = /tmp/remodroid/agent.sock', result.stdout)
        for invalid in ('oops\n', '[server]\nx = 1\nx = 2\n[agent]\n',
                        '[server]\nx = ' + 'a'*1024 + '\n[agent]\n',
                        '[server]\n[agent]\n[agent]\n'):
            result = run('awk', '-f', str(DEPLOY / 'remodroid/normalize.awk'), input=invalid)
            self.assertNotEqual(result.returncode, 0)

    @unittest.skipUnless(sys.platform.startswith('linux') and shutil.which('cc') and shutil.which('jq'),
                         'lifecycle fixture requires Linux /proc, cc, and jq')
    def test_isolated_lifecycle(self):
        archive = self.build()
        stage = self.work / 'stage'; stage.mkdir()
        with tarfile.open(archive) as tf:
            tf.extractall(stage, filter='data')
        package = stage / 'remodroid'
        fs = self.work / 'root'; fs.mkdir()
        ks = fs / 'koolshare'; ks.mkdir()
        configdir = fs / 'jffs/configs/remodroid'
        bindir = ks / 'bin'; bindir.mkdir()
        for directory in ('scripts', 'webs', 'res', 'init.d'):
            (ks / directory).mkdir()
        (ks / 'scripts/base.sh').write_text('# fixture\n')
        (fs / 'jffs').mkdir(); (fs / 'tmp').mkdir()
        # Rewrite fixed production paths ONLY inside this isolated test copy.
        for f in package.rglob('*'):
            if f.is_file() and f.suffix in ('.sh', '.awk', '.example'):
                text = f.read_text()
                text = re.sub(r'/koolshare|/jffs|/tmp/remodroid', lambda m: str(fs) + m[0], text)
                f.write_text(text)
        (bindir / 'jq').symlink_to(shutil.which('jq'))
        (bindir / 'base64_encode').write_text('#!/bin/sh\nbase64\n')
        (bindir / 'pidof').write_text('#!/bin/sh\nexit 1\n')
        (bindir / 'dbus').write_text('''#!/usr/bin/env python3
import os, json, sys
p=os.environ['FIXTURE_DB']
try: d=json.load(open(p))
except FileNotFoundError: d={}
op=sys.argv[1]; arg=sys.argv[2]
if op=='get': print(d.get(arg,''))
elif op=='set':
 k,v=arg.split('=',1); d[k]=v
elif op=='remove': d.pop(arg,None)
if op!='get':
 with open(p,'w') as f: json.dump(d,f)
''')
        (bindir / 'curl').write_text('''#!/usr/bin/env python3
import os,sys
with open(os.environ['FIXTURE_RESPONSE'],'w') as f:
 f.write(sys.argv[sys.argv.index('--data-binary')+1])
''')
        for f in bindir.iterdir():
            if not f.is_symlink(): f.chmod(0o755)
        c = self.work / 'agent.c'
        c.write_text('#include <stdio.h>\n#include <string.h>\n#include <unistd.h>\n'
                     'int main(int argc,char**argv){if(argc>1&&!strcmp(argv[1],"-h"))return 0;'
                     'if(argc>3&&!strcmp(argv[3],"enroll")){FILE*f=fopen(argv[2],"w");'
                     'if(!f)return 1;fputs("[server]\\nurl = ws://127.0.0.1:9/ws/agent\\n'
                     'device_id = fixture\\ndevice_secret = fixture\\n[agent]\\n",f);fclose(f);return 0;}'
                     'puts("fixture started");fflush(stdout);while(1)pause();}\n')
        subprocess.run(['cc', str(c), '-o', str(package / 'remodroid/bin/remodroid-agent')], check=True)
        # The fixture package is armv7; tell install.sh's arch guard to treat this
        # host as armv7 so the lifecycle runs regardless of the CI runner's real arch.
        env = dict(os.environ, FIXTURE_DB=str(self.work/'db.json'), FIXTURE_RESPONSE=str(self.work/'rpc'),
                   REMODROID_DEV_ARCH='armv7')
        def install():
            result = run('sh', str(package/'install.sh'), env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        install()
        ctl = ks / 'scripts/remodroid_control.sh'
        def control(action, success=True):
            result = run('sh', str(ctl), action, env=env, timeout=15)
            self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
            return json.loads(result.stdout)
        try:
            self.assertFalse(control('status')['running'])
            self.assertFalse(control('status')['autostart'])
            control('start', success=False)  # Empty credentials must not launch anything.
            original = (configdir/'agent.ini').read_text()
            # INI templates have inline comments after empty credential values.
            # A configured URL must not make those comments count as credentials.
            (configdir/'agent.ini').write_text(original.replace('url = ', 'url = ws://127.0.0.1:9/ws/agent '))
            control('start', success=False)
            (configdir/'agent.ini').write_text(original)
            def save(text, success=True):
                encoded = base64.b64encode(text.encode()).decode()
                subprocess.run([str(bindir/'dbus'), 'set', 'remodroid_payload='+encoded], env=env, check=True)
                return control('save', success=success)
            def enroll(data, success=False):
                encoded = base64.b64encode(json.dumps(data).encode()).decode()
                subprocess.run([str(bindir/'dbus'), 'set', 'remodroid_enrollment='+encoded], env=env, check=True)
                response = control('enroll', success=success)
                self.assertNotIn('remodroid_enrollment', json.loads((self.work/'db.json').read_text()))
                return response
            for data in ({'panel': 'http://localhost;touch /tmp/no', 'code': 'ABCD-EFGH'},
                         {'panel': 'http://localhost', 'code': '$(touch /tmp/no)'},
                         {'panel': [], 'code': 'ABCD-EFGH'}):
                enroll(data)
                self.assertEqual((configdir/'agent.ini').read_text(), original)
            save('[broken', success=False)
            self.assertEqual((configdir/'agent.ini').read_text(), original)
            agent = package / 'remodroid/bin/remodroid-agent'
            installed_agent = ks / 'remodroid/bin/remodroid-agent'
            fixture = '#!/bin/sh\n[ "$3" = enroll ] || exit 2\nprintf "refused fixture code\\n"\nexit 1\n'
            installed_agent.write_text(fixture)
            installed_agent.chmod(0o755)
            enroll({'panel':'http://127.0.0.1:9', 'code':'ABCD-EFGH'})
            self.assertEqual((configdir/'agent.ini').read_text(), original)
            self.assertTrue((fs/'tmp/remodroid/enroll.log').exists())
            self.assertFalse((fs/'tmp/remodroid/enroll.ini').exists())
            shutil.copyfile(agent, installed_agent)
            bound = enroll({'panel':'http://127.0.0.1:9', 'code':'ABCD-EFGH'}, success=True)
            self.assertTrue(bound['running'])
            self.assertIn('device_secret = fixture', bound['config'])
            self.assertIn('files_dir = ', bound['config'])
            enroll({'panel':'http://127.0.0.1:9', 'code':'ABCD-EFGH'})  # Stop before rebind.
            control('stop')
            save(original)

            marker = self.work/'must-not-exist'
            literal = f'$(touch {marker})'
            config = '[server]\nurl = ws://127.0.0.1:9/ws/agent\ndevice_id = '+literal+'\ndevice_secret = fixture\n[agent]\nfiles_dir = /wrong\n'
            saved = save(config)
            self.assertIn(literal, saved['config'])
            self.assertFalse(marker.exists())
            self.assertNotIn('/wrong', saved['config'])
            self.assertEqual((configdir/'agent.ini').stat().st_mode & 0o777, 0o600)
            self.assertTrue(control('start')['running'])
            first = control('status')['pid']
            self.assertEqual(control('start')['pid'], first)  # No duplicate daemon.
            second = control('restart')['pid']
            self.assertNotEqual(first, second)
            self.assertTrue(control('auto_on')['autostart'])
            result = run('sh', str(ctl), '12345', 'status', env=env, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            rpc = json.loads(base64.b64decode((self.work/'rpc').read_text()))
            self.assertTrue(rpc['running'])
            self.assertEqual(rpc['config'], '')  # Polling never returns the secret config.
            self.assertFalse(control('stop')['running'])
            config_before = (configdir/'agent.ini').read_text()
            (configdir/'adbkey').write_text('fixture private key')
            install()  # Upgrade preserves identity, keys, and autostart; does not start.
            self.assertEqual((configdir/'agent.ini').read_text(), config_before)
            self.assertEqual((configdir/'adbkey').read_text(), 'fixture private key')
            self.assertFalse(control('status')['running'])
            self.assertTrue(control('boot')['running'])
            control('stop')
            # A stale PID that refers to this test process must never be killed.
            (fs/'tmp/remodroid/agent.pid').write_text(str(os.getpid()))
            self.assertFalse(control('stop')['running'])
            result = run('sh', str(ks/'scripts/uninstall_remodroid.sh'), env=env, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((configdir/'agent.ini').exists())
            self.assertTrue((configdir/'adbkey').exists())
            self.assertFalse((configdir/'autostart').exists())
            self.assertFalse((ks/'webs/Module_remodroid.asp').exists())
            self.assertFalse((ks/'remodroid').exists())
        finally:
            if ctl.exists(): run('sh', str(ctl), 'stop', env=env, timeout=15)


if __name__ == '__main__':
    unittest.main(verbosity=2)
