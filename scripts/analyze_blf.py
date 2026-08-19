#!/usr/bin/env python3
"""Reconstruct ISO-TP / UDS diagnostic stream from a Vector BLF logging file.

Reassembles single frames and FF/CF sequences per (channel, arbitration id)
and prints the UDS payload stream: <timestamp> <ID> <DIR> <payload-hex>.
Usage: analyze_blf.py <file.blf> [id1,id2,...]   (ids in hex, optional filter)
"""
import sys
import can

DEFAULT_IDS = {0x72C, 0x72D, 0x72E, 0x72F, 0x7DF, 0x701, 0x761, 0x772, 0x77A,
               0x773, 0x77B, 0x771, 0x779, 0x770, 0x778, 0x7E0, 0x7E8}


def main():
    path = sys.argv[1]
    only_ids = None
    if len(sys.argv) > 2:
        only_ids = {int(x, 16) for x in sys.argv[2].split(",")}

    pending = {}  # (channel, aid, is_tx) -> {len, buf}
    rows = 0
    try:
        with can.BLFReader(path) as reader:
            for msg in reader:
                if not hasattr(msg, "arbitration_id"):
                    continue
                aid = msg.arbitration_id
                if only_ids is not None:
                    if aid not in only_ids:
                        continue
                elif aid not in DEFAULT_IDS and not (0x700 <= aid <= 0x7FF):
                    continue
                data = bytes(msg.data)
                if not data:
                    continue
                is_tx = getattr(msg, "is_tx", False)
                key = (msg.channel, aid, is_tx)
                pci = data[0] & 0xF0
                if pci == 0x00:
                    if data[0] == 0x00 and len(data) >= 2:
                        length = data[1]
                        payload = data[2:2 + length]
                    else:
                        length = data[0] & 0x0F
                        payload = data[1:1 + length]
                    if payload:
                        d = "Tx" if is_tx else "Rx"
                        print(f"{msg.timestamp:012.6f} {aid:03X} {d} {payload.hex(' ')}")
                        rows += 1
                elif pci == 0x10:
                    length = ((data[0] & 0x0F) << 8) | data[1]
                    pending[key] = {"len": length, "buf": bytearray(data[2:])}
                elif pci == 0x20:
                    p = pending.get(key)
                    if p:
                        p["buf"].extend(data[1:])
                        if len(p["buf"]) >= p["len"]:
                            payload = bytes(p["buf"][:p["len"]])
                            d = "Tx" if is_tx else "Rx"
                            print(f"{msg.timestamp:012.6f} {aid:03X} {d} {payload.hex(' ')}")
                            rows += 1
                            del pending[key]
                # 0x30 flow control ignored
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    print(f"# diag payloads={rows}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
