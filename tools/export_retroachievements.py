#!/usr/bin/env python3
"""Locate the ordinary achievement C array in wasm-ld's map and emit local RA assets.

Addresses describe this particular ROM. Nothing reserves console RAM or exports a
Wasm address global; the game's regular data allocation owns the record.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

MAGIC = 0x57454143
VERSION = 1
WORD_BYTES = 4
RAM_BYTES = 4194304 * WORD_BYTES


def record_address(link_map: str) -> int:
    """Require exactly one aligned 16-byte achievement_ram symbol in the link map."""
    matches = re.findall(
        r"^\s*([0-9a-fA-F]+)\s+[0-9a-fA-F]+\s+([0-9a-fA-F]+)\s+achievement_ram\s*$",
        link_map, re.MULTILINE)
    if len(matches) != 1:
        raise ValueError("link map must contain exactly one achievement_ram symbol")
    address, size = (int(value, 16) for value in matches[0])
    if size != 16 or address % WORD_BYTES:
        raise ValueError("achievement_ram must be an aligned four-word C array")
    return address


def quoted(value: str) -> str:
    """Quote one RAIntegration colon-delimited field, including embedded escapes."""
    return json.dumps(value, ensure_ascii=False)


def copy_badge(icon: str | dict, game_root: Path, target: Path) -> None:
    """Preserve a standalone PNG, or extract a badge's authored atlas rectangle."""
    if isinstance(icon, str):
        shutil.copyfile(game_root / icon, target)
        return
    from PIL import Image
    with Image.open(game_root / icon["source"]) as source:
        if "rect" in icon:
            x, y, width, height = icon["rect"]
            source = source.crop((x, y, x + width, y + height))
        source.save(target)


def export(link_map: Path, manifest: Path, rom: Path, game_root: Path,
           output: Path, ram_byte_base: int) -> dict:
    """Write a build-specific -User.txt, a frontend sidecar, badge PNGs and address notes."""
    offset = record_address(link_map.read_text())
    address = ram_byte_base + offset
    if ram_byte_base < 0 or ram_byte_base % WORD_BYTES or address + 16 > RAM_BYTES:
        raise ValueError("achievement record falls outside the core's system RAM")
    entries = json.loads(manifest.read_text())
    if len(entries) > 32:
        raise ValueError("achievement bitmask supports at most 32 entries")
    lines = ["1.0", rom.stem]
    guards = f"0xX{address:06x}={MAGIC}_0xX{address + 4:06x}={VERSION}_0xX{address + 12:06x}=1"
    badges = output.parent / "local"
    badges.mkdir(parents=True, exist_ok=True)
    for index, entry in enumerate(entries):
        # M..T are RA's byte-local bits 0..7, independent of the CPU's word addresses.
        bit = "MNOPQRST"[index % 8]
        condition = guards + f"_0x{bit}{address + 8 + index // 8:06x}=1"
        badge = f"achievement-{index + 1}.png"
        copy_badge(entry["icon"], game_root, badges / badge)
        lines.append(":".join([
            "0", quoted(condition), quoted(entry["title"]), quoted(entry["description"]),
            "", "", "", quoted("Waste's Edge port"), str(entry["points"]), "", "", "", "",
            quoted("local/" + badge),
        ]))
    text = "\n".join(lines) + "\n"
    output.write_text(text)
    rom.with_suffix(".achievements.txt").write_text(text)
    notes = {
        "format": "achievement-ram-v1", "rom_sha256": hashlib.sha256(rom.read_bytes()).hexdigest(),
        "symbol": "achievement_ram", "wasm_byte_offset": offset,
        "linear_memory_ram_byte_base": ram_byte_base, "console_ram_word": address // WORD_BYTES,
        "retroachievements_byte_address": address, "size_bytes": 16,
        "fields": {"magic": address, "version": address + 4, "unlocked": address + 8, "active": address + 12},
        "bits": [{"bit": index, "id": entry["id"], "title": entry["title"]}
                 for index, entry in enumerate(entries)],
    }
    output.with_suffix(".ram.json").write_text(json.dumps(notes, indent=2) + "\n")
    return notes


def main() -> None:
    """Expose the current backend's linear RAM base as an explicit build setting."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--gamedata", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ram-byte-base", type=lambda value: int(value, 0), required=True)
    args = parser.parse_args()
    notes = export(args.map, args.manifest, args.rom, args.gamedata, args.output, args.ram_byte_base)
    print(f"Achievement record: Wasm +0x{notes['wasm_byte_offset']:x}, "
          f"console RAM word {notes['console_ram_word']}, RA byte 0x{notes['retroachievements_byte_address']:x}")


if __name__ == "__main__":
    main()
