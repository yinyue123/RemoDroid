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
    if not os.environ.get('RD_URL'):  # the router agent's own API, through SSH: no panel or token needed
        try:
            with socket.create_connection(('127.0.0.1', 18795), timeout=1):
                pass
        except OSError:
            subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes',
                            '-o', 'ConnectTimeout=8', '-fN', '-L',
                            '127.0.0.1:18795:127.0.0.1:8790', 'admin@192.168.10.3'], check=True)
        os.environ['RD_URL'] = 'http://127.0.0.1:18795'
        os.environ.setdefault('RD_DEVICE', 'local')
    os.environ['NO_PROXY'] = '127.0.0.1'

if __name__ == '__main__':
    setup()
    from remodroid.cli import main
    sys.exit(main())
