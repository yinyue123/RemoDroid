#!/usr/bin/env python3
"""Checks the RTP packetizers: encode a clip (x265 / x264), packetize it with
the real C sender (rtp_dump), depacketize here per RFC 7798 / RFC 6184, and
verify FFmpeg decodes the result to exactly the same pictures as the original.

  python rtp_depack.py --bin build/host/rtc/rtp_dump
"""
import argparse
import fractions
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

import av
import numpy as np


def encode(codec_name, path, frames=60, w=1280, h=720):
    enc = av.CodecContext.create(codec_name, "w")
    enc.width, enc.height, enc.pix_fmt = w, h, "yuv420p"
    enc.time_base = fractions.Fraction(1, 30)
    enc.framerate = 30
    if codec_name == "libx265":
        enc.options = {"preset": "ultrafast", "tune": "zerolatency", "x265-params": "keyint=30:repeat-headers=1:bframes=0:log-level=error"}
    else:
        enc.options = {"preset": "ultrafast", "tune": "zerolatency", "x264-params": "keyint=30:repeat-headers=1:bframes=0"}
    raw = []
    with open(path, "wb") as f:
        def put(pkt):
            d = bytes(pkt)
            raw.append(d)
            f.write(struct.pack(">IB", len(d), 1 if pkt.is_keyframe else 0) + d)
        for i in range(frames):
            img = np.random.default_rng(i // 10).integers(0, 255, (h, w, 3), dtype=np.uint8)  # big frames -> FUs
            img[:, : (i * 20) % w] = 0
            fr = av.VideoFrame.from_ndarray(img, format="rgb24").reformat(format="yuv420p")
            fr.pts = i
            for p in enc.encode(fr):
                put(p)
        for p in enc.encode(None):
            put(p)
    return raw


def decode_md5(codec, annexb_frames):
    dec = av.CodecContext.create(codec, "r")
    out = []
    for d in annexb_frames:
        for pkt in dec.parse(d):
            for fr in dec.decode(pkt):
                out.append(hashlib.md5(fr.to_ndarray(format="yuv420p").tobytes()).hexdigest())
    for pkt in dec.parse(None) if hasattr(dec, "parse") else []:
        for fr in dec.decode(pkt):
            out.append(hashlib.md5(fr.to_ndarray(format="yuv420p").tobytes()).hexdigest())
    for fr in dec.decode(None):
        out.append(hashlib.md5(fr.to_ndarray(format="yuv420p").tobytes()).hexdigest())
    return out


def depacketize(path, h265):
    data = open(path, "rb").read()
    off, frames, cur, fu = 0, [], b"", None
    exts_ok = True
    last_seq = None
    while off < len(data):
        n = struct.unpack(">H", data[off:off + 2])[0]
        p = data[off + 2:off + 2 + n]
        off += 2 + n
        v, x, marker = p[0] >> 6, (p[0] >> 4) & 1, p[1] >> 7
        seq = struct.unpack(">H", p[2:4])[0]
        if last_seq is not None and seq != (last_seq + 1) & 0xFFFF:
            raise SystemExit("sequence gap")
        last_seq = seq
        hl = 12
        if x:
            if p[12:14] != b"\xbe\xde":
                exts_ok = False
            hl = 16 + struct.unpack(">H", p[14:16])[0] * 4
        pl = p[hl:]
        if h265:
            t = (pl[0] >> 1) & 0x3F
            if t == 49:  # FU
                fuh = pl[2]
                s, e, ft = fuh >> 7, (fuh >> 6) & 1, fuh & 0x3F
                if s:
                    fu = bytes([(pl[0] & 0x81) | (ft << 1), pl[1]]) + pl[3:]
                else:
                    fu += pl[3:]
                if e:
                    cur += b"\x00\x00\x00\x01" + fu
                    fu = None
            elif t == 48:  # aggregation packet
                i = 2
                while i + 2 <= len(pl):
                    ln = struct.unpack(">H", pl[i:i + 2])[0]
                    cur += b"\x00\x00\x00\x01" + pl[i + 2:i + 2 + ln]
                    i += 2 + ln
            else:
                cur += b"\x00\x00\x00\x01" + pl
        else:
            t = pl[0] & 0x1F
            if t == 28:  # FU-A
                s, e, ft = pl[1] >> 7, (pl[1] >> 6) & 1, pl[1] & 0x1F
                if s:
                    fu = bytes([(pl[0] & 0xE0) | ft]) + pl[2:]
                else:
                    fu += pl[2:]
                if e:
                    cur += b"\x00\x00\x00\x01" + fu
            elif t == 24:  # STAP-A
                i = 1
                while i + 2 <= len(pl):
                    ln = struct.unpack(">H", pl[i:i + 2])[0]
                    cur += b"\x00\x00\x00\x01" + pl[i + 2:i + 2 + ln]
                    i += 2 + ln
            else:
                cur += b"\x00\x00\x00\x01" + pl
        if marker:
            frames.append(cur)
            cur = b""
        if len(p) > 1200:
            raise SystemExit(f"packet too large: {len(p)}")
    return frames, exts_ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="build/host/rtc/rtp_dump")
    a = ap.parse_args()
    ok = True
    tmp = tempfile.mkdtemp()
    for name, lib, dec in (("h265", "libx265", "hevc"), ("h264", "libx264", "h264")):
        src = os.path.join(tmp, f"{name}.frames")
        raw = encode(lib, src)
        out = os.path.join(tmp, f"{name}.rtp")
        info = subprocess.check_output([a.bin, name, src, out]).decode().strip()
        frames, exts_ok = depacketize(out, name == "h265")
        want, got = decode_md5(dec, raw), decode_md5(dec, frames)
        match = want == got and len(want) > 0
        print(f"{name}: {info} -> {len(frames)} frames depacketized, decoded {len(got)}/{len(want)} identical: {match}, "
              f"header extensions ok: {exts_ok}")
        ok &= match and exts_ok
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
