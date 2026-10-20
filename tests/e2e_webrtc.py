#!/usr/bin/env python3
"""Full-path WebRTC test (plan §3, Path A):

  aiortc viewer --(ws /ws/viewer)--> remodroid-server --(ws /ws/agent)--> remodroid-agent
        ^                                                                     | tunnel (TCP link)
        +-------------------- UDP media relayed by the agent <------- rtc_test (phone stack)

Checks login, device detection, signaling through server and agent, ICE over
the agent's UDP relay, DTLS-SRTP, decoded H.264 frames and data channels.

  python tests/e2e_webrtc.py --bin build/host
"""
import argparse
import asyncio
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "rtc", "test"))
from rtc_interop import make_clip  # noqa: E402

from aiortc import RTCPeerConnection, RTCSessionDescription  # noqa: E402
from aiortc.sdp import candidate_from_sdp  # noqa: E402


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class WS:
    """Minimal WebSocket client (text frames, masks, answers pings)."""

    async def connect(self, port, path, headers):
        self.r, self.w = await asyncio.open_connection("127.0.0.1", port)
        key = base64.b64encode(os.urandom(16)).decode()
        extra = "".join(f"{k}: {v}\r\n" for k, v in headers.items())
        self.w.write((f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                      f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n{extra}\r\n").encode())
        head = await self.r.readuntil(b"\r\n\r\n")
        status = int(head.split()[1])
        if status != 101:
            raise RuntimeError(f"upgrade failed: HTTP {status}")
        self.q = asyncio.Queue()
        self.task = asyncio.ensure_future(self._read())
        return self

    async def _read(self):
        buf = b""
        while True:
            d = await self.r.read(65536)
            if not d:
                await self.q.put(None)
                return
            buf += d
            while len(buf) >= 2:
                op, ln, off = buf[0] & 15, buf[1] & 127, 2
                if ln == 126:
                    if len(buf) < 4:
                        break
                    ln, off = struct.unpack(">H", buf[2:4])[0], 4
                elif ln == 127:
                    if len(buf) < 10:
                        break
                    ln, off = struct.unpack(">Q", buf[2:10])[0], 10
                if len(buf) < off + ln:
                    break
                payload, buf = buf[off:off + ln], buf[off + ln:]
                if op == 1:
                    await self.q.put(json.loads(payload))
                elif op == 9:
                    self._send(10, payload)

    def _send(self, op, payload):
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x80 | op]) + (bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack(">H", n))
        self.w.write(hdr + mask + bytes(b ^ mask[i & 3] for i, b in enumerate(payload)))

    def send(self, obj):
        self._send(1, json.dumps(obj).encode())

    async def wait(self, pred, timeout=10, what="message"):
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError(what)
            m = await asyncio.wait_for(self.q.get(), left)
            if m is None:
                raise RuntimeError("websocket closed")
            if pred(m):
                return m
            if m.get("t") == "signal":
                await self.on_signal(m)

    async def on_signal(self, m):
        pass


