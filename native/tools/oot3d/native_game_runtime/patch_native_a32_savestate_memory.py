#!/usr/bin/env python3
"""Patch explicit guest-memory bytes in an OoT3D native A32 savestate.

Reads either container version; writes version 1 (uncompressed), which the
runtime still loads.
"""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

import msgpack

from compare_savestates import HEADER_SIZE, MAGIC, fnv1a64, load


CONTAINER_VERSION = 1


def parse_write(specification: str) -> tuple[int, bytes]:
    try:
        raw_address, raw_bytes = specification.split(":", 1)
        address = int(raw_address, 0)
        replacement = bytes.fromhex(raw_bytes)
    except (ValueError, TypeError) as exception:
        raise argparse.ArgumentTypeError(
            "write must use <address>:<even-length-hex-bytes>"
        ) from exception
    if address < 0 or address > 0xFFFFFFFF or not replacement:
        raise argparse.ArgumentTypeError("write address or payload is invalid")
    return address, replacement


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--write", required=True, action="append", type=parse_write,
        help="guest write as <address>:<hex-bytes>; may be repeated",
    )
    arguments = parser.parse_args()

    document = load(arguments.input, verify_checksum=True)
    if document.get("format") != "oot3d_native_a32_savestate_v1":
        raise ValueError("unsupported savestate document")
    memory = document["process"]["memory"]
    regions = memory["regions"]

    for address, replacement in arguments.write:
        owners = []
        for region in regions:
            base = int(region["base_address"])
            region_bytes = region["bytes"]
            if base <= address and address + len(replacement) <= base + len(region_bytes):
                owners.append((region, address - base))
        if len(owners) != 1:
            raise ValueError(
                f"write at 0x{address:08X} has {len(owners)} owning regions"
            )
        region, offset = owners[0]
        patched = bytearray(region["bytes"])
        patched[offset:offset + len(replacement)] = replacement
        region["bytes"] = bytes(patched)
        print(f"patched 0x{address:08X}..0x{address + len(replacement):08X}")

    memory["write_generation"] = int(memory.get("write_generation", 0)) + 1
    output_payload = msgpack.packb(document, use_bin_type=True)
    output_header = MAGIC + struct.pack(
        "<IIQQQ",
        CONTAINER_VERSION,
        HEADER_SIZE,
        len(output_payload),
        fnv1a64(output_payload),
        0,
    )
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_bytes(output_header + output_payload)
    print(f"wrote {arguments.output} ({len(output_header) + len(output_payload)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
