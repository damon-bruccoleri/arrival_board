#!/usr/bin/env python3
"""Generate a small soft white steam puff PNG (reused for steam effect)."""
import math
import os

try:
    from PIL import Image
except ImportError:
    print("Install Pillow: pip install Pillow")
    exit(1)

SIZE = 96  # small soft puff
CENTER = SIZE / 2
# Soft falloff: white at center, alpha falls off with distance
RADIUS = CENTER - 4

img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
pix = img.load()
for y in range(SIZE):
    for x in range(SIZE):
        dx = x - CENTER
        dy = y - CENTER
        d = math.sqrt(dx * dx + dy * dy)
        if d >= RADIUS:
            alpha = 0
        else:
            # Smooth falloff: alpha ~ (1 - (d/r)^2)^1.5 for soft edge
            t = 1.0 - (d / RADIUS) ** 2
            t = max(0, t) ** 1.5
            alpha = int(220 * t)  # max ~220 for semi-transparent white
        pix[x, y] = (255, 255, 255, alpha)

out = os.path.join(os.path.dirname(__file__), "..", "steam_puff.png")
img.save(out)
print("Wrote", out)