async def run(args):
    bindir = os.path.abspath(args.bin)
    d = tempfile.mkdtemp(prefix="remodroid-e2e-")
    http_port, tun_port, turn_port = free_port(), free_port(), free_port()
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.connect(("10.255.255.255", 1))  # no traffic: just picks the outgoing interface
    host_ip = probe.getsockname()[0]
    probe.close()
    hp = lambda pw: subprocess.check_output([f"{bindir}/remodroid-server", "hashpw", pw]).decode().strip()
    os.makedirs(f"{d}/web")
    open(f"{d}/web/index.html", "w").write("ok")
    open(f"{d}/server.ini", "w").write(f"""[http]
listen = 127.0.0.1:{http_port}
web_root = {d}/web
[users]
me = {hp('pw')}
[devices]
router1 = {hp('dev')}
[keepalive]
ping_seconds = 5
timeout_seconds = 20
heartbeat_seconds = 5
[turn]
listen = 0.0.0.0:{turn_port}
public_ip = {host_ip}
relay_min_port = 40000
relay_max_port = 40100
urls = turn:{host_ip}:{turn_port}?transport=udp
shared_secret = test-turn-secret
""")
    open(f"{d}/agent.ini", "w").write(f"""[server]
url = ws://127.0.0.1:{http_port}/ws/agent
device_id = router1
device_secret = dev
[agent]
socket = {d}/agent.sock
adbkey = {d}/adbkey
log_level = {'debug' if args.verbose else 'info'}
[phone]
link = tcp:127.0.0.1:{tun_port}
""")
    clip = f"{d}/clip.frames"
    make_clip(clip)
    out = None if args.verbose else subprocess.DEVNULL
    procs = [
        subprocess.Popen([f"{bindir}/remodroid-server", "-c", f"{d}/server.ini"], stderr=out),
        subprocess.Popen([f"{bindir}/rtc/rtc_test", "--listen", f"tcp:127.0.0.1:{tun_port}", "--frames", clip,
                          "--direct", "0", "--seconds", "60", "--log", "debug" if args.verbose else "info"],
                         stdout=subprocess.PIPE, stderr=out, text=True),
    ]
    time.sleep(0.3)
    procs.append(subprocess.Popen([f"{bindir}/remodroid-agent", "-c", f"{d}/agent.ini"], stderr=out))
    res = {}
    try:
        await asyncio.sleep(0.5)
        # login: the login page's POST -> cookie
        r, w = await asyncio.open_connection("127.0.0.1", http_port)
        body = json.dumps({"user": "me", "password": "pw"}).encode()
        w.write(f"POST /login HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n".encode() + body)
        head = (await r.readuntil(b"\r\n\r\n")).decode()
        cookie = [l.split(":", 1)[1].split(";")[0].strip() for l in head.split("\r\n") if l.lower().startswith("set-cookie")][0]
        w.close()

        ws = await WS().connect(http_port, "/ws/viewer", {"Cookie": cookie, "Origin": f"http://127.0.0.1:{http_port}"})
        await ws.wait(lambda m: m["t"] in ("devices", "device.update") and any(
            dv.get("online") and dv["phone"].get("server") == "running"
            for dv in (m["d"]["devices"] if m["t"] == "devices" else [m["d"]["patch"]])), 15, "phone server running")
        res["device_ready"] = True

        pc = RTCPeerConnection()
        frames, channels, replies = [], {}, []
        opened = asyncio.Event()

        @pc.on("track")
        def on_track(track):
            async def eat():
                while True:
                    try:
                        f = await track.recv()
                    except Exception:
                        return
                    frames.append((f.width, f.height))
            asyncio.ensure_future(eat())

        @pc.on("datachannel")
        def on_dc(ch):
            channels[ch.label] = ch
            ch.on("message", lambda m: replies.append(m))
            if {"ctrl", "input", "file"} <= set(channels):
                opened.set()

        sid = None
        pending = []

        async def on_signal(m):
            sd = m["d"]
            if sd["type"] == "offer":
                await pc.setRemoteDescription(RTCSessionDescription(sd["sdp"], "offer"))
                await pc.setLocalDescription(await pc.createAnswer())
                ws.send({"t": "signal", "d": {"type": "answer", "sdp": pc.localDescription.sdp}})
                for c in pending:
                    await pc.addIceCandidate(c)
                pending.clear()
            elif sd["type"] == "candidate" and sd.get("candidate"):
                res.setdefault("phone_candidates", []).append(sd["candidate"])
                c = candidate_from_sdp(sd["candidate"].split(":", 1)[1])
                c.sdpMid, c.sdpMLineIndex = "0", 0
                if pc.remoteDescription:
                    await pc.addIceCandidate(c)
                else:
                    pending.append(c)

        ws.on_signal = on_signal
        ws.send({"t": "session.request", "id": 1, "d": {"device": "router1", "quality": {"mode": "auto"}, "force_relay": args.relay}})
        ack = await ws.wait(lambda m: m["t"] in ("ack", "error") and m.get("id") == 1, 10, "session ack")
        res["sid"] = sid = ack["d"].get("sid")
        t0 = time.time()

        async def pump():
            while True:
                try:
                    await ws.wait(lambda m: False, 3600)
                except Exception:
                    return
        pumper = asyncio.ensure_future(pump())
        await asyncio.wait_for(opened.wait(), 20)
        res["connect_s"] = round(time.time() - t0, 2)
        channels["ctrl"].send("ping-through-relay")
        await asyncio.sleep(5)
        res["decoded_frames"] = len(frames)
        res["frame_size"] = list(frames[-1]) if frames else None
        res["replies"] = replies
        procs[1].send_signal(10)  # SIGUSR1: the phone prints its stats while still connected
        await asyncio.sleep(0.5)
        pumper.cancel()
        await pc.close()
    finally:
        procs[1].send_signal(15)
        try:
            pout, _ = procs[1].communicate(timeout=10)
            st = [json.loads(l) for l in (pout or "").splitlines() if l.startswith("{")]
            res["phone"] = max(st, key=lambda x: x["packets"]) if st else None
        except Exception:
            pass
        for p in procs:
            if p.poll() is None:
                p.terminate()
        for p in procs:
            try:
                p.wait(5)
            except Exception:
                p.kill()
        shutil.rmtree(d, ignore_errors=True)
    print(json.dumps(res, indent=2))
    ok = (res.get("decoded_frames", 0) >= 60 and "echo:ping-through-relay" in res.get("replies", [])
          and res.get("phone", {}).get("local") == ("relay" if args.relay else "host"))
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/host")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--relay", action="store_true", help="phone uses only TURN relay candidates (built-in TURN)")
    sys.exit(asyncio.run(run(ap.parse_args())))
