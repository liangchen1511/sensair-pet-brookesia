#!/usr/bin/env python3
"""Extract files from ESP MMAP asset packs (new and legacy headers)."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


NEW_HEADER_SIZE = 32
LEGACY_HEADER_SIZE = 12
ENTRY_TAIL_SIZE = 12
FILE_MAGIC = b"\x5a\x5a"


def parse_header(data: bytes, legacy_name_length: int) -> tuple[int, int, int]:
    if data.startswith(b"MMAP"):
        _, _, name_length, file_count, _, payload_length = struct.unpack_from("<4s5I", data, 0)
        if NEW_HEADER_SIZE + payload_length > len(data):
            raise ValueError("MMAP payload exceeds input size")
        return NEW_HEADER_SIZE, name_length, file_count

    if len(data) < LEGACY_HEADER_SIZE:
        raise ValueError("Input is too small for an MMAP pack")
    file_count, _, payload_length = struct.unpack_from("<3I", data, 0)
    if LEGACY_HEADER_SIZE + payload_length > len(data):
        raise ValueError("Legacy MMAP payload exceeds input size")
    return LEGACY_HEADER_SIZE, legacy_name_length, file_count


def inspect_eaf(payload: bytes) -> dict[str, int | str] | None:
    if len(payload) < 32 or payload[0] != 0x89 or payload[1:4] not in (b"EAF", b"AAF"):
        return None

    frame_count = struct.unpack_from("<I", payload, 4)[0]
    if frame_count == 0:
        return {"format": payload[1:4].decode("ascii"), "frames": 0, "width": 0, "height": 0}

    frame_offset = struct.unpack_from("<I", payload, 20)[0]
    frame_start = 16 + frame_count * 8 + frame_offset
    if frame_start + 16 > len(payload) or payload[frame_start : frame_start + 2] != FILE_MAGIC:
        return {"format": payload[1:4].decode("ascii"), "frames": frame_count, "width": 0, "height": 0}

    frame_header = frame_start + 2
    width = struct.unpack_from("<H", payload, frame_header + 10)[0]
    height = struct.unpack_from("<H", payload, frame_header + 12)[0]
    return {
        "format": payload[1:4].decode("ascii"),
        "frames": frame_count,
        "width": width,
        "height": height,
    }


def extract(pack_path: Path, output_dir: Path, legacy_name_length: int) -> list[dict[str, object]]:
    data = pack_path.read_bytes()
    header_size, name_length, file_count = parse_header(data, legacy_name_length)
    entry_size = name_length + ENTRY_TAIL_SIZE
    data_start = header_size + file_count * entry_size
    manifest: list[dict[str, object]] = []

    output_dir.mkdir(parents=True, exist_ok=True)
    for index in range(file_count):
        entry_start = header_size + index * entry_size
        name_bytes = data[entry_start : entry_start + name_length]
        name = name_bytes.split(b"\0", 1)[0].decode("utf-8")
        size, offset, width, height = struct.unpack_from("<IIHH", data, entry_start + name_length)
        payload_start = data_start + offset
        payload_end = payload_start + 2 + size
        if payload_end > len(data) or data[payload_start : payload_start + 2] != FILE_MAGIC:
            raise ValueError(f"Invalid payload for asset {name!r}")

        payload = data[payload_start + 2 : payload_end]
        destination = output_dir / name
        destination.write_bytes(payload)
        item: dict[str, object] = {
            "name": name,
            "size": size,
            "table_width": width,
            "table_height": height,
        }
        eaf_info = inspect_eaf(payload)
        if eaf_info:
            item["eaf"] = eaf_info
        manifest.append(item)

    (output_dir / "extracted_manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("pack", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--legacy-name-length", type=int, default=32)
    args = parser.parse_args()

    manifest = extract(args.pack, args.output, args.legacy_name_length)
    print(f"Extracted {len(manifest)} assets to {args.output}")
    for item in manifest:
        eaf = item.get("eaf")
        if eaf:
            print(
                f"  {item['name']}: {eaf['width']}x{eaf['height']}, "
                f"{eaf['frames']} frames, {item['size']} bytes"
            )


if __name__ == "__main__":
    main()
