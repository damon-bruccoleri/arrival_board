#!/usr/bin/env python3
"""Generate a short mechanical flip/click WAV (no sox required)."""
import struct
import math
import sys

def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "flip.wav"
    rate = 44100
    duration = 0.04  # 40 ms
    n = int(rate * duration)
    # Short click: fast decay sine burst
    frames = []
    for i in range(n):
        t = i / rate
        env = math.exp(-t * 80)
        s = int(6000 * env * math.sin(2 * math.pi * 800 * t))
        frames.append(struct.pack("<h", max(-32767, min(32767, s))))
    data = b"".join(frames)
    # WAV header (44 bytes)
    header = (
        b"RIFF"
        + struct.pack("<I", 36 + len(data))
        + b"WAVE"
        + b"fmt "
        + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
        + b"data"
        + struct.pack("<I", len(data))
    )
    with open(out, "wb") as f:
        f.write(header)
        f.write(data)
    print("Wrote", out)

if __name__ == "__main__":
    main()
