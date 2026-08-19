#!/usr/bin/env python3
"""Scan all BLF files in the ARC Logging dir and summarize each diagnostic
session: ECU response IDs, 0x520 frames, service sequence milestones, and
whether the flash reached reset (11 01) / restored (14 FF FF FF)."""
import glob
import os
import sys
import can
from collections import Counter

LOGGING = r"D:\project\楚能_D7_ARC3.31_30186_刷写规范_20260813\LP_ARC331\ARC\Logging"


def summarize(path):
    with can.BLFReader(path) as reader:
        n = 0
        resp_ids = Counter()
        w520 = 0
        first_diag = None
        last_diag = None
        milestones = []
        pending = {}
        seen = set()
        for msg in reader:
            if not hasattr(msg, "arbitration_id"):
                continue
            aid = msg.arbitration_id
            n += 1
            if aid == 0x520:
                w520 += 1
                continue
            if not (0x700 <= aid <= 0x7FF):
                continue
            data = bytes(msg.data)
            if not data:
                continue
            if first_diag is None:
                first_diag = msg.timestamp
            last_diag = msg.timestamp
            key = (msg.channel, aid, bool(getattr(msg, "is_tx", False)))
            pci = data[0] & 0xF0
            payload = None
            if pci == 0x00:
                if data[0] == 0x00 and len(data) >= 2:
                    length = data[1]
                    payload = data[2:2 + length]
                else:
                    length = data[0] & 0x0F
                    payload = data[1:1 + length]
            elif pci == 0x10:
                length = ((data[0] & 0x0F) << 8) | data[1]
                pending[key] = {"len": length, "buf": bytearray(data[2:])}
            elif pci == 0x20:
                p = pending.get(key)
                if p:
                    p["buf"].extend(data[1:])
                    if len(p["buf"]) >= p["len"]:
                        payload = bytes(p["buf"][:p["len"]])
                        del pending[key]
            if not payload:
                continue
            is_tx = key[2]
            if not is_tx:  # responses
                resp_ids[aid] += 1
            first = payload[0]
            if first in (0x10, 0x22, 0x27, 0x2E, 0x31, 0x34, 0x36, 0x37, 0x85, 0x28, 0x11, 0x14, 0x3E):
                if first == 0x31 and len(payload) >= 4:
                    tag = f"31 {payload[1]:02X} {payload[2]:02X} {payload[3]:02X}"
                elif first in (0x10, 0x27, 0x28, 0x85, 0x11, 0x14, 0x3E, 0x22, 0x2E, 0x34, 0x36, 0x37):
                    tag = f"{first:02X} {payload[1]:02X}" if len(payload) > 1 else f"{first:02X}"
                else:
                    tag = f"{first:02X}"
                if tag not in seen:
                    seen.add(tag)
                    milestones.append((msg.timestamp, aid, "Tx" if is_tx else "Rx", tag))
            if first == 0x7F and len(payload) >= 3:
                tag = f"NRC {payload[2]:02X} (req {payload[1]:02X})"
                if tag not in seen:
                    seen.add(tag)
                    milestones.append((msg.timestamp, aid, "Rx", tag))
    return {
        "msgs": n, "w520": w520,
        "resp_ids": dict(resp_ids),
        "dur": (last_diag - first_diag) if first_diag and last_diag else 0,
        "milestones": milestones,
    }


def main():
    files = sorted(glob.glob(os.path.join(LOGGING, "*.blf")))
    print(f"# {len(files)} BLF files\n")
    for path in files:
        name = os.path.basename(path)
        size = os.path.getsize(path)
        try:
            s = summarize(path)
        except Exception as exc:
            print(f"== {name} ({size}B) ERROR: {exc}")
            continue
        keys = ",".join(f"{k:03X}" for k in sorted(s["resp_ids"].keys()))
        print(f"== {name} ({size}B) msgs={s['msgs']} w520={s['w520']} dur={s['dur']:.1f}s resp={keys}")
        for ts, aid, d, tag in s["milestones"]:
            rel = ts - (s["milestones"][0][0] if s["milestones"] else 0)
            print(f"   {rel:8.2f} {aid:03X} {d} {tag}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
