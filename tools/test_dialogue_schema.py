#!/usr/bin/env python3
"""Check validation of flag masks, entry actions and alternative NPC responses."""
import copy
import json
import tempfile
import unittest
from pathlib import Path

from content_compiler.compile_content import (
    ContentError, collect_dialogue, dialogue_choice_text, runtime_dialogues,
)


class DialogueSchemaTests(unittest.TestCase):
    """Exercise authoring errors before they can reach the ROM tables."""

    def setUp(self):
        """Create the smallest ordinary dialogue using the new primitives."""
        self.document = {
            "id": "test", "start": "greeting",
            "speakers": [{"id": "guide", "name": "Guide", "role": "npc"}],
            "nodes": [
                {"id": "greeting", "speaker": "guide", "text": "Hello",
                 "actions": [{"op": "set_bits", "flag": "witness", "value": 1}],
                 "responses": [{"node": "reply", "when": {"flag": "witness", "has_bits": 1}}]},
                {"id": "reply", "speaker": "guide", "text": "Goodbye", "end": True},
            ],
        }

    def validate(self, document):
        """Validate materialized JSONC through the ordinary content entry point."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "test.jsonc"
            path.write_text(json.dumps(document), encoding="utf-8")
            return collect_dialogue([path], {}, root)

    def test_valid_responses_and_actions(self):
        """Entry actions and conditional responses are legal source data."""
        self.assertEqual(self.validate(self.document), {"test"})

    def test_reject_invalid_response_and_action(self):
        """Reject broken references, ambiguous continuations and script operations."""
        changes = [
            lambda n: n["responses"][0].update(node="missing"),
            lambda n: n.update(next="reply"),
            lambda n: n.update(choices=[]),
            lambda n: n["actions"][0].update(op="show_dialogue"),
            lambda n: n["responses"][0].update(repeatable="true"),
        ]
        for change in changes:
            with self.subTest(change=change):
                document = copy.deepcopy(self.document)
                change(document["nodes"][0])
                with self.assertRaises(ContentError):
                    self.validate(document)

    def test_reject_empty_or_negative_mask(self):
        """A bit test must require at least one valid bit."""
        for comparison in ("has_bits", "lacks_bits"):
            for mask in (0, -1):
                with self.subTest(comparison=comparison, mask=mask):
                    document = copy.deepcopy(self.document)
                    document["nodes"][0]["responses"][0]["when"] = {"flag": "witness", comparison: mask}
                    with self.assertRaises(ContentError):
                        self.validate(document)

    def test_inferred_player_choice_and_shared_menu(self):
        """Infer wording and gettext fragments without changing target effects."""
        document = copy.deepcopy(self.document)
        document["speakers"].append({"id": "player", "name": "Banec", "role": "player"})
        document["start_actions"] = [{"op": "set", "flag": "asked", "value": 0}]
        document["nodes"][0].pop("responses")
        document["nodes"][0]["choices"] = [{"next": "ask", "when": {"flag": "asked", "equals": 0}}]
        document["nodes"].extend([
            {"id": "ask", "speaker": "player", "text": "Please help.",
             "translation_parts": ["Please ", "help."],
             "actions": [{"op": "set", "flag": "asked", "value": 1}], "next": "reply"},
            {"id": "again", "speaker": "guide", "text": "Anything else?", "choices": "greeting"},
        ])
        self.assertEqual(self.validate(document), {"test"})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "test.jsonc"
            path.write_text(json.dumps(document), encoding="utf-8")
            dialogues, _, flags = runtime_dialogues([path], [], {}, root, {}, {}, {})
        dialogue = dialogues[0]
        choice = dialogue["nodes"][0]["choices"][0]
        self.assertEqual(choice["text"], "Please help.")
        self.assertEqual(choice["translation_parts"], ["Please ", "help."])
        self.assertEqual(choice["next"], 2)
        self.assertEqual(dialogue["nodes"][2]["actions"][0]["value"], 1)
        self.assertIs(dialogue["nodes"][0]["choices"], dialogue["nodes"][3]["choices"])
        self.assertEqual(dialogue["start_actions"][0]["flag"], flags.index("asked"))

    def test_explicit_choice_label_keeps_its_own_translation(self):
        """An explicit override does not inherit the target's gettext fragments."""
        document = copy.deepcopy(self.document)
        document["nodes"][0].pop("responses")
        document["nodes"][0]["choices"] = [{"text": "Leave.", "next": "reply"}]
        document["nodes"][1]["translation_parts"] = ["Good", "bye"]
        self.assertEqual(self.validate(document), {"test"})
        text, parts = dialogue_choice_text(document["nodes"][0]["choices"][0],
            {n["id"]: n for n in document["nodes"]}, {"guide": "npc"}, "test")
        self.assertEqual(text, "Leave.")
        self.assertIsNone(parts)

    def test_reject_ambiguous_inference_and_invalid_start_actions(self):
        """Require a player target and reject unsupported conversation effects."""
        for choice in ({"next": "reply"}, {"next": "missing"}, {"text": None, "next": "reply"}):
            with self.subTest(choice=choice):
                document = copy.deepcopy(self.document)
                document["nodes"][0].pop("responses")
                document["nodes"][0]["choices"] = [choice]
                with self.assertRaises(ContentError):
                    self.validate(document)
        document = copy.deepcopy(self.document)
        document["start_actions"] = [{"op": "walk_actor", "entity": "guide"}]
        with self.assertRaises(ContentError):
            self.validate(document)


if __name__ == "__main__":
    unittest.main()
