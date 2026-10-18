"""The verified connection for this phone; usable as the rd CLI entrypoint."""
import os
from pathlib import Path
import socket
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'python'))


def setup():
    if os.readlink('/proc/self/ns/net') != os.readlink('/proc/1/ns/net'):
        os.execvp('nsenter', ['nsenter', '-t', '1', '-n', sys.executable, *sys.argv])
    if not os.environ.get('RD_URL'):
        try:
            with socket.create_connection(('127.0.0.1', 18790), timeout=1):
                pass
        except OSError:
            subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes',
                            '-o', 'ConnectTimeout=8', '-fN', '-L',
                            '127.0.0.1:18790:127.0.0.1:5580', 'sz'], check=True)
        os.environ['RD_URL'] = 'http://127.0.0.1:18790'
    os.environ.setdefault('RD_DEVICE', 'rt-ac86u-89d0')
    if not os.environ.get('RD_TOKEN'):
        os.environ['RD_TOKEN'] = Path('/root/.config/remodroid/phone-token').read_text().strip()
    os.environ['NO_PROXY'] = '127.0.0.1,47.107.89.35'


if __name__ == '__main__':
    setup()
    from remodroid.cli import main
    sys.exit(main())
