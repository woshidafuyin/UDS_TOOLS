#!/usr/bin/env python3
"""Compare APP s19 (CHF0330N) window data with CBF APP main data."""
import hashlib
import zlib

def parse_s19_window(path, start, length):
    image = bytearray([0xFF]) * length
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line[0] != "S":
                continue
            t = line[1]
            if t not in "123":
                continue
            addr_bytes = {"1": 2, "2": 3, "3": 4}[t]
            b = bytes.fromhex(line[2:])
            if b[0] != len(b) - 1:
                raise ValueError("count mismatch")
            if sum(b) & 0xFF != 0xFF:
                raise ValueError("checksum mismatch")
            addr = int.from_bytes(b[1:1 + addr_bytes], "big")
            data = b[1 + addr_bytes:-1]
            if addr < start or addr + len(data) > start + length:
                continue
            off = addr - start
            image[off:off + len(data)] = data
    return bytes(image)

def cbf_main(path):
    data = open(path, "rb").read()
    text = data.decode("latin1")
    hdr_end = text.find("\n}")
    off = hdr_end + 2
    while off < len(data) and data[off] in (10, 13):
        off += 1
    ln = int.from_bytes(data[off + 4:off + 8], "big")
    return data[off + 8:off + 8 + ln]

app_s19 = r"D:\project\UDS_tools\UDS_tools\resources\chuneng_d7_arc331_zip\APP\ARC2.36BC3_LEE35A_B1.01.00_APP_V3.01.00_CHF0330N_without_boot.s19"
cbf_app = r"D:\project\UDS_tools\UDS_tools\resources\chuneng_d7_arc331_zip\CBF\APP\7052A5023002AB.cbf"

s19 = parse_s19_window(app_s19, 0x000C0000, 0x180000)
main = cbf_main(cbf_app)
print("S19  APP window 0xC0000/0x180000 sha256:", hashlib.sha256(s19).hexdigest())
print("S19  APP window crc32:", hex(zlib.crc32(s19) & 0xFFFFFFFF))
print("CBF  APP main         sha256:", hashlib.sha256(main).hexdigest())
print("CBF  APP main         crc32:", hex(zlib.crc32(main) & 0xFFFFFFFF))
print("SAME APP DATA:", s19 == main)
