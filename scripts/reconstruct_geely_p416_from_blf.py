#!/usr/bin/env python3
"""Reconstruct the exact P416 VBF payloads observed in successful BLFs.

The resulting files are reproducible evidence artifacts, not supplier-original
VBF releases.  Every APP and PLS cycle must agree before output is written.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path

import can


@dataclass(eq=True)
class Block:
    dfi: int
    address: int
    length: int
    data: bytes = b""


@dataclass(eq=True)
class Cycle:
    blocks: list[Block] = field(default_factory=list)
    signatures: list[bytes] = field(default_factory=list)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def isotp_payloads(path: Path):
    state: dict[int, tuple[int, bytearray]] = {}
    for message in can.BLFReader(path):
        arbitration_id = int(message.arbitration_id)
        if arbitration_id not in {0x716, 0x701, 0x7FF, 0x7DF}:
            continue
        data = bytes(message.data)
        if not data:
            continue
        frame_type = data[0] >> 4
        if frame_type == 0:
            length = data[0] & 0x0F
            if length == 0 and len(data) > 1:
                length = data[1]
                yield arbitration_id, data[2 : 2 + length]
            else:
                yield arbitration_id, data[1 : 1 + length]
        elif frame_type == 1:
            length = ((data[0] & 0x0F) << 8) | data[1]
            state[arbitration_id] = (length, bytearray(data[2:]))
        elif frame_type == 2 and arbitration_id in state:
            length, payload = state[arbitration_id]
            payload.extend(data[1:])
            if len(payload) >= length:
                yield arbitration_id, bytes(payload[:length])
                del state[arbitration_id]


def extract_cycles(path: Path) -> list[Cycle]:
    cycles: list[Cycle] = []
    current: Cycle | None = None
    active_block: Block | None = None
    active_data = bytearray()

    for arbitration_id, payload in isotp_payloads(path):
        if arbitration_id != 0x716 or not payload:
            continue
        if payload == b"\x27\x01":
            current = Cycle()
            active_block = None
            active_data.clear()
            continue
        if current is None:
            continue
        if payload[0] == 0x34:
            if len(payload) != 11 or payload[2] != 0x44:
                raise RuntimeError(f"unexpected RequestDownload in {path}: {payload.hex()}")
            active_block = Block(
                dfi=payload[1],
                address=int.from_bytes(payload[3:7], "big"),
                length=int.from_bytes(payload[7:11], "big"),
            )
            active_data.clear()
        elif payload[0] == 0x36 and active_block is not None:
            active_data.extend(payload[2:])
        elif payload == b"\x37" and active_block is not None:
            if len(active_data) != active_block.length:
                raise RuntimeError(
                    f"block {active_block.address:08X} length mismatch in {path}"
                )
            active_block.data = bytes(active_data)
            current.blocks.append(active_block)
            active_block = None
            active_data.clear()
        elif payload.startswith(b"\x31\x01\x02\x12"):
            signature = payload[4:]
            if len(signature) != 256:
                raise RuntimeError(f"signature length mismatch in {path}")
            current.signatures.append(signature)
        elif payload == b"\x11\x01":
            if len(current.blocks) == 10 and len(current.signatures) == 3:
                cycles.append(current)
            else:
                raise RuntimeError(
                    f"incomplete cycle in {path}: {len(current.blocks)} blocks, "
                    f"{len(current.signatures)} signatures"
                )
            current = None
    if not cycles:
        raise RuntimeError(f"no complete P416 flashing cycle found in {path}")
    return cycles


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def build_vbf(
    path: Path,
    part_number: str,
    part_type: str,
    dfi: int,
    blocks: list[Block],
    signature: bytes,
    erase: list[tuple[int, int]] | None = None,
    call: int | None = None,
) -> None:
    binary = bytearray()
    for block in blocks:
        if block.dfi != dfi:
            raise RuntimeError(f"mixed DFI in {part_type} block set")
        binary.extend(struct.pack(">II", block.address, len(block.data)))
        binary.extend(block.data)
        binary.extend(struct.pack(">H", crc16_ccitt_false(block.data)))
    checksum = zlib.crc32(binary) & 0xFFFFFFFF
    lines = [
        "vbf_version = 2.6;",
        "header {",
        "\t// Reconstructed byte-for-byte from successful 2026-07-31 P416 BLF traces",
        f'\tsw_part_number = "{part_number}";',
        '\tsw_version = "BLF-RECON-1";',
        f"\tsw_part_type = {part_type};",
        f"\tdata_format_identifier = 0x{dfi:02X};",
        "\tecu_address = 0x1316;",
        f"\tfile_checksum = 0x{checksum:08X};",
    ]
    if call is not None:
        lines.append(f"\tcall = 0x{call:08X};")
    if erase:
        values = ",".join(f"{{0x{address:08X},0x{length:08X}}}" for address, length in erase)
        lines.append(f"\terase = {{{values}}};")
    lines.append(f"\tsw_signature_dev = 0x{signature.hex().upper()};")
    lines.append("}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes("\r\n".join(lines).encode("ascii") + binary)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app-blf", type=Path, required=True)
    parser.add_argument("--pls-blf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    app_cycles = extract_cycles(args.app_blf)
    pls_cycles = extract_cycles(args.pls_blf)
    baseline = app_cycles[0]
    for label, cycles in (("APP", app_cycles), ("PLS", pls_cycles)):
        for index, cycle in enumerate(cycles, 1):
            if cycle != baseline:
                raise RuntimeError(f"{label} cycle {index} differs from APP baseline")

    expected = [
        (0x10, 0x00430000, 0x48),
        (0x10, 0x00430200, 0x11DE3),
        (0x10, 0x00453828, 0x3630),
        (0x10, 0x0045A5C8, 0x364),
        (0x10, 0x0045BF80, 0x6),
        (0x10, 0x0045C000, 0xE0),
        (0x00, 0x0013C000, 0x2C),
        (0x00, 0x0013C100, 0x40),
        (0x10, 0x000C0000, 0x33),
        (0x10, 0x000C1000, 0x34DEF),
    ]
    actual = [(block.dfi, block.address, block.length) for block in baseline.blocks]
    if actual != expected:
        raise RuntimeError(f"P416 block layout differs from captured contract: {actual}")

    outputs = {
        "SBL/P416_SBL_reconstructed.vbf": (baseline.blocks[:6], baseline.signatures[0]),
        "ESS/P416_ESS_reconstructed.vbf": (baseline.blocks[6:8], baseline.signatures[1]),
        "APP/P416_APP_reconstructed.vbf": (baseline.blocks[8:], baseline.signatures[2]),
    }
    build_vbf(
        args.output / "SBL/P416_SBL_reconstructed.vbf",
        "P416_RECON_SBL", "SBL", 0x10,
        baseline.blocks[:6], baseline.signatures[0], call=0x00430000,
    )
    build_vbf(
        args.output / "ESS/P416_ESS_reconstructed.vbf",
        "P416_RECON_ESS", "EXE", 0x00,
        baseline.blocks[6:8], baseline.signatures[1],
        erase=[(0x0013C000, 0x2C), (0x0013C100, 0x40)],
    )
    build_vbf(
        args.output / "APP/P416_APP_reconstructed.vbf",
        "P416_RECON_APP", "EXE", 0x10,
        baseline.blocks[8:], baseline.signatures[2],
        erase=[(0x000C0000, 0x2C), (0x000C1000, 0x7B000)],
    )

    manifest = {
        "status": "reconstructed_from_successful_blf_not_supplier_original",
        "app_blf": {"name": args.app_blf.name, "sha256": sha256(args.app_blf), "cycles": len(app_cycles)},
        "pls_blf": {"name": args.pls_blf.name, "sha256": sha256(args.pls_blf), "cycles": len(pls_cycles)},
        "outputs": {},
    }
    for relative in outputs:
        output = args.output / relative
        manifest["outputs"][relative] = {
            "bytes": output.stat().st_size,
            "sha256": sha256(output),
        }
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
