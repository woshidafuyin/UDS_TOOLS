#!/usr/bin/env python3
"""Parse S19 and compare with CBF main data (Driver window)."""
import hashlib
import re
import sys

def parse_s19(path, start, length):
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
            count = b[0]
            if count != len(b) - 1:
                raise ValueError("count mismatch")
            checksum = sum(b) & 0xFF
            if checksum != 0xFF:
                raise ValueError("checksum mismatch")
            addr = int.from_bytes(b[1:1 + addr_bytes], "big")
            data = b[1 + addr_bytes:-1]
            if addr < start or addr + len(data) > start + length:
                continue
            off = addr - start
            image[off:off + len(data)] = data
    return bytes(image)

driver_srec = r"D:\project\UDS_tools\UDS_tools\resources\chuneng_d7_arc331_zip\Driver\FlashDriver.srec"
cbf_driver = r"D:\project\UDS_tools\UDS_tools\resources\chuneng_d7_arc331_zip\CBF\Driver\driver_712345678AB.cbf"

# CBF main (0x10280000 window) and the S19 driver window at 0x00000000
s19_data = parse_s19(driver_srec, 0x00000000, 0x4000)
print("FlashDriver.srec window 0x0/0x4000 sha256:", hashlib.sha256(s19_data).hexdigest())
print("FlashDriver.srec window crc32(ieee):", hex(__import__("zlib").crc32(s19_data) & 0xFFFFFFFF))

data = open(cbf_driver, "rb").read()
text = data.decode("latin1")
hdr_end = text.find("\n}")
off = hdr_end + 2
while off < len(data) and data[off] in (10, 13):
    off += 1
addr = int.from_bytes(data[off:off + 4], "big")
ln = int.from_bytes(data[off + 4:off + 8], "big")
main = data[off + 8:off + 8 + ln]
print("CBF Driver main addr=0x%08X len=0x%X sha256:" % (addr, ln), hashlib.sha256(main).hexdigest())
print("CBF Driver main crc32(ieee):", hex(__import__("zlib").crc32(main) & 0xFFFFFFFF))
print("SAME DATA:", s19_data == main)
