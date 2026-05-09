#!/usr/bin/env python3
"""Generate a minimal 128-byte EDID for 1920x1080@60Hz."""
import sys

edid = bytearray(128)

# EDID header
edid[0:8] = b"\x00\xff\xff\xff\xff\xff\xff\x00"

# Manufacturer: LNX
edid[8] = 0x31; edid[9] = 0xD8
edid[10:12] = b"\x00\x00"    # product
edid[12:16] = b"\x00\x00\x00\x00"  # serial
edid[16] = 1; edid[17] = 34  # week 1, year 2024
edid[18] = 1; edid[19] = 3   # EDID 1.3

edid[20] = 0x80  # digital input
edid[21] = 53; edid[22] = 30  # image size cm
edid[23] = 120  # gamma 2.2
edid[24] = 0x0A  # features

# Color characteristics (sRGB-ish)
edid[25:35] = b"\xEE\x91\xA3\x54\x4C\x99\x26\x0F\x50\x54"

# Established / standard timings (none)
edid[35:38] = b"\x00\x00\x00"
for i in range(38, 54, 2):
    edid[i] = 0x01; edid[i+1] = 0x01

# DTD 1: 1920x1080@60Hz  pixel clock 148.5 MHz
# 148500/10 = 14850 = 0x3A02
edid[54] = 0x02; edid[55] = 0x3A
edid[56] = 0x80  # H active low 8 (1920 & 0xFF)
edid[57] = 0x18  # H blanking low 8 (280 & 0xFF)
edid[58] = 0x71  # H active hi4 | H blank hi4
edid[59] = 0x38  # V active low 8 (1080 & 0xFF)
edid[60] = 0x2D  # V blanking low 8 (45 & 0xFF)
edid[61] = 0x40  # V active hi4 | V blank hi4
edid[62] = 0x58  # H sync offset (88)
edid[63] = 0x2C  # H sync width (44)
edid[64] = 0x45  # V sync offset 4 | V sync width 5
edid[65] = 0x00
edid[66] = 0x12; edid[67] = 0x2C; edid[68] = 0x21  # image size
edid[69] = 0x00; edid[70] = 0x00  # borders
edid[71] = 0x1E  # non-interlaced

# DTD 2: Monitor name
edid[72:76] = b"\x00\x00\x00\xFC"
edid[76] = 0x00
name = b"Linux FHD\x0a"
edid[77:77+len(name)] = name
for i in range(77+len(name), 90):
    edid[i] = 0x20

# DTD 3: Range limits
edid[90:94] = b"\x00\x00\x00\xFD"
edid[94] = 0x00
edid[95] = 56; edid[96] = 62   # V rate range
edid[97] = 30; edid[98] = 70   # H rate range kHz
edid[99] = 0x11                 # max pixel clock /10 = 170 MHz
edid[100:108] = b"\x00\x0A\x20\x20\x20\x20\x20\x20"

# DTD 4: empty
edid[108:126] = b"\x00" * 18

edid[126] = 0  # no extensions
edid[127] = (256 - (sum(edid[:127]) % 256)) % 256

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/1080p.bin"
with open(out, "wb") as f:
    f.write(edid)
print(f"Wrote {len(edid)}-byte EDID to {out}  checksum=0x{edid[127]:02x}")
