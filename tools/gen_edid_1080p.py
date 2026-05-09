#!/usr/bin/env python3
"""Generate a 256-byte EDID: 1920x1080@60Hz + CEA extension with HDMI audio.

The 128-byte firmware EDID we used for stable KMS mode must include a CEA-861
extension with an HDMI Vendor Specific Data Block and Audio Data Block.
Otherwise vc4 HDMI audio fails (PipeWire shows only auto_null; aplay returns
errors).  Not specific to Trixie, but full KMS + PipeWire makes the failure
obvious (no sink).
"""
import sys


def checksum(block):
    """Last byte of each 128-byte block."""
    block[127] = (256 - (sum(block[:127]) % 256)) % 256


# --- Base block (0-127) ---
edid = bytearray(128)

edid[0:8] = b"\x00\xff\xff\xff\xff\xff\xff\x00"
edid[8] = 0x31
edid[9] = 0xD8
edid[10:12] = b"\x00\x00"
edid[12:16] = b"\x00\x00\x00\x00"
edid[16] = 1
edid[17] = 34
edid[18] = 1
edid[19] = 3

edid[20] = 0x80
edid[21] = 53
edid[22] = 30
edid[23] = 120
edid[24] = 0x0A

edid[25:35] = b"\xEE\x91\xA3\x54\x4C\x99\x26\x0F\x50\x54"
edid[35:38] = b"\x00\x00\x00"
for i in range(38, 54, 2):
    edid[i] = 0x01
    edid[i + 1] = 0x01

edid[54] = 0x02
edid[55] = 0x3A
edid[56] = 0x80
edid[57] = 0x18
edid[58] = 0x71
edid[59] = 0x38
edid[60] = 0x2D
edid[61] = 0x40
edid[62] = 0x58
edid[63] = 0x2C
edid[64] = 0x45
edid[65] = 0x00
edid[66] = 0x12
edid[67] = 0x2C
edid[68] = 0x21
edid[69] = 0x00
edid[70] = 0x00
edid[71] = 0x1E

edid[72:76] = b"\x00\x00\x00\xFC"
edid[76] = 0x00
name = b"Linux FHD\x0a"
edid[77 : 77 + len(name)] = name
for i in range(77 + len(name), 90):
    edid[i] = 0x20

edid[90:94] = b"\x00\x00\x00\xFD"
edid[94] = 0x00
edid[95] = 56
edid[96] = 62
edid[97] = 30
edid[98] = 70
edid[99] = 0x11
edid[100:108] = b"\x00\x0A\x20\x20\x20\x20\x20\x20"

edid[108:126] = b"\x00" * 18

edid[126] = 1  # one CEA extension follows
checksum(edid)

# --- CEA extension (128-255) ---
cea = bytearray(128)
cea[0] = 0x02  # CEA 861 extension tag
cea[1] = 0x03  # revision 3
# Byte 3: Underscan | Basic audio | YCbCr 4:4:4 | YCbCr 4:2:2
cea[3] = 0x07

# Data blocks start at offset 4; DTD index follows last data byte + padding.
off = 4

# HDMI Vendor Specific Data Block (IEEE OUI 0x000C03); SPA 1.0.0.0
# Header: block type 3, payload length 5
cea[off] = (3 << 5) | 5
off += 1
cea[off : off + 5] = bytes([0x03, 0x0C, 0x00, 0x10, 0x00])
off += 5

# Audio Data Block: one short LPCM descriptor (2 ch, 32/44.1/48 kHz, 16/20/24-bit)
cea[off] = (1 << 5) | 3
off += 1
cea[off : off + 3] = bytes([0x09, 0x7F, 0x07])
off += 3

# Speaker allocation block (FL/FR)
cea[off] = (4 << 5) | 3
off += 1
cea[off : off + 3] = bytes([0x01, 0x00, 0x00])
off += 3

cea[2] = off  # detailed timings start here (none valid — filler zeros for parsers)

checksum(cea)

full = edid + cea

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/1080p.bin"
with open(out, "wb") as f:
    f.write(full)
print(f"Wrote {len(full)}-byte EDID to {out}")
print(f"  base checksum=0x{edid[127]:02x}  cea checksum=0x{cea[127]:02x}")
