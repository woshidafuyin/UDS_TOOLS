#!/usr/bin/env python3
"""Compare Chuneng Driver/APP CBF headers and main-data hashes."""
import hashlib
import json
import os
import re

def parse_cbf(path):
    data = open(path, "rb").read()
    text = data.decode("latin1")
    hdr_end = text.find("\n}")
    if hdr_end < 0:
        return {"error": "no header end"}
    hdr = text[:hdr_end + 2]

    def get(field):
        m = re.search(r"(?:^|\n)\s*" + field + r"\s*=\s*([^;]+)\s*;", hdr)
        return m.group(1).strip().strip('"') if m else None

    off = hdr_end + 2
    while off < len(data) and data[off] in (10, 13):
        off += 1
    addr = int.from_bytes(data[off:off + 4], "big")
    ln = int.from_bytes(data[off + 4:off + 8], "big")
    main = data[off + 8:off + 8 + ln]
    sig = get("dev_signature") or ""
    if sig.startswith("0x") or sig.startswith("0X"):
        sig_bytes = bytes.fromhex(sig[2:])
    else:
        sig_bytes = bytes.fromhex(sig)
    return {
        "sw_id": get("sw_id"),
        "sw_ver": get("sw_version"),
        "sw_type": get("sw_type"),
        "ecu_address": get("ecu_address"),
        "main_addr": hex(addr),
        "main_len": ln,
        "main_sha256": hashlib.sha256(main).hexdigest(),
        "sig_len": len(sig_bytes),
        "sig_sha256": hashlib.sha256(sig_bytes).hexdigest(),
    }

paths = [
    r"D:\project\UDS_tools\UDS_tools\resources\chuneng_d7_arc331_zip\CBF\Driver\driver_712345678AB.cbf",
    r"D:\project\楚能_D7_ARC3.31_30186_刷写规范_20260813\1\ARC3.31BC3_CND7AP_B1.00.00_APP_V1.00.00_CHF0389N(2026.08.05)\712345678AA.cbf",
    r"D:\project\UDS_tools\UDS_tools\resources\chuneng_d7_arc331_zip\CBF\APP\7052A5023002AB.cbf",
]
for p in paths:
    print("====", os.path.basename(p))
    print(json.dumps(parse_cbf(p), indent=1, ensure_ascii=False))
