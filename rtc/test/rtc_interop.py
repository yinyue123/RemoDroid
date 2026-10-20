#!/usr/bin/env python3
"""Interop test for the phone-side WebRTC stack against aiortc.

This script plays both the router agent (CTRL frames over the tunnel) and
the viewer (aiortc answers the phone's offer), then checks that:
  * ICE, DTLS-SRTP and SCTP come up,
  * H.264 RTP arrives and decodes into real frames,
  * the ctrl/input/file data channels open and round-trip messages.

  python rtc_interop.py --bin build/host/rtc/rtc_test [--keep]
"""
import argparse
import asyncio
import json
import os
import struct
import subprocess
import sys
import tempfile
import time

import av
from aiortc import RTCPeerConnection, RTCSessionDescription
from aiortc.sdp import candidate_from_sdp

T_CTRL = 0x30
T_PING = 0x20
T_PONG = 0x21


def make_clip(path, frames=90, w=640, h=360):
    """Encodes a moving test pattern with x264 (baseline, Annex-B, SPS/PPS on keyframes)."""
    codec = av.CodecContext.create("libx264", "w")
    codec.width, codec.height = w, h
    codec.pix_fmt = "yuv420p"
    codec.time_base = av.time_base if False else __import__("fractions").Fraction(1, 30)
    codec.framerate = 30
    codec.profile = "Constrained Baseline"
    codec.options = {"preset": "ultrafast", "tune": "zerolatency", "x264-params": "keyint=30:repeat-headers=1:bframes=0"}
    import numpy as np
    out = open(path, "wb")
    n = 0
    for i in range(frames):
        img = np.zeros((h, w, 3), dtype=np.uint8)
        img[:, :, 0] = (np.arange(w) + i * 4) % 256
        img[:, :, 1] = ((np.arange(h)[:, None] + i * 2) % 256)
        img[:, :, 2] = 128
        frame = av.VideoFrame.from_ndarray(img, format="rgb24").reformat(format="yuv420p")
        frame.pts = i
        for pkt in codec.encode(frame):
            data = bytes(pkt)
            out.write(struct.pack(">IB", len(data), 1 if pkt.is_keyframe else 0) + data)
            n += 1
    for pkt in codec.encode(None):
        data = bytes(pkt)
        out.write(struct.pack(">IB", len(data), 1 if pkt.is_keyframe else 0) + data)
        n += 1
    out.close()
    return n


def tun_frame(t, payload=b""):
    hdr = bytes([t, 0, 0])
    return struct.pack(">H", len(hdr) + len(payload)) + hdr + payload


