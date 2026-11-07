#!/usr/bin/env python3
"""Host checks for link-map validation and the frontend's byte-addressed local export."""
import csv
import json
from pathlib import Path
import tempfile
import unittest

from export_retroachievements import export, record_address


class AchievementExportTest(unittest.TestCase):
    """Check real format and address-unit boundaries rather than trusting symbol names."""

    def test_map_validation(self):
        """Reject missing, duplicate, unaligned and truncated records."""
        valid = "    1000 0 10 achievement_ram\n"
        self.assertEqual(record_address(valid), 4096)
        for invalid in ("", valid * 2, "1001 0 10 achievement_ram\n", "1000 0 c achievement_ram\n"):
            with self.assertRaises(ValueError):
                record_address(invalid)

    def test_export(self):
        """Exercise two mask bytes and ensure guards point into native byte-addressed RAM."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            link_map = root / "game.map"
            link_map.write_text("1000 0 10 achievement_ram\n")
            manifest = root / "manifest.json"
            icon = root / "icon.png"
            icon.write_bytes(b"test badge")
            entries = [{"id": str(index), "title": 'A: "badge"', "description": "A description",
                        "points": 5, "icon": "icon.png"} for index in range(9)]
            manifest.write_text(json.dumps(entries))
            rom = root / "game.v32"
            rom.write_bytes(b"test ROM")
            output = root / "game-User.txt"
            notes = export(link_map, manifest, rom, root, output, 4000000)
            address = 4004096
            self.assertEqual(notes["retroachievements_byte_address"], address)
            self.assertEqual(notes["console_ram_word"], 1001024)
            lines = output.read_text().splitlines()
            self.assertEqual(lines[:2], ["1.0", "game"])
            records = list(csv.reader(lines[2:], delimiter=":", escapechar="\\"))
            self.assertTrue(all(len(record) == 14 for record in records))
            self.assertEqual(records[0][2], 'A: "badge"')
            self.assertIn(f"0xX{address + 12:06x}=1", records[0][1])
            self.assertIn(f"0xM{address + 8:06x}=1", records[0][1])
            self.assertIn(f"0xT{address + 8:06x}=1", records[7][1])
            self.assertIn(f"0xM{address + 9:06x}=1", records[8][1])
            self.assertEqual(output.read_bytes(), rom.with_suffix(".achievements.txt").read_bytes())
            self.assertEqual((root / "local/achievement-9.png").read_bytes(), icon.read_bytes())
            with self.assertRaises(ValueError):
                export(link_map, manifest, rom, root, output, 0x1000000)


if __name__ == "__main__":
    unittest.main()
