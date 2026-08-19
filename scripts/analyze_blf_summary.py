#!/usr/bin/env python3
"""Summarize the UDS diagnostic exchange in a Vector BLF file.

Prints one line per reassembled ISO-TP payload, truncated to the first
N bytes (default 12), so large TransferData blocks do not flood the output.
Usage: analyze_blf_summary.py <file.blf> [ids...] [--trunc N]
"""
import sys
import can

DEFAULT_IDS = {0x72C, 0x72D, 0x72E, 0x72F, 0x7DF, 0x701, 0x761, 0x772, 0x77A,
               0x773, 0x77B, 0x771, 0x779, 0x770, 0x778, 0x7E0, 0x7E8}


def main():
    args = sys.argv[1:]
    path = args[0]
    only_ids = None
    trunc = 12
    for arg in args[1:]:
        if arg == "--trunc":
            continue
        if arg.isdigit():
            trunc = int(arg)
            continue
        try:
            ids = {int(x, 16) for x in arg.split(",")}
            only_ids = ids if only_ids is None else only_ids | ids
        except ValueError:
            pass

    pending = {}
    rows = 0
    last_ts = None
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
            key = (msg.channel, aid, bool(getattr(msg, "is_tx", False)))
            pci = data[0] & 0xF0
            if pci == 0x00:
                if data[0] == 0x00 and len(data) >= 2:
                    length = data[1]
                    payload = data[2:2 + length]
                else:
                    length = data[0] & 0x0F
                    payload = data[1:1 + length]
                if not payload:
                    continue
                d = "Tx" if key[2] else "Rx"
                body = payload[:trunc].hex(" ")
                more = "+" if len(payload) > trunc else ""
                print(f"{msg.timestamp:012.6f} {aid:03X} {d} {body}{more}")
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
                        d = "Tx" if key[2] else "Rx"
                        body = payload[:trunc].hex(" ")
                        more = "+" if len(payload) > trunc else ""
                        print(f"{msg.timestamp:012.6f} {aid:03X} {d} {body}{more}  ({len(payload)}B)")
                        rows += 1
                        del pending[key]
    print(f"# diag payloads={rows}", file=sys.stderr)


if __name__ == "__main__":
    main()