async def run(args):
    tmp = tempfile.mkdtemp(prefix="rtc-interop-")
    clip = os.path.join(tmp, "clip.frames")
    nclip = make_clip(clip)
    port = 27000 + os.getpid() % 1000
    phone = subprocess.Popen(
        [args.bin, "--listen", f"tcp:127.0.0.1:{port}", "--frames", clip, "--direct", "1", "--audio", "1", "--seconds", "40",
         "--log", "debug" if args.verbose else "info"],
        stdout=subprocess.PIPE, stderr=None if args.verbose else subprocess.DEVNULL, text=True)
    results = {"clip_frames": nclip}
    try:
        for _ in range(50):
            try:
                reader, writer = await asyncio.open_connection("127.0.0.1", port)
                break
            except OSError:
                await asyncio.sleep(0.1)
        else:
            raise RuntimeError("phone tunnel did not come up")

        pc = RTCPeerConnection()
        decoded = []
        audio = []
        channels = {}
        got = []
        opened = asyncio.Event()

        @pc.on("track")
        def on_track(track):
            async def consume():
                while True:
                    try:
                        f = await track.recv()
                    except Exception:
                        return
                    if track.kind == "audio":
                        audio.append(f.samples)
                    else:
                        decoded.append((f.width, f.height))
            asyncio.ensure_future(consume())

        @pc.on("datachannel")
        def on_dc(ch):
            channels[ch.label] = ch

            @ch.on("message")
            def on_message(m):
                got.append((ch.label, m))

            if {"ctrl", "input", "file"} <= set(channels):
                opened.set()

        def send_ctrl(obj):
            writer.write(tun_frame(T_CTRL, json.dumps(obj).encode()))

        sid = "s_interop"
        send_ctrl({"t": "session.start", "sid": sid, "d": {"sid": sid, "ice_servers": [], "quality": {"mode": "auto"}}})

        buf = b""
        answered = False
        t0 = time.time()
        pending_cands = []

        async def read_tunnel():
            nonlocal buf, answered
            while True:
                chunk = await reader.read(65536)
                if not chunk:
                    return
                buf += chunk
                while len(buf) >= 2:
                    n = struct.unpack(">H", buf[:2])[0]
                    if len(buf) < 2 + n:
                        break
                    fr, buf = buf[2:2 + n], buf[2 + n:]
                    t = fr[0]
                    if t == T_PING:
                        writer.write(tun_frame(T_PONG))
                    elif t == T_CTRL:
                        m = json.loads(fr[3:])
                        if m.get("t") == "signal":
                            d = m["d"]
                            if d["type"] == "offer":
                                results["offer_has_h264"] = "H264/90000" in d["sdp"]
                                results["offer_has_opus"] = "opus/48000/2" in d["sdp"]
                                await pc.setRemoteDescription(RTCSessionDescription(d["sdp"], "offer"))
                                answer = await pc.createAnswer()
                                await pc.setLocalDescription(answer)
                                send_ctrl({"t": "signal", "sid": sid, "d": {"type": "answer", "sdp": pc.localDescription.sdp}})
                                answered = True
                                for c in pending_cands:
                                    await pc.addIceCandidate(c)
                            elif d["type"] == "candidate" and d.get("candidate"):
                                c = candidate_from_sdp(d["candidate"].split(":", 1)[1])
                                c.sdpMid, c.sdpMLineIndex = d.get("sdpMid", "0"), d.get("sdpMLineIndex", 0)
                                results.setdefault("phone_candidates", []).append(d["candidate"])
                                if answered:
                                    await pc.addIceCandidate(c)
                                else:
                                    pending_cands.append(c)
                        elif m.get("t") == "session.ended":
                            results["ended"] = m["d"].get("reason")

        rt = asyncio.ensure_future(read_tunnel())
        try:
            await asyncio.wait_for(opened.wait(), 20)
            results["channels_open_s"] = round(time.time() - t0, 2)
        except asyncio.TimeoutError:
            results["channels_open_s"] = None

        if "ctrl" in channels:
            channels["ctrl"].send("hello")
            channels["input"].send(bytes([1, 0, 0, 1, 0x40, 0, 0x40, 0, 0xff, 0xff, 0, 0, 0, 1]))
            channels["file"].send(b"\x00\x00\x00\x07\x00\x00\x00\x00" + b"x" * 16000)
        await asyncio.sleep(6)
        results["decoded_frames"] = len(decoded)
        results["frame_size"] = decoded[-1] if decoded else None
        results["audio_frames"] = len(audio)
        results["dc_replies"] = [m for _, m in got]
        stats = await pc.getStats()
        for s in stats.values():
            if s.type == "inbound-rtp":
                results["inbound_packets"] = s.packetsReceived
                results["inbound_lost"] = s.packetsLost
        phone.send_signal(15)
        out, _ = phone.communicate(timeout=10)
        last = [l for l in out.splitlines() if l.startswith("{")]
        results["phone"] = json.loads(last[-1]) if last else None
        await pc.close()
        rt.cancel()
    finally:
        if phone.poll() is None:
            phone.kill()

    print(json.dumps(results, indent=2))
    ok = (results.get("channels_open_s") is not None and results["decoded_frames"] >= 60
          and "echo:hello" in results["dc_replies"]
          and any('"label":"input"' in m and '"binary":1' in m and '"n":14' in m for m in results["dc_replies"])
          and any('"label":"file"' in m and '"n":16008' in m for m in results["dc_replies"])
          and results["frame_size"] == (640, 360)
          and results["audio_frames"] >= 150)  # 20 ms packets for 6+ s
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/host/rtc/rtc_test")
    ap.add_argument("--verbose", action="store_true")
    sys.exit(asyncio.run(run(ap.parse_args())))
