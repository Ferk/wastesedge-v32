#!/usr/bin/env python3
"""Validate editable JSONC/Tiled inputs and emit deterministic C metadata."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path


GENERATED_RESOURCE_PREFIX = "@generated/"
BACKGROUND_CHUNK_TILES_X = 8
BACKGROUND_CHUNK_TILES_Y = 6
DIALOGUE_TEXT_FONT_SIZE = 15
DIALOGUE_TEXT_LINE_HEIGHT = 19
DIALOGUE_ATLAS_WIDTH = 512
DIALOGUE_ATLAS_HEIGHT = 512
CHARACTER_SHEET_WIDTHS = (256, 512, 1024)


class ContentError(Exception):
    pass


def strip_jsonc(source: str) -> str:
    """Remove line/block comments without touching comment-like string content."""
    output: list[str] = []
    index = 0
    in_string = False
    escaped = False
    while index < len(source):
        char = source[index]
        next_char = source[index + 1] if index + 1 < len(source) else ""
        if in_string:
            output.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            index += 1
        elif char == '"':
            output.append(char)
            in_string = True
            index += 1
        elif char == "/" and next_char == "/":
            index += 2
            while index < len(source) and source[index] not in "\r\n":
                index += 1
        elif char == "/" and next_char == "*":
            index += 2
            closed = False
            while index < len(source):
                if source[index] == "*" and index + 1 < len(source) and source[index + 1] == "/":
                    index += 2
                    closed = True
                    break
                if source[index] in "\r\n":
                    output.append(source[index])
                index += 1
            if not closed:
                raise ContentError("unterminated block comment")
        else:
            output.append(char)
            index += 1
    return "".join(output)


def load_jsonc(path: Path) -> object:
    try:
        return json.loads(strip_jsonc(path.read_text(encoding="utf-8")))
    except OSError as error:
        raise ContentError(f"{path}: cannot read file: {error}") from error
    except json.JSONDecodeError as error:
        raise ContentError(f"{path}:{error.lineno}:{error.colno}: {error.msg}") from error


def expect(value: object, expected: type, description: str) -> object:
    if not isinstance(value, expected):
        raise ContentError(f"{description} must be a {expected.__name__}")
    return value


def identifier(value: object, description: str) -> str:
    value = expect(value, str, description)
    if not re.fullmatch(r"[a-z][a-z0-9_]*", value):
        raise ContentError(f"{description} must be lower-case snake case, got {value!r}")
    return value


def relative_input(root: Path, raw_path: object, category: str) -> Path:
    relative = expect(raw_path, str, f"manifest {category} entry")
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise ContentError(f"manifest {category} entry leaves gamedata: {relative!r}") from error
    if not candidate.is_file():
        raise ContentError(f"manifest {category} input is missing: {relative}")
    return candidate


def properties(raw_properties: object, description: str) -> dict[str, object]:
    if raw_properties is None:
        return {}
    raw_properties = expect(raw_properties, list, f"{description}.properties")
    result: dict[str, object] = {}
    for raw_property in raw_properties:
        raw_property = expect(raw_property, dict, f"{description}.properties entry")
        name = expect(raw_property.get("name"), str, f"{description}.properties name")
        if name in result:
            raise ContentError(f"{description} has duplicate property {name!r}")
        result[name] = raw_property.get("value")
    return result


def collect_entities(paths: list[Path]) -> dict[str, dict[str, object]]:
    result: dict[str, dict[str, object]] = {}
    for path in paths:
        document = expect(load_jsonc(path), dict, str(path))
        entries = expect(document.get("entities"), list, f"{path}.entities")
        for entry in entries:
            entry = expect(entry, dict, f"{path}.entities entry")
            entry_id = identifier(entry.get("id"), f"{path} entity id")
            if entry_id in result:
                raise ContentError(f"duplicate entity id: {entry_id}")
            kind = identifier(entry.get("kind"), f"{path} entity {entry_id} kind")
            if kind not in {"actor", "object"}:
                raise ContentError(f"{path} entity {entry_id} has unsupported kind {kind!r}")
            player_controlled = entry.get("player_controlled", False)
            if not isinstance(player_controlled, bool):
                raise ContentError(f"{path} entity {entry_id} player_controlled must be a bool")
            blocks_movement = entry.get("blocks_movement", entry.get("kind") == "actor")
            if not isinstance(blocks_movement, bool):
                raise ContentError(f"{path} entity {entry_id} blocks_movement must be a bool")
            raw_states = expect(entry.get("states", ["default"]), list,
                                f"{path} entity {entry_id} states")
            states: list[str] = []
            for raw_state in raw_states:
                state = identifier(raw_state, f"{path} entity {entry_id} state")
                if state in states:
                    raise ContentError(f"{path} entity {entry_id} duplicates state {state!r}")
                states.append(state)
            if not states:
                raise ContentError(f"{path} entity {entry_id} must define at least one state")
            result[entry_id] = {
                "kind": kind,
                "player_controlled": player_controlled,
                "blocks_movement": blocks_movement,
                "states": states,
            }
    if sum(1 for entry in result.values() if entry["player_controlled"]) > 1:
        raise ContentError("only one entity definition may set player_controlled")
    return result


def collect_schedules(paths: list[Path], entities: dict[str, dict[str, object]]) -> dict[str, dict[str, object]]:
    """Validate reusable NPC routes before scene IDs are resolved."""
    result: dict[str, dict[str, object]] = {}
    facing_names = {"north", "south", "west", "east"}
    for path in paths:
        document = expect(load_jsonc(path), dict, str(path))
        for raw_schedule in expect(document.get("schedules"), list, f"{path}.schedules"):
            schedule = expect(raw_schedule, dict, f"{path}.schedules entry")
            schedule_id = identifier(schedule.get("id"), f"{path} schedule id")
            if schedule_id in result:
                raise ContentError(f"duplicate schedule id: {schedule_id}")
            entity_id = identifier(schedule.get("entity"), f"{path} schedule {schedule_id} entity")
            if entity_id not in entities or entities[entity_id]["kind"] != "actor":
                raise ContentError(f"{path}: schedule {schedule_id!r} needs a known actor entity")
            loop = schedule.get("loop", True)
            if not isinstance(loop, bool):
                raise ContentError(f"{path}: schedule {schedule_id!r} loop must be a bool")
            speech = None
            raw_speech = schedule.get("speech")
            if raw_speech is not None:
                raw_speech = expect(raw_speech, dict, f"{path} schedule {schedule_id} speech")
                speech_lines: list[str] = []
                for line_number, raw_line in enumerate(expect(
                        raw_speech.get("lines"), list,
                        f"{path} schedule {schedule_id} speech lines")):
                    line = expect(raw_line, str,
                                  f"{path} schedule {schedule_id} speech line {line_number}")
                    if not line:
                        raise ContentError(
                            f"{path}: schedule {schedule_id!r} has an empty speech line")
                    speech_lines.append(line)
                if not speech_lines:
                    raise ContentError(
                        f"{path}: schedule {schedule_id!r} speech needs at least one line")
                speech_delay_min = integer(raw_speech.get("delay_min_frames"),
                                           f"{path} schedule {schedule_id} speech minimum delay")
                speech_delay_max = integer(raw_speech.get("delay_max_frames"),
                                           f"{path} schedule {schedule_id} speech maximum delay")
                speech_duration = integer(raw_speech.get("duration_frames", 180),
                                          f"{path} schedule {schedule_id} speech duration")
                if (speech_delay_min < 1 or speech_delay_max < speech_delay_min or
                        speech_duration < 1):
                    raise ContentError(
                        f"{path}: schedule {schedule_id!r} has invalid speech timing")
                speech = {"lines": speech_lines, "delay_min": speech_delay_min,
                          "delay_max": speech_delay_max, "duration": speech_duration}
            steps: list[dict[str, object]] = []
            step_labels: set[str] = set()
            for index, raw_step in enumerate(expect(schedule.get("steps"), list,
                                                     f"{path} schedule {schedule_id} steps")):
                step = expect(raw_step, dict, f"{path} schedule {schedule_id} step {index}")
                operation = identifier(step.get("op"), f"{path} schedule {schedule_id} step {index} op")
                if operation not in {"move", "place", "wait", "speak",
                                     "set_object_state", "toggle_object_state",
                                     "jump", "random_jump"}:
                    raise ContentError(f"{path}: unsupported schedule operation {operation!r}")
                delay_min = integer(step.get("delay_min_frames", step.get("delay_frames", 0)),
                                    f"{path} schedule {schedule_id} step {index} minimum delay")
                delay_max = integer(step.get("delay_max_frames", delay_min),
                                    f"{path} schedule {schedule_id} step {index} maximum delay")
                if delay_min < 0 or delay_max < delay_min:
                    raise ContentError(
                        f"{path}: schedule {schedule_id!r} step {index} has an invalid delay range")
                compiled_step: dict[str, object] = {
                    "op": operation, "delay_min": delay_min, "delay_max": delay_max,
                }
                if "label" in step:
                    label = identifier(step.get("label"),
                                       f"{path} schedule {schedule_id} step {index} label")
                    if label in step_labels:
                        raise ContentError(
                            f"{path}: schedule {schedule_id!r} duplicates label {label!r}")
                    step_labels.add(label)
                    compiled_step["label"] = label
                if operation in {"move", "place"}:
                    scene = identifier(step.get("scene"),
                                       f"{path} schedule {schedule_id} step {index} scene")
                    x = integer(step.get("x"), f"{path} schedule {schedule_id} step {index} x")
                    y = integer(step.get("y"), f"{path} schedule {schedule_id} step {index} y")
                    facing = identifier(step.get("facing"),
                                        f"{path} schedule {schedule_id} step {index} facing")
                    if facing not in facing_names:
                        raise ContentError(f"{path}: unsupported schedule facing {facing!r}")
                    compiled_step.update({"scene": scene, "x": x, "y": y,
                                          "facing": facing})
                elif operation == "speak":
                    text = expect(step.get("text"), str,
                                  f"{path} schedule {schedule_id} step {index} text")
                    if not text:
                        raise ContentError(
                            f"{path}: schedule {schedule_id!r} speak step {index} needs text")
                    duration = integer(step.get("duration_frames", 180),
                                       f"{path} schedule {schedule_id} step {index} duration")
                    if duration < 1:
                        raise ContentError(
                            f"{path}: schedule {schedule_id!r} speak step {index} "
                            "needs a positive duration")
                    compiled_step.update({"text": text, "duration": duration})
                elif operation in {"set_object_state", "toggle_object_state"}:
                    compiled_step["scene"] = identifier(
                        step.get("scene"), f"{path} schedule {schedule_id} step {index} scene")
                    compiled_step["object"] = identifier(
                        step.get("object"), f"{path} schedule {schedule_id} step {index} object")
                    if operation == "set_object_state":
                        compiled_step["state"] = identifier(
                            step.get("state"), f"{path} schedule {schedule_id} step {index} state")
                elif operation == "jump":
                    compiled_step["target"] = identifier(
                        step.get("target"), f"{path} schedule {schedule_id} step {index} target")
                elif operation == "random_jump":
                    targets: list[str] = []
                    for target_number, raw_target in enumerate(expect(
                            step.get("targets"), list,
                            f"{path} schedule {schedule_id} step {index} targets")):
                        target = identifier(
                            raw_target,
                            f"{path} schedule {schedule_id} step {index} target {target_number}")
                        if target in targets:
                            raise ContentError(
                                f"{path}: schedule {schedule_id!r} random jump step {index} "
                                f"duplicates target {target!r}")
                        targets.append(target)
                    if not targets:
                        raise ContentError(
                            f"{path}: schedule {schedule_id!r} random jump step {index} "
                            "needs at least one target")
                    compiled_step["targets"] = targets
                elif delay_min == 0:
                    raise ContentError(
                        f"{path}: schedule {schedule_id!r} wait step {index} needs a positive delay")
                steps.append(compiled_step)
            if not steps:
                raise ContentError(f"{path}: schedule {schedule_id!r} must contain at least one step")
            for index, step in enumerate(steps):
                targets = ([step["target"]] if step["op"] == "jump" else
                           step.get("targets", []))
                for target in targets:
                    if target not in step_labels:
                        raise ContentError(
                            f"{path}: schedule {schedule_id!r} step {index} refers to "
                            f"unknown label {target!r}")
            next_schedule = schedule.get("next")
            if next_schedule is not None:
                next_schedule = identifier(next_schedule, f"{path} schedule {schedule_id} next")
            result[schedule_id] = {"id": schedule_id, "entity": entity_id,
                                   "loop": loop, "next": next_schedule,
                                   "steps": steps, "speech": speech}
    for schedule_id, schedule in result.items():
        next_schedule = schedule["next"]
        if next_schedule is None:
            continue
        if next_schedule not in result:
            raise ContentError(
                f"schedule {schedule_id!r} refers to unknown next schedule {next_schedule!r}")
        if result[str(next_schedule)]["entity"] != schedule["entity"]:
            raise ContentError(
                f"schedule {schedule_id!r} cannot chain to {next_schedule!r} for another entity")
    return result


def dialogue_speakers(document: dict[str, object], path: Path, entities: set[str],
                      game_root: Path) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    speaker_ids: set[str] = set()
    player_count = 0
    for raw_speaker in expect(document.get("speakers"), list, f"{path}.speakers"):
        speaker = expect(raw_speaker, dict, f"{path}.speakers entry")
        speaker_id = identifier(speaker.get("id"), f"{path} speaker id")
        if speaker_id in speaker_ids:
            raise ContentError(f"{path}: duplicate speaker id {speaker_id!r}")
        speaker_ids.add(speaker_id)
        name = expect(speaker.get("name"), str, f"{path} speaker {speaker_id} name")
        if not name:
            raise ContentError(f"{path}: speaker {speaker_id!r} name cannot be empty")
        role = identifier(speaker.get("role"), f"{path} speaker {speaker_id} role")
        if role not in {"npc", "player", "narrator"}:
            raise ContentError(f"{path}: speaker {speaker_id!r} has unsupported role {role!r}")
        if role == "player":
            player_count += 1
        entity = speaker.get("entity")
        if entity is not None and identifier(entity, f"{path} speaker {speaker_id} entity") not in entities:
            raise ContentError(f"{path}: speaker {speaker_id!r} refers to unknown entity {entity!r}")
        portrait = speaker.get("portrait")
        portrait_width = 0
        portrait_height = 0
        if portrait is not None:
            portrait = expect(portrait, str, f"{path} speaker {speaker_id} portrait")
            portrait_path = path_inside(game_root, (game_root / portrait).resolve(),
                                        f"{path} speaker portrait")
            if not portrait_path.is_file():
                raise ContentError(f"{path}: missing speaker portrait {portrait!r}")
            portrait_data = portrait_path.read_bytes()
            if portrait_path.suffix.lower() != ".png" or portrait_data[:8] != b"\x89PNG\r\n\x1a\n":
                raise ContentError(f"{path}: speaker portrait must be a PNG: {portrait!r}")
            portrait_width = int.from_bytes(portrait_data[16:20], "big")
            portrait_height = int.from_bytes(portrait_data[20:24], "big")
        result.append({
            "id": speaker_id,
            "name": name,
            "role": role,
            "portrait": portrait,
            "portrait_width": portrait_width,
            "portrait_height": portrait_height,
        })
    if player_count > 1:
        raise ContentError(f"{path}: dialogue may define only one player speaker")
    return result


def dialogue_start_cases(document: dict[str, object], path: Path,
                         node_ids: set[str]) -> list[dict[str, object]]:
    """Validate an ordered dialogue entry selection.

    A string remains the compact form for an unconditional start. A list may
    place flag comparisons before one required final fallback.
    """
    raw_start = document.get("start")
    if isinstance(raw_start, str):
        cases = [{"node": identifier(raw_start, f"{path} start"),
                  "flag": None, "value": 0}]
    else:
        cases = []
        for index, raw_case in enumerate(expect(raw_start, list, f"{path} start")):
            case = expect(raw_case, dict, f"{path} start case {index}")
            node = identifier(case.get("node"), f"{path} start case {index} node")
            when = case.get("when")
            if when is None:
                flag = None
                value = 0
            else:
                condition = expect(when, dict, f"{path} start case {index} when")
                flag = identifier(condition.get("flag"),
                                  f"{path} start case {index} condition flag")
                value = integer(condition.get("equals"),
                                f"{path} start case {index} condition equals")
            cases.append({"node": node, "flag": flag, "value": value})
    if not cases:
        raise ContentError(f"{path}: dialogue start must not be empty")
    for index, case in enumerate(cases):
        if case["node"] not in node_ids:
            raise ContentError(f"{path}: start node {case['node']!r} does not exist")
        if case["flag"] is None and index != len(cases) - 1:
            raise ContentError(f"{path}: only the final dialogue start case may be unconditional")
    if cases[-1]["flag"] is not None:
        raise ContentError(f"{path}: final dialogue start case must be an unconditional fallback")
    return cases


def collect_dialogue(paths: list[Path], entities: set[str], game_root: Path) -> set[str]:
    result: set[str] = set()
    for path in paths:
        document = expect(load_jsonc(path), dict, str(path))
        dialogue_id = identifier(document.get("id"), f"{path} dialogue id")
        if dialogue_id in result:
            raise ContentError(f"duplicate dialogue id: {dialogue_id}")
        speakers = dialogue_speakers(document, path, entities, game_root)
        speaker_ids = {str(speaker["id"]) for speaker in speakers}
        speaker_roles = {str(speaker["id"]): str(speaker["role"]) for speaker in speakers}
        nodes = expect(document.get("nodes"), list, f"{path}.nodes")
        node_ids: set[str] = set()
        for node in nodes:
            node = expect(node, dict, f"{path}.nodes entry")
            node_id = identifier(node.get("id"), f"{path} node id")
            if node_id in node_ids:
                raise ContentError(f"{path}: duplicate node id {node_id}")
            node_ids.add(node_id)
            speaker = identifier(node.get("speaker"), f"{path} node {node_id} speaker")
            if speaker not in speaker_ids:
                raise ContentError(f"{path}: node {node_id} refers to unknown speaker {speaker!r}")
        dialogue_start_cases(document, path, node_ids)
        for node in nodes:
            node_id = identifier(node.get("id"), f"{path} node id")
            speaker_id = identifier(node.get("speaker"), f"{path} node {node_id} speaker")
            if "on_end" in node:
                identifier(node["on_end"], f"{path} node {node_id} on_end")
            if "next" in node and identifier(node["next"], f"{path} node next") not in node_ids:
                raise ContentError(f"{path}: node {node_id!r} refers to an unknown next node")
            if speaker_roles[speaker_id] == "player" and node.get("choices"):
                raise ContentError(f"{path}: player response node {node_id!r} cannot contain choices")
            for choice in node.get("choices", []):
                choice = expect(choice, dict, f"{path} choice")
                if "next" in choice and identifier(choice["next"], f"{path} choice next") not in node_ids:
                    raise ContentError(f"{path}: choice refers to an unknown next node")
                expect(choice.get("text"), str, f"{path} choice text")
                when = choice.get("when")
                if when is not None:
                    condition = expect(when, dict, f"{path} choice condition")
                    identifier(condition.get("flag"), f"{path} choice condition flag")
                    integer(condition.get("equals"), f"{path} choice condition equals")
                for action in expect(choice.get("actions", []), list, f"{path} choice actions"):
                    action = expect(action, dict, f"{path} choice action")
                    if identifier(action.get("op"), f"{path} choice action op") != "set":
                        raise ContentError(f"{path}: dialogue choices currently support only 'set' actions")
                    identifier(action.get("flag"), f"{path} choice action flag")
                    integer(action.get("value"), f"{path} choice action value")
        result.add(dialogue_id)
    return result


def collect_audio(paths: list[Path], game_root: Path) -> list[dict[str, object]]:
    tracks: list[dict[str, object]] = []
    seen: set[str] = set()
    for path in paths:
        document = expect(load_jsonc(path), dict, str(path))
        for raw_track in expect(document.get("tracks"), list, f"{path}.tracks"):
            track = expect(raw_track, dict, f"{path} track")
            track_id = identifier(track.get("id"), f"{path} track id")
            if track_id in seen:
                raise ContentError(f"duplicate music track id: {track_id}")
            seen.add(track_id)
            source_name = expect(track.get("source"), str, f"{path} track {track_id} source")
            source = path_inside(game_root, (game_root / source_name).resolve(),
                                 f"{path} track {track_id} source")
            if not source.is_file():
                raise ContentError(f"{path}: missing music source {source_name!r}")
            if source.suffix.lower() not in {".ogg", ".wav"}:
                raise ContentError(f"{path}: unsupported music source format {source.suffix!r}")
            sample_rate = integer(track.get("sample_rate", 11025), f"{path} track {track_id} sample_rate")
            if sample_rate < 8000 or sample_rate > 44100:
                raise ContentError(f"{path}: track {track_id!r} sample_rate must be between 8000 and 44100")
            volume = track.get("volume", 0.5)
            if isinstance(volume, bool) or not isinstance(volume, (int, float)):
                raise ContentError(f"{path} track {track_id} volume must be a number")
            volume = float(volume)
            if volume < 0.0 or volume > 1.0:
                raise ContentError(f"{path}: track {track_id!r} volume must be between 0 and 1")
            loop = track.get("loop", True)
            if not isinstance(loop, bool):
                raise ContentError(f"{path} track {track_id} loop must be a bool")
            tracks.append({
                "id": track_id,
                "source": source.relative_to(game_root).as_posix(),
                "sample_rate": sample_rate,
                "speed": sample_rate / 44100.0,
                "volume": volume,
                "loop": int(loop),
                "generated": f"generated/audio/{track_id}.wav",
            })
    tracks.sort(key=lambda item: str(item["id"]))
    return tracks


def collect_scripts(paths: list[Path], entities: dict[str, dict[str, object]],
                    dialogue: set[str], music: set[str],
                    schedules: dict[str, dict[str, object]]) -> dict[str, list[dict[str, object]]]:
    scripts: dict[str, list[dict[str, object]]] = {}
    allowed = {"set", "set_control", "set_entity_state", "show_dialogue",
               "change_scene", "move_entity", "wait", "play_music",
               "start_schedule", "stop_schedule", "set_entity_dialogue"}
    for path in paths:
        document = expect(load_jsonc(path), dict, str(path))
        script_id = identifier(document.get("id"), f"{path} script id")
        if script_id in scripts:
            raise ContentError(f"duplicate script id: {script_id}")
        actions = expect(document.get("actions"), list, f"{path}.actions")
        checked: list[dict[str, object]] = []
        for action in actions:
            action = expect(action, dict, f"{path} action")
            operation = identifier(action.get("op"), f"{path} action op")
            if operation not in allowed:
                raise ContentError(f"{path}: unsupported action {operation!r}")
            if operation == "show_dialogue" and identifier(action.get("dialogue"), f"{path} dialogue") not in dialogue:
                raise ContentError(f"{path}: unknown dialogue {action.get('dialogue')!r}")
            if operation == "set_entity_dialogue" and identifier(
                    action.get("dialogue"), f"{path} dialogue") not in dialogue:
                raise ContentError(f"{path}: unknown dialogue {action.get('dialogue')!r}")
            if operation == "play_music" and identifier(action.get("track"), f"{path} music track") not in music:
                raise ContentError(f"{path}: unknown music track {action.get('track')!r}")
            if operation in {"set_entity_state", "move_entity", "start_schedule",
                             "stop_schedule", "set_entity_dialogue"}:
                entity_id = identifier(action.get("entity"), f"{path} entity")
                if entity_id not in entities:
                    raise ContentError(f"{path}: unknown entity {entity_id!r}")
                if operation == "set_entity_dialogue" and entities[entity_id]["kind"] != "actor":
                    raise ContentError(f"{path}: set_entity_dialogue requires an actor entity")
                if operation == "set_entity_state":
                    state = identifier(action.get("state"), f"{path} entity state")
                    if state not in entities[entity_id]["states"]:
                        raise ContentError(f"{path}: entity {entity_id!r} has no state {state!r}")
                elif operation == "move_entity":
                    integer(action.get("x"), f"{path} move_entity x")
                    integer(action.get("y"), f"{path} move_entity y")
                    facing = identifier(action.get("facing"), f"{path} move_entity facing")
                    if facing not in {"north", "south", "west", "east"}:
                        raise ContentError(f"{path}: unsupported move_entity facing {facing!r}")
                elif operation == "start_schedule":
                    schedule_id = identifier(action.get("schedule"), f"{path} schedule")
                    if schedule_id not in schedules:
                        raise ContentError(f"{path}: unknown schedule {schedule_id!r}")
                    if schedules[schedule_id]["entity"] != entity_id:
                        raise ContentError(
                            f"{path}: schedule {schedule_id!r} belongs to entity "
                            f"{schedules[schedule_id]['entity']!r}, not {entity_id!r}")
                    if integer(action.get("delay_frames", 0),
                               f"{path} start_schedule delay_frames") < 0:
                        raise ContentError(f"{path}: start_schedule delay_frames cannot be negative")
            if operation == "set_control" and not isinstance(action.get("enabled"), bool):
                raise ContentError(f"{path}: set_control enabled must be a bool")
            if operation == "wait" and integer(action.get("frames"), f"{path} wait frames") < 1:
                raise ContentError(f"{path}: wait frames must be positive")
            checked.append(action)
        scripts[script_id] = checked
    return scripts


def path_inside(root: Path, path: Path, description: str) -> Path:
    try:
        path.relative_to(root.resolve())
    except ValueError as error:
        raise ContentError(f"{description} leaves gamedata: {path}") from error
    return path


def validate_tilesets(map_path: Path, document: dict[str, object], game_root: Path) -> tuple[set[str], set[int]]:
    result: set[str] = set()
    valid_gids: set[int] = set()
    for entry in expect(document.get("tilesets", []), list, f"{map_path}.tilesets"):
        entry = expect(entry, dict, f"{map_path}.tilesets entry")
        first_gid = expect(entry.get("firstgid"), int, f"{map_path} tileset firstgid")
        source = expect(entry.get("source"), str, f"{map_path} tileset source")
        tileset_path = path_inside(game_root, (map_path.parent / source).resolve(), f"{map_path} tileset source")
        if not tileset_path.is_file():
            raise ContentError(f"{map_path}: missing tileset {source!r}")
        tileset = expect(load_jsonc(tileset_path), dict, str(tileset_path))
        if tileset.get("type") != "tileset":
            raise ContentError(f"{tileset_path}: expected a Tiled tileset")
        tile_entries = list(expect(tileset.get("tiles", []), list, f"{tileset_path}.tiles"))
        image_entries = list(tile_entries)
        if "image" in tileset:
            image_entries.append(tileset)
        for tile in image_entries:
            tile = expect(tile, dict, f"{tileset_path} tile")
            if "image" not in tile:
                continue
            image_source = expect(tile["image"], str, f"{tileset_path} tile image")
            image_path = path_inside(game_root, (tileset_path.parent / image_source).resolve(), f"{tileset_path} tile image")
            if not image_path.is_file():
                raise ContentError(f"{tileset_path}: missing tile image {image_source!r}")
            if image_path.suffix.lower() != ".png":
                raise ContentError(f"{tileset_path}: unsupported tile image format {image_path.suffix!r}")
            if image_path.read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
                raise ContentError(f"{tileset_path}: tile image is not a PNG: {image_source!r}")
        if tile_entries:
            for tile in tile_entries:
                tile = expect(tile, dict, f"{tileset_path} tile")
                valid_gids.add(first_gid + expect(tile.get("id"), int, f"{tileset_path} tile id"))
        elif "tilecount" in tileset:
            tile_count = expect(tileset["tilecount"], int, f"{tileset_path} tilecount")
            valid_gids.update(range(first_gid, first_gid + tile_count))
        result.add(tileset_path.relative_to(game_root).as_posix())
    return result, valid_gids


def collect_scenes(paths: list[Path], entities: set[str], scripts: dict[str, list[dict[str, object]]], game_root: Path) -> tuple[dict[str, set[str]], set[str]]:
    scenes: dict[str, set[str]] = {}
    tilesets: set[str] = set()
    for path in paths:
        document = expect(load_jsonc(path), dict, str(path))
        scene_properties = properties(document.get("properties"), str(path))
        scene_id = identifier(scene_properties.get("id"), f"{path} scene id")
        if scene_id in scenes:
            raise ContentError(f"duplicate scene id: {scene_id}")
        if "on_enter" in scene_properties:
            entry_script = identifier(scene_properties["on_enter"], f"{path} scene on_enter")
            if entry_script not in scripts:
                raise ContentError(f"{path}: unknown on_enter script {entry_script!r}")
        scene_tilesets, valid_gids = validate_tilesets(path, document, game_root)
        tilesets.update(scene_tilesets)
        spawns: set[str] = set()
        object_ids: set[str] = set()
        for layer in expect(document.get("layers"), list, f"{path}.layers"):
            if isinstance(layer, dict) and layer.get("type") == "tilelayer":
                for gid in expect(layer.get("data"), list, f"{path} tile layer data"):
                    gid = expect(gid, int, f"{path} tile layer gid")
                    if gid != 0 and gid not in valid_gids:
                        raise ContentError(f"{path}: tile layer uses unknown tile gid {gid}")
            if not isinstance(layer, dict) or layer.get("type") != "objectgroup":
                continue
            for obj in expect(layer.get("objects", []), list, f"{path} object layer"):
                obj = expect(obj, dict, f"{path} map object")
                if "gid" in obj and expect(obj["gid"], int, f"{path} object gid") not in valid_gids:
                    raise ContentError(f"{path}: object uses unknown tile gid {obj['gid']}")
                values = properties(obj.get("properties"), f"{path} object {obj.get('name', '?')}")
                kind = values.get("kind")
                if kind is None:
                    continue
                kind = identifier(kind, f"{path} object kind")
                if kind == "spawn":
                    object_id = identifier(values.get("id"), f"{path} object id")
                    if object_id in object_ids:
                        raise ContentError(f"{path}: duplicate object id {object_id}")
                    object_ids.add(object_id)
                    spawns.add(object_id)
                elif kind == "entity":
                    object_id = identifier(values.get("id"), f"{path} object id")
                    if object_id in object_ids:
                        raise ContentError(f"{path}: duplicate object id {object_id}")
                    object_ids.add(object_id)
                    entity_def = identifier(values.get("entity_def"), f"{path} entity_def")
                    if entity_def not in entities:
                        raise ContentError(f"{path}: unknown entity definition {entity_def!r}")
                elif kind in {"hotspot", "exit", "trigger"}:
                    object_id = identifier(values.get("id"), f"{path} object id")
                    if object_id in object_ids:
                        raise ContentError(f"{path}: duplicate object id {object_id}")
                    object_ids.add(object_id)
                elif kind == "tiled_occluder":
                    object_id = identifier(values.get("id"), f"{path} object id")
                    if object_id in object_ids:
                        raise ContentError(f"{path}: duplicate object id {object_id}")
                    object_ids.add(object_id)
                    tile_gid = expect(values.get("tile_gid"), int, f"{path} tiled occluder tile_gid")
                    if tile_gid not in valid_gids:
                        raise ContentError(f"{path}: tiled occluder uses unknown tile gid {tile_gid}")
                elif kind != "collision":
                    raise ContentError(f"{path}: unsupported object kind {kind!r}")
                for field in ("interaction", "script"):
                    if field in values and identifier(values[field], f"{path} object {field}") not in scripts:
                        raise ContentError(f"{path}: unknown script {values[field]!r}")
        scenes[scene_id] = spawns
    for script_id, actions in scripts.items():
        for action in actions:
            if action["op"] != "change_scene":
                continue
            scene_id = identifier(action.get("scene"), f"script {script_id} scene")
            spawn_id = identifier(action.get("spawn"), f"script {script_id} spawn")
            if scene_id not in scenes:
                raise ContentError(f"script {script_id}: unknown scene {scene_id!r}")
            if spawn_id not in scenes[scene_id]:
                raise ContentError(f"script {script_id}: scene {scene_id!r} has no spawn {spawn_id!r}")
    return scenes, tilesets


def enum_name(prefix: str, value: str) -> str:
    return f"{prefix}_{re.sub(r'[^A-Z0-9_]', '_', value.upper())}"


def write_output(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def materialize_audio(tracks: list[dict[str, object]], game_root: Path, output_root: Path) -> None:
    if not tracks:
        return
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise ContentError("ffmpeg is required to convert authored Ogg/WAV music")
    audio_root = output_root / "audio"
    audio_root.mkdir(parents=True, exist_ok=True)
    for track in tracks:
        source = game_root / str(track["source"])
        output = output_root.parent / str(track["generated"])
        temporary = output.with_suffix(".tmp.wav")
        command = [
            ffmpeg, "-v", "error", "-y", "-i", str(source), "-vn",
            "-map_metadata", "-1", "-fflags", "+bitexact", "-flags:a", "+bitexact",
            "-ac", "2", "-ar", str(track["sample_rate"]), "-c:a", "pcm_s16le",
            str(temporary),
        ]
        try:
            completed = subprocess.run(command, check=False, stdout=subprocess.DEVNULL,
                                       stderr=subprocess.PIPE, text=True)
        except OSError as error:
            raise ContentError(f"cannot run ffmpeg for music track {track['id']!r}: {error}") from error
        if completed.returncode != 0:
            raise ContentError(
                f"ffmpeg failed for music track {track['id']!r}: {completed.stderr.strip()}")
        try:
            unchanged = output.is_file() and output.read_bytes() == temporary.read_bytes()
            if unchanged:
                temporary.unlink()
            else:
                temporary.replace(output)
        except OSError as error:
            raise ContentError(f"cannot materialize music track {track['id']!r}: {error}") from error


def write_generated_png(path: Path, image: object) -> None:
    """Write a derived PNG only when its pixels changed."""
    try:
        from PIL import Image
        if path.exists():
            with Image.open(path) as existing:
                if existing.size == image.size and existing.convert("RGBA").tobytes() == image.tobytes():
                    return
        path.parent.mkdir(parents=True, exist_ok=True)
        image.save(path)
    except OSError as error:
        raise ContentError(f"cannot write generated image {path}: {error}") from error


def materialize_background_chunks(scenes: list[dict[str, object]], game_root: Path, output_root: Path) -> None:
    """Bake ordered authored tile layers into cullable Vircon background chunks."""
    try:
        from PIL import Image
    except ImportError as error:
        raise ContentError("Pillow is required to build background chunks") from error

    for scene in scenes:
        scene_id = expect(scene["id"], str, "runtime scene id")
        width = integer(scene["width"], "runtime scene width")
        height = integer(scene["height"], "runtime scene height")
        tile_size = integer(scene["tile_size"], "runtime scene tile size")
        terrain = expect(scene["terrain"], list, "runtime terrain")
        map_image = Image.new("RGBA", (width * tile_size, height * tile_size), (0, 0, 0, 0))
        image_cache: dict[str, object] = {}
        for placement in terrain:
            placement = expect(placement, dict, "runtime terrain placement")
            image = expect(placement["image"], str, "runtime terrain image")
            if image not in image_cache:
                source_path = game_root / image
                try:
                    with Image.open(source_path) as source:
                        image_cache[image] = source.convert("RGBA")
                except OSError as error:
                    raise ContentError(f"cannot read terrain tile {image}: {error}") from error
            map_image.alpha_composite(
                image_cache[image],
                (integer(placement["x"], "runtime terrain x"),
                 integer(placement["y"], "runtime terrain y")),
            )
        background: list[dict[str, object]] = []
        for tile_y in range(0, height, BACKGROUND_CHUNK_TILES_Y):
            for tile_x in range(0, width, BACKGROUND_CHUNK_TILES_X):
                chunk_columns = min(BACKGROUND_CHUNK_TILES_X, width - tile_x)
                chunk_rows = min(BACKGROUND_CHUNK_TILES_Y, height - tile_y)
                chunk_width = chunk_columns * tile_size
                chunk_height = chunk_rows * tile_size
                canvas = map_image.crop((tile_x * tile_size, tile_y * tile_size,
                                         tile_x * tile_size + chunk_width,
                                         tile_y * tile_size + chunk_height))
                if canvas.getbbox() is None:
                    continue
                relative_output = Path("backgrounds") / f"{scene_id}-{tile_x:03d}-{tile_y:03d}.png"
                write_generated_png(output_root / relative_output, canvas)
                background.append({
                    "image": GENERATED_RESOURCE_PREFIX + relative_output.as_posix(),
                    "width": chunk_width,
                    "height": chunk_height,
                    "x": tile_x * tile_size,
                    "y": tile_y * tile_size,
                })
        scene["background"] = background


def materialize_scenery_strips(scenes: list[dict[str, object]], game_root: Path,
                               output_root: Path) -> None:
    """Bake repeated tile runs, sharing identical strip textures between rows."""
    try:
        from PIL import Image
    except ImportError as error:
        raise ContentError("Pillow is required to build scenery strips") from error

    generated_strips: dict[tuple[str, int, int], Path] = {}
    used_outputs: set[Path] = set()
    for scene in scenes:
        for item in expect(scene["scenery"], list, "runtime scenery"):
            repeat_count = item.pop("repeat_count", None)
            if repeat_count is None:
                continue
            repeat_count = integer(repeat_count, "runtime scenery repeat count")
            item.pop("repeat_key", None)
            source_image = expect(item["image"], str, "runtime scenery image")
            tile_size = integer(scene["tile_size"], "runtime scene tile size")
            strip_key = (source_image, repeat_count, tile_size)
            relative_output = generated_strips.get(strip_key)
            if relative_output is None:
                source_path = game_root / source_image
                try:
                    with Image.open(source_path) as source:
                        tile = source.convert("RGBA")
                except OSError as error:
                    raise ContentError(f"cannot read scenery tile {source_path}: {error}") from error
                if tile.size != (tile_size, tile_size):
                    raise ContentError(
                        f"tiled occluder image {source_path} is {tile.width}x{tile.height}, expected "
                        f"{tile_size}x{tile_size}"
                    )
                strip = Image.new("RGBA", (repeat_count * tile_size, tile_size), (0, 0, 0, 0))
                for column in range(repeat_count):
                    strip.alpha_composite(tile, (column * tile_size, 0))
                source_stem = re.sub(r"[^a-zA-Z0-9_-]", "_", Path(source_image).stem)
                source_hash = hashlib.sha256(source_image.encode("utf-8")).hexdigest()[:8]
                relative_output = Path("scenery") / f"{source_stem}-{source_hash}-x{repeat_count}.png"
                write_generated_png(output_root / relative_output, strip)
                generated_strips[strip_key] = relative_output
            used_outputs.add(relative_output)
            item["image"] = GENERATED_RESOURCE_PREFIX + relative_output.as_posix()
            item["width"] = repeat_count * tile_size
            item["height"] = tile_size
    scenery_root = output_root / "scenery"
    if scenery_root.is_dir():
        for existing in scenery_root.glob("*.png"):
            if existing.relative_to(output_root) not in used_outputs:
                existing.unlink()


def materialize_character_sheets(visuals: list[dict[str, object]], game_root: Path,
                                 output_root: Path) -> None:
    """Pack each actor's authored frame PNGs into one runtime texture."""
    try:
        from PIL import Image
    except ImportError as error:
        raise ContentError("Pillow is required to build character sheets") from error

    used_outputs: set[Path] = set()
    for visual in visuals:
        source_character = expect(visual["source_character"], str, "runtime actor source_character")
        frame_groups = expect(visual["frames"], list, "runtime actor frames")
        frames = [frame for group in frame_groups for frame in expect(group, list, "runtime actor frame group")
                  if frame is not None]
        unique_frames: dict[str, dict[str, object]] = {}
        for frame_value in frames:
            frame = expect(frame_value, dict, "runtime actor frame")
            image = expect(frame["image"], str, "runtime actor image")
            unique_frames.setdefault(image, frame)

        loaded: dict[str, object] = {}
        for image, frame in unique_frames.items():
            source_path = game_root / image
            try:
                with Image.open(source_path) as source:
                    loaded[image] = source.convert("RGBA")
            except OSError as error:
                raise ContentError(f"cannot read actor frame {source_path}: {error}") from error
            expected_size = (integer(frame["width"], "runtime actor frame width"),
                             integer(frame["height"], "runtime actor frame height"))
            if loaded[image].size != expected_size:
                raise ContentError(
                    f"actor frame {source_path} is {loaded[image].width}x{loaded[image].height}, "
                    f"expected {expected_size[0]}x{expected_size[1]}"
                )
            if loaded[image].width > 1024 or loaded[image].height > 1024:
                raise ContentError(f"actor frame {source_path} exceeds Vircon32's 1024x1024 texture limit")

        placements: dict[str, tuple[int, int]] | None = None
        sheet_width = 0
        sheet_height = 0
        for candidate_width in CHARACTER_SHEET_WIDTHS:
            candidate: dict[str, tuple[int, int]] = {}
            cursor_x = 0
            cursor_y = 0
            row_height = 0
            used_width = 0
            for image, source in loaded.items():
                if cursor_x and cursor_x + source.width > candidate_width:
                    cursor_x = 0
                    cursor_y += row_height
                    row_height = 0
                candidate[image] = (cursor_x, cursor_y)
                cursor_x += source.width
                row_height = max(row_height, source.height)
                used_width = max(used_width, cursor_x)
            candidate_height = cursor_y + row_height
            if candidate_height <= 1024:
                placements = candidate
                sheet_width = used_width
                sheet_height = candidate_height
                break
        if placements is None or sheet_width <= 0 or sheet_height <= 0:
            raise ContentError(
                f"actor tileset {source_character!r} cannot fit in one Vircon32 1024x1024 texture"
            )

        sheet = Image.new("RGBA", (sheet_width, sheet_height), (0, 0, 0, 0))
        for image, source in loaded.items():
            sheet.alpha_composite(source, placements[image])
        source_stem = re.sub(r"[^a-zA-Z0-9_-]", "_", Path(source_character).stem)
        source_hash = hashlib.sha256(source_character.encode("utf-8")).hexdigest()[:8]
        relative_output = Path("characters") / f"{source_stem}-{source_hash}.png"
        write_generated_png(output_root / relative_output, sheet)
        used_outputs.add(relative_output)
        runtime_image = GENERATED_RESOURCE_PREFIX + relative_output.as_posix()
        for frame_value in frames:
            frame = expect(frame_value, dict, "runtime actor frame")
            authored_image = expect(frame["image"], str, "runtime actor image")
            frame["image"] = runtime_image
            frame["source_x"], frame["source_y"] = placements[authored_image]

    character_root = output_root / "characters"
    if character_root.is_dir():
        for existing in character_root.glob("*.png"):
            if existing.relative_to(output_root) not in used_outputs:
                existing.unlink()


def materialize_dialogue_glyph_atlas(dialogues: list[dict[str, object]],
                                     schedules: list[dict[str, object]], game_root: Path,
                                     output_root: Path) -> tuple[list[dict[str, object]], int]:
    """Render each distinct UI glyph once and encode dialogue and ambient speech."""
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError as error:
        raise ContentError("Pillow is required to build the dialogue glyph atlas") from error

    font_path = game_root / "assets/ui/fonts/Aclonica.ttf"
    if not font_path.is_file():
        raise ContentError(f"missing dialogue font: {font_path.relative_to(game_root)}")
    try:
        font = ImageFont.truetype(str(font_path), DIALOGUE_TEXT_FONT_SIZE)
    except OSError as error:
        raise ContentError(f"cannot load dialogue font: {error}") from error
    font_ascent, font_descent = font.getmetrics()
    if font_ascent + font_descent > DIALOGUE_TEXT_LINE_HEIGHT:
        raise ContentError(
            f"dialogue font metrics need {font_ascent + font_descent} pixels, "
            f"but line height is {DIALOGUE_TEXT_LINE_HEIGHT}"
        )

    character_set: set[str] = set()
    for dialogue in dialogues:
        for speaker in expect(dialogue["speakers"], list, "runtime dialogue speakers"):
            character_set.update(expect(speaker["name"], str, "dialogue speaker name"))
        for node in expect(dialogue["nodes"], list, "runtime dialogue nodes"):
            character_set.update(expect(node["text"], str, "dialogue node text"))
            for choice in expect(node["choices"], list, "runtime dialogue choices"):
                character_set.update(expect(choice["text"], str, "dialogue choice text"))
    for schedule in schedules:
        speech = schedule.get("speech")
        if speech is not None:
            for line in expect(speech["lines"], list, "runtime schedule speech lines"):
                character_set.update(expect(line, str, "runtime schedule speech line"))
    character_set.discard(" ")
    character_set.discard("\n")
    character_set.discard("\r")
    character_set.discard("\t")

    rendered: dict[str, object] = {}
    glyph_origins: dict[str, int] = {}
    for character in sorted(character_set):
        advance = max(1, int(font.getlength(character) + 0.5))
        left, _, right, _ = font.getbbox(character, anchor="ls")
        left_edge = min(0, left)
        width = max(advance, right) - left_edge
        origin_x = -left_edge
        if width + 2 > DIALOGUE_ATLAS_WIDTH:
            raise ContentError(f"dialogue glyph is too wide for the font atlas: {character!r}")
        image = Image.new("RGBA", (width, DIALOGUE_TEXT_LINE_HEIGHT), (0, 0, 0, 0))
        ImageDraw.Draw(image).text(
            (origin_x, font_ascent), character, font=font,
            fill=(255, 255, 255, 255), anchor="ls"
        )
        rendered[character] = image
        glyph_origins[character] = origin_x

    pages: list[object] = []
    page = Image.new("RGBA", (DIALOGUE_ATLAS_WIDTH, DIALOGUE_ATLAS_HEIGHT), (0, 0, 0, 0))
    page_index = 0
    x = 1
    y = 1
    used_x = 1
    used_y = 1
    entries: list[dict[str, object]] = []
    for character in sorted(character_set):
        image = rendered[character]
        width, height = image.size
        if x + width + 1 > DIALOGUE_ATLAS_WIDTH:
            x = 1
            y += DIALOGUE_TEXT_LINE_HEIGHT + 2
        if y + height + 1 > DIALOGUE_ATLAS_HEIGHT:
            pages.append((page, used_x, used_y))
            page = Image.new("RGBA", (DIALOGUE_ATLAS_WIDTH, DIALOGUE_ATLAS_HEIGHT), (0, 0, 0, 0))
            page_index += 1
            x = 1
            y = 1
            used_x = 1
            used_y = 1
        page.alpha_composite(image, (x, y))
        relative_output = Path("dialogue") / f"glyphs-{page_index:03d}.png"
        entries.append({
            "character": character,
            "image": GENERATED_RESOURCE_PREFIX + relative_output.as_posix(),
            "x": x,
            "y": y,
            "width": width,
            "height": height,
            "origin_x": glyph_origins[character],
            "advance": max(1, int(font.getlength(character) + 0.5)),
        })
        x += width + 2
        used_x = max(used_x, x)
        used_y = max(used_y, y + height + 1)
    if character_set:
        pages.append((page, used_x, used_y))

    for index, (atlas, width, height) in enumerate(pages):
        relative_output = Path("dialogue") / f"glyphs-{index:03d}.png"
        write_generated_png(output_root / relative_output, atlas.crop((0, 0, width, height)))

    glyph_index = {str(entry["character"]): index for index, entry in enumerate(entries)}

    def encode(text: str) -> list[int]:
        return [-1 if character.isspace() else glyph_index[character] for character in text]

    for dialogue in dialogues:
        for speaker in dialogue["speakers"]:
            speaker["name_glyphs"] = encode(str(speaker["name"]))
        for node in dialogue["nodes"]:
            node["text_glyphs"] = encode(str(node["text"]))
            for choice in node["choices"]:
                choice["text_glyphs"] = encode(str(choice["text"]))
    for schedule in schedules:
        speech = schedule.get("speech")
        if speech is not None:
            speech["line_glyphs"] = [encode(str(line)) for line in speech["lines"]]
    return entries, max(1, int(font.getlength(" ") + 0.5))


def collect_runtime_resources(scenes: list[dict[str, object]], visuals: list[dict[str, object]],
                              object_visuals: list[dict[str, object]],
                              dialogues: list[dict[str, object]], glyphs: list[dict[str, object]]) -> list[str]:
    """Return image references in exactly the order supplied to the ROM builder."""
    resources: set[str] = set()
    for scene in scenes:
        for item in expect(scene["background"], list, "runtime background"):
            resources.add(expect(item["image"], str, "runtime background image"))
        for item in expect(scene["scenery"], list, "runtime scenery"):
            resources.add(expect(item["image"], str, "runtime scenery image"))
    for visual in visuals:
        frames = expect(visual["frames"], list, "runtime actor frames")
        for frame in frames[0] + frames[1]:
            if frame is not None:
                resources.add(expect(frame["image"], str, "runtime actor image"))
    for visual in object_visuals:
        for frame in expect(visual["frames"], list, "runtime object frames"):
            if frame is not None:
                resources.add(expect(frame["image"], str, "runtime object image"))
    for dialogue in dialogues:
        for speaker in expect(dialogue["speakers"], list, "runtime dialogue speakers"):
            portrait = speaker.get("portrait")
            if portrait is not None:
                resources.add(expect(portrait, str, "runtime dialogue portrait"))
    for glyph in glyphs:
        resources.add(expect(glyph["image"], str, "runtime dialogue glyph atlas"))
    return (sorted(path for path in resources if not path.startswith(GENERATED_RESOURCE_PREFIX)) +
            sorted(path for path in resources if path.startswith(GENERATED_RESOURCE_PREFIX)))


def scene_tiles(map_path: Path, document: dict[str, object], game_root: Path) -> dict[int, dict[str, object]]:
    """Resolve collection-tile GIDs into compiler-friendly metadata."""
    result: dict[int, dict[str, object]] = {}
    for entry in expect(document.get("tilesets", []), list, f"{map_path}.tilesets"):
        entry = expect(entry, dict, f"{map_path}.tilesets entry")
        first_gid = expect(entry.get("firstgid"), int, f"{map_path} tileset firstgid")
        source = expect(entry.get("source"), str, f"{map_path} tileset source")
        tileset_path = path_inside(game_root, (map_path.parent / source).resolve(), f"{map_path} tileset source")
        tileset = expect(load_jsonc(tileset_path), dict, str(tileset_path))
        for tile in expect(tileset.get("tiles", []), list, f"{tileset_path}.tiles"):
            tile = expect(tile, dict, f"{tileset_path} tile")
            image = tile.get("image")
            if image is None:
                continue
            image = expect(image, str, f"{tileset_path} tile image")
            image_path = path_inside(game_root, (tileset_path.parent / image).resolve(), f"{tileset_path} tile image")
            result[first_gid + expect(tile.get("id"), int, f"{tileset_path} tile id")] = {
                "image": image_path.relative_to(game_root).as_posix(),
                "imagewidth": expect(tile.get("imagewidth"), int, f"{tileset_path} tile imagewidth"),
                "imageheight": expect(tile.get("imageheight"), int, f"{tileset_path} tile imageheight"),
                "properties": properties(tile.get("properties"), f"{tileset_path} tile"),
            }
    return result


def integer(value: object, description: str) -> int:
    return expect(value, int, description)


def runtime_dialogues(dialogue_paths: list[Path], script_paths: list[Path], entities: set[str],
                      game_root: Path, script_index: dict[str, int]) -> tuple[list[dict[str, object]], dict[str, str], list[str]]:
    """Compile dialogue nodes, choices, and their deliberately small effects."""
    dialogues: list[dict[str, object]] = []
    flags: set[str] = set()
    for path in dialogue_paths:
        document = expect(load_jsonc(path), dict, str(path))
        dialogue_id = identifier(document.get("id"), f"{path} dialogue id")
        raw_speakers = dialogue_speakers(document, path, entities, game_root)
        speaker_index = {str(speaker["id"]): index for index, speaker in enumerate(raw_speakers)}
        raw_nodes = expect(document.get("nodes"), list, f"{path}.nodes")
        if not raw_nodes:
            raise ContentError(f"{path}: dialogue must contain at least one node")
        node_index = {identifier(expect(node, dict, f"{path}.node").get("id"), f"{path} node id"): index
                      for index, node in enumerate(raw_nodes)}
        starts = dialogue_start_cases(document, path, set(node_index))
        for start in starts:
            if start["flag"] is not None:
                flags.add(str(start["flag"]))
        nodes: list[dict[str, object]] = []
        for index, raw_node in enumerate(raw_nodes):
            node = expect(raw_node, dict, f"{path}.node")
            choices: list[dict[str, object]] = []
            for raw_choice in expect(node.get("choices", []), list, f"{path} choices"):
                choice = expect(raw_choice, dict, f"{path} choice")
                raw_condition = choice.get("when")
                if raw_condition is None:
                    condition_flag = None
                    condition_value = 0
                else:
                    condition = expect(raw_condition, dict, f"{path} choice condition")
                    condition_flag = identifier(
                        condition.get("flag"), f"{path} choice condition flag")
                    condition_value = integer(
                        condition.get("equals"), f"{path} choice condition equals")
                    flags.add(condition_flag)
                actions: list[dict[str, int]] = []
                for raw_action in expect(choice.get("actions", []), list, f"{path} choice actions"):
                    action = expect(raw_action, dict, f"{path} choice action")
                    flag = identifier(action.get("flag"), f"{path} choice action flag")
                    flags.add(flag)
                    actions.append({"flag": flag, "value": integer(action.get("value"), f"{path} choice action value")})
                choices.append({
                    "text": expect(choice.get("text"), str, f"{path} choice text"),
                    "next": node_index[identifier(choice.get("next"), f"{path} choice next")],
                    "actions": actions,
                    "condition_flag": condition_flag,
                    "condition_value": condition_value,
                })
            explicit_next = node.get("next")
            if explicit_next is not None:
                next_node = node_index[identifier(explicit_next, f"{path} node next")]
            elif choices or node.get("end", False):
                next_node = -1
            else:
                next_node = index + 1 if index + 1 < len(raw_nodes) else -1
            speaker_id = identifier(node.get("speaker"), f"{path} node speaker")
            nodes.append({
                "text": expect(node.get("text"), str, f"{path} node text"),
                "speaker": speaker_index[speaker_id],
                "choices": choices,
                "next": next_node,
                "on_end": (-1 if node.get("on_end") is None else
                           script_index[identifier(node.get("on_end"),
                                                   f"{path} node on_end")]),
            })
        dialogues.append({
            "id": dialogue_id,
            "starts": [{
                "node": node_index[str(start["node"])],
                "flag": start["flag"],
                "value": start["value"],
            } for start in starts],
            "on_end": (-1 if document.get("on_end") is None else
                       script_index[identifier(document.get("on_end"), f"{path} on_end")]),
            "speakers": raw_speakers,
            "nodes": nodes,
        })
    dialogues.sort(key=lambda item: str(item["id"]))
    dialogue_ids = {str(item["id"]) for item in dialogues}
    script_dialogues: dict[str, str] = {}
    for path in script_paths:
        document = expect(load_jsonc(path), dict, str(path))
        script_id = identifier(document.get("id"), f"{path} script id")
        for action in expect(document.get("actions"), list, f"{path}.actions"):
            action = expect(action, dict, f"{path} action")
            if action.get("op") == "show_dialogue":
                dialogue_id = identifier(action.get("dialogue"), f"{path} dialogue")
                if dialogue_id not in dialogue_ids:
                    raise ContentError(f"{path}: unknown dialogue {dialogue_id!r}")
                script_dialogues[script_id] = dialogue_id
                break
    flag_index = {flag: index for index, flag in enumerate(sorted(flags))}
    for dialogue in dialogues:
        for start in dialogue["starts"]:
            if start["flag"] is not None:
                start["flag"] = flag_index[str(start["flag"])]
        for node in dialogue["nodes"]:
            for choice in node["choices"]:
                if choice["condition_flag"] is not None:
                    choice["condition_flag"] = flag_index[str(choice["condition_flag"])]
                for action in choice["actions"]:
                    action["flag"] = flag_index[str(action["flag"])]
    return dialogues, script_dialogues, sorted(flags)


def runtime_schedules(schedules: dict[str, dict[str, object]],
                      entity_definitions: dict[str, dict[str, object]],
                      scenes: list[dict[str, object]]) -> list[dict[str, object]]:
    entity_index = {entity_id: index for index, entity_id in enumerate(sorted(entity_definitions))}
    scene_index = {str(scene["id"]): index for index, scene in enumerate(scenes)}
    object_index = {
        (str(scene["id"]), str(item["id"])): item
        for scene in scenes for item in scene["scenery"] if "id" in item
    }
    facing_index = {"north": 0, "south": 1, "west": 2, "east": 3}
    operation_index = {"move": 0, "place": 1, "wait": 2,
                       "set_object_state": 3, "toggle_object_state": 4,
                       "speak": 5, "jump": 6, "random_jump": 7}
    schedule_index = {schedule_id: index for index, schedule_id in enumerate(sorted(schedules))}
    result: list[dict[str, object]] = []
    for schedule_id in sorted(schedules):
        schedule = schedules[schedule_id]
        authored_speech = schedule["speech"]
        ambient_lines = ([] if authored_speech is None else
                         list(authored_speech["lines"]))
        speech_lines = list(ambient_lines)
        labels = {
            str(step["label"]): index
            for index, step in enumerate(schedule["steps"])
            if "label" in step
        }
        branches: list[int] = []
        steps: list[tuple[int, int, int, int, int, int, int, int, int]] = []
        for step_number, step in enumerate(schedule["steps"]):
            operation = str(step["op"])
            delay_min = int(step["delay_min"])
            delay_max = int(step["delay_max"])
            if operation == "wait":
                steps.append((operation_index[operation], -1, 0, 0, 0,
                              delay_min, delay_max, -1, -1))
                continue
            if operation == "speak":
                text = str(step["text"])
                if text not in speech_lines:
                    speech_lines.append(text)
                steps.append((operation_index[operation], -1, 0, 0, 0,
                              delay_min, delay_max, speech_lines.index(text),
                              int(step["duration"])))
                continue
            if operation == "jump":
                steps.append((operation_index[operation], -1, 0, 0, 0,
                              delay_min, delay_max, labels[str(step["target"])], 0))
                continue
            if operation == "random_jump":
                first_branch = len(branches)
                branches.extend(labels[str(target)] for target in step["targets"])
                steps.append((operation_index[operation], -1, 0, 0, 0,
                              delay_min, delay_max, first_branch,
                              len(step["targets"])))
                continue
            scene_id = str(step["scene"])
            if scene_id not in scene_index:
                raise ContentError(
                    f"schedule {schedule_id!r} step {step_number} refers to unknown scene {scene_id!r}")
            if operation in {"move", "place"}:
                scene = scenes[scene_index[scene_id]]
                x = int(step["x"])
                y = int(step["y"])
                if x < 0 or y < 0 or x >= int(scene["width"]) or y >= int(scene["height"]):
                    raise ContentError(
                        f"schedule {schedule_id!r} step {step_number} position ({x}, {y}) "
                        f"is outside scene {scene_id!r}")
                steps.append((operation_index[operation], scene_index[scene_id], x, y,
                              facing_index[str(step["facing"])], delay_min, delay_max,
                              -1, -1))
                continue
            object_id = str(step["object"])
            target = object_index.get((scene_id, object_id))
            if target is None:
                raise ContentError(
                    f"schedule {schedule_id!r} step {step_number} refers to unknown stateful "
                    f"object {object_id!r} in scene {scene_id!r}")
            states = list(target["states"])
            value = -1
            if operation == "set_object_state":
                state = str(step["state"])
                if state not in states:
                    raise ContentError(
                        f"schedule {schedule_id!r} step {step_number} gives object "
                        f"{object_id!r} unknown state {state!r}")
                value = states.index(state)
            elif len(states) != 2:
                raise ContentError(
                    f"schedule {schedule_id!r} step {step_number} can only toggle a "
                    f"two-state object; {object_id!r} has {len(states)} states")
            steps.append((operation_index[operation], scene_index[scene_id], 0, 0, 0,
                          delay_min, delay_max, int(target["state_slot"]), value))
        speech = None
        if speech_lines:
            speech = {
                "lines": speech_lines,
                "ambient_count": len(ambient_lines),
                "delay_min": 0 if authored_speech is None else authored_speech["delay_min"],
                "delay_max": 0 if authored_speech is None else authored_speech["delay_max"],
                "duration": 0 if authored_speech is None else authored_speech["duration"],
            }
        result.append({"id": schedule_id,
                       "entity": entity_index[str(schedule["entity"])],
                       "loop": int(bool(schedule["loop"])),
                       "next": (-1 if schedule["next"] is None else
                                schedule_index[str(schedule["next"])]),
                       "steps": steps,
                       "branches": branches,
                       "speech": speech})
    return result


def runtime_scripts(script_paths: list[Path], entity_definitions: dict[str, dict[str, object]],
                    dialogue_index: dict[str, int], scenes: list[dict[str, object]],
                    music_index: dict[str, int], schedule_index: dict[str, int]) -> list[dict[str, object]]:
    """Compile the cooperative action subset required by the introductory handoff."""
    opcodes = {
        "set_control": 0,
        "set_entity_state": 1,
        "move_entity": 2,
        "wait": 3,
        "show_dialogue": 4,
        "change_scene": 5,
        "play_music": 6,
        "start_schedule": 7,
        "stop_schedule": 8,
        "set_entity_dialogue": 9,
    }
    facing_index = {"north": 0, "south": 1, "west": 2, "east": 3}
    entity_index = {entity_id: index for index, entity_id in enumerate(sorted(entity_definitions))}
    scene_index = {str(scene["id"]): index for index, scene in enumerate(scenes)}
    spawn_index = {
        str(scene["id"]): {str(spawn["id"]): index for index, spawn in enumerate(scene["spawns"])}
        for scene in scenes
    }
    scripts: list[dict[str, object]] = []
    for path in script_paths:
        document = expect(load_jsonc(path), dict, str(path))
        script_id = identifier(document.get("id"), f"{path} script id")
        actions: list[tuple[int, int, int, int, int]] = []
        for raw_action in expect(document.get("actions"), list, f"{path}.actions"):
            action = expect(raw_action, dict, f"{path} action")
            operation = identifier(action.get("op"), f"{path} action op")
            if operation not in opcodes:
                raise ContentError(f"{path}: action {operation!r} is validated but not yet supported at runtime")
            a = b = c = d = 0
            if operation == "set_control":
                a = int(bool(action["enabled"]))
            elif operation == "set_entity_state":
                entity_id = identifier(action.get("entity"), f"{path} entity")
                state = identifier(action.get("state"), f"{path} entity state")
                a = entity_index[entity_id]
                b = list(entity_definitions[entity_id]["states"]).index(state)
            elif operation == "move_entity":
                entity_id = identifier(action.get("entity"), f"{path} entity")
                a = entity_index[entity_id]
                b = integer(action.get("x"), f"{path} move x")
                c = integer(action.get("y"), f"{path} move y")
                d = facing_index[identifier(action.get("facing"), f"{path} move facing")]
            elif operation == "wait":
                a = integer(action.get("frames"), f"{path} wait frames")
            elif operation == "show_dialogue":
                a = dialogue_index[identifier(action.get("dialogue"), f"{path} dialogue")]
            elif operation == "change_scene":
                scene_id = identifier(action.get("scene"), f"{path} scene")
                spawn_id = identifier(action.get("spawn"), f"{path} spawn")
                a = scene_index[scene_id]
                b = spawn_index[scene_id][spawn_id]
                if not any(int(entity["player_controlled"]) for entity in scenes[a]["entities"]):
                    raise ContentError(
                        f"{path}: change_scene target {scene_id!r} has no player-controlled entity")
            elif operation == "play_music":
                a = music_index[identifier(action.get("track"), f"{path} music track")]
            elif operation == "start_schedule":
                a = entity_index[identifier(action.get("entity"), f"{path} entity")]
                b = schedule_index[identifier(action.get("schedule"), f"{path} schedule")]
                c = integer(action.get("delay_frames", 0), f"{path} schedule delay")
            elif operation == "stop_schedule":
                a = entity_index[identifier(action.get("entity"), f"{path} entity")]
            elif operation == "set_entity_dialogue":
                a = entity_index[identifier(action.get("entity"), f"{path} entity")]
                b = dialogue_index[identifier(action.get("dialogue"), f"{path} dialogue")]
            actions.append((opcodes[operation], a, b, c, d))
        scripts.append({"id": script_id, "actions": actions})
    scripts.sort(key=lambda item: str(item["id"]))
    return scripts


def runtime_scene_data(map_paths: list[Path], entity_definitions: dict[str, dict[str, object]], game_root: Path,
                       script_dialogues: dict[str, str], dialogue_index: dict[str, int],
                       script_index: dict[str, int]) -> tuple[list[dict[str, object]], list[dict[str, object]], list[dict[str, object]], list[str]]:
    """Compile the small initial scene runtime representation.

    This deliberately covers authored ground tiles, static depth-capable map
    objects, actor visual frames, collision rectangles, and placed entities.
    More map-layer capabilities are added only when a converted scene requires
    them.
    """
    raw_scenes: list[dict[str, object]] = []
    raw_visuals: dict[str, dict[str, object]] = {}
    raw_object_visuals: dict[str, dict[str, object]] = {}
    resources: set[str] = set()
    entity_index = {entity_id: index for index, entity_id in enumerate(sorted(entity_definitions))}
    move_index = {
        "stand_north": 0, "stand_south": 1, "stand_west": 2, "stand_east": 3,
        "walk_north": 0, "walk_south": 1, "walk_west": 2, "walk_east": 3,
    }

    for map_path in map_paths:
        document = expect(load_jsonc(map_path), dict, str(map_path))
        scene_properties = properties(document.get("properties"), str(map_path))
        scene_id = identifier(scene_properties.get("id"), f"{map_path} scene id")
        entry_dialogue = -1
        if "on_enter" in scene_properties:
            entry_script = identifier(scene_properties["on_enter"], f"{map_path} scene on_enter")
            if entry_script not in script_dialogues:
                raise ContentError(f"{map_path}: on_enter script {entry_script!r} must currently contain show_dialogue")
            entry_dialogue = dialogue_index[script_dialogues[entry_script]]
        width = integer(document.get("width"), f"{map_path}.width")
        height = integer(document.get("height"), f"{map_path}.height")
        tile_size = integer(document.get("tilewidth"), f"{map_path}.tilewidth")
        tiles = scene_tiles(map_path, document, game_root)
        terrain: list[dict[str, object]] = []
        collisions: list[dict[str, int]] = []
        entities: list[dict[str, int]] = []
        scenery: list[dict[str, int]] = []
        spawns: list[dict[str, object]] = []
        triggers: list[dict[str, int]] = []
        stateful_scenery_count = 0

        for layer in expect(document.get("layers"), list, f"{map_path}.layers"):
            layer = expect(layer, dict, f"{map_path} layer")
            if layer.get("type") == "tilelayer":
                data = expect(layer.get("data"), list, f"{map_path} tile layer data")
                if len(data) != width * height:
                    raise ContentError(f"{map_path}: finite tile layer must contain {width * height} cells")
                for index, gid in enumerate(data):
                    gid = integer(gid, f"{map_path} tile layer gid")
                    if gid == 0:
                        continue
                    tile = tiles.get(gid)
                    if tile is None:
                        raise ContentError(f"{map_path}: tile layer uses unknown tile gid {gid}")
                    image = str(tile["image"])
                    image_width = integer(tile["imagewidth"], f"{map_path} terrain imagewidth")
                    image_height = integer(tile["imageheight"], f"{map_path} terrain imageheight")
                    # Tiled aligns collection-of-images tiles to the bottom
                    # left of an orthogonal cell. This permits transparent,
                    # oversized ground decals on a second Tile Layer while
                    # retaining the ordinary terrain beneath them.
                    terrain.append({
                        "image": image,
                        "x": (index % width) * tile_size,
                        "y": (index // width) * tile_size + tile_size - image_height,
                    })
                    resources.add(image)
                continue
            if layer.get("type") != "objectgroup":
                continue
            for obj in expect(layer.get("objects", []), list, f"{map_path} object layer"):
                obj = expect(obj, dict, f"{map_path} map object")
                values = properties(obj.get("properties"), f"{map_path} object {obj.get('name', '?')}")
                kind = values.get("kind")
                if kind == "spawn":
                    spawn_x = integer(obj.get("x"), f"{map_path} spawn x")
                    spawn_y = integer(obj.get("y"), f"{map_path} spawn y")
                    if spawn_x % tile_size or spawn_y % tile_size:
                        raise ContentError(f"{map_path}: spawn points must align to the {tile_size}-pixel grid")
                    facing_name = identifier(values.get("facing", "south"), f"{map_path} spawn facing")
                    facing = {"north": 0, "south": 1, "west": 2, "east": 3}.get(facing_name)
                    if facing is None:
                        raise ContentError(f"{map_path}: unsupported spawn facing {facing_name!r}")
                    spawns.append({
                        "id": identifier(values.get("id"), f"{map_path} spawn id"),
                        "tile_x": spawn_x // tile_size,
                        "tile_y": spawn_y // tile_size,
                        "facing": facing,
                    })
                    continue
                if kind in {"exit", "trigger"}:
                    trigger_x = integer(obj.get("x"), f"{map_path} trigger x")
                    trigger_y = integer(obj.get("y"), f"{map_path} trigger y")
                    trigger_width = integer(obj.get("width"), f"{map_path} trigger width")
                    trigger_height = integer(obj.get("height"), f"{map_path} trigger height")
                    if (trigger_x % tile_size or trigger_y % tile_size or
                        trigger_width <= 0 or trigger_height <= 0 or
                        trigger_width % tile_size or trigger_height % tile_size):
                        raise ContentError(f"{map_path}: triggers must be positive {tile_size}-pixel-aligned rectangles")
                    when_name = identifier(values.get("when", "enter"), f"{map_path} trigger when")
                    when = {"enter": 0, "leave": 1}.get(when_name)
                    if when is None:
                        raise ContentError(f"{map_path}: unsupported trigger phase {when_name!r}")
                    facing_name = values.get("facing")
                    facing = -1
                    if facing_name is not None:
                        facing_name = identifier(facing_name, f"{map_path} trigger facing")
                        facing = {"north": 0, "south": 1, "west": 2, "east": 3}.get(facing_name, -2)
                        if facing == -2:
                            raise ContentError(f"{map_path}: unsupported trigger facing {facing_name!r}")
                    triggers.append({
                        "x": trigger_x // tile_size,
                        "y": trigger_y // tile_size,
                        "width": trigger_width // tile_size,
                        "height": trigger_height // tile_size,
                        "script": script_index[identifier(values.get("script"), f"{map_path} trigger script")],
                        "when": when,
                        "facing": facing,
                    })
                    continue
                if kind == "collision":
                    collisions.append({
                        "x": integer(obj.get("x"), f"{map_path} collision x"),
                        "y": integer(obj.get("y"), f"{map_path} collision y"),
                        "width": integer(obj.get("width"), f"{map_path} collision width"),
                        "height": integer(obj.get("height"), f"{map_path} collision height"),
                        "walkability": integer(values.get("walkability"), f"{map_path} collision walkability"),
                    })
                    continue
                if kind == "tiled_occluder":
                    object_id = identifier(values.get("id"), f"{map_path} tiled occluder id")
                    tile_gid = integer(values.get("tile_gid"), f"{map_path} tiled occluder tile_gid")
                    tile = tiles.get(tile_gid)
                    if tile is None:
                        raise ContentError(f"{map_path}: tiled occluder uses unknown tile gid {tile_gid}")
                    area_x = integer(obj.get("x"), f"{map_path} tiled occluder x")
                    area_y = integer(obj.get("y"), f"{map_path} tiled occluder y")
                    area_width = integer(obj.get("width"), f"{map_path} tiled occluder width")
                    area_height = integer(obj.get("height"), f"{map_path} tiled occluder height")
                    if (area_x % tile_size or area_y % tile_size or
                        area_width <= 0 or area_height <= 0 or
                        area_width % tile_size or area_height % tile_size):
                        raise ContentError(
                            f"{map_path}: tiled occluder {object_id!r} must be a positive "
                            f"{tile_size}-pixel-aligned rectangle"
                        )
                    image = str(tile["image"])
                    image_width = integer(tile["imagewidth"],
                                          f"{map_path} tiled occluder imagewidth")
                    image_height = integer(tile["imageheight"],
                                           f"{map_path} tiled occluder imageheight")
                    if image_width != tile_size or image_height != tile_size:
                        raise ContentError(
                            f"{map_path}: tiled occluder {object_id!r} image must be "
                            f"{tile_size}x{tile_size} pixels"
                        )
                    columns = area_width // tile_size
                    rows = area_height // tile_size
                    alpha = integer(values.get("alpha", 255),
                                    f"{map_path} tiled occluder alpha")
                    if alpha < 0 or alpha > 255:
                        raise ContentError(f"{map_path}: scenery alpha must be between 0 and 255")
                    for row in range(rows):
                        visual_y = area_y + row * tile_size
                        scenery.append({
                            "image": image,
                            "width": tile_size,
                            "height": tile_size,
                            "x": area_x,
                            "y": visual_y,
                            # Sort just after an actor whose footing is at the
                            # visible fringe's baseline. A source object's
                            # collision footprint may extend below its image;
                            # using it here makes fringe art cover scenery one
                            # row closer to the camera.
                            "depth": visual_y + image_height + 1,
                            "alpha": alpha,
                            "repeat_count": columns,
                            "repeat_key": f"{object_id}-{row:03d}",
                        })
                    resources.add(image)
                    continue
                if "gid" not in obj:
                    continue
                tile = tiles.get(integer(obj["gid"], f"{map_path} entity gid"))
                if tile is None:
                    raise ContentError(f"{map_path}: object uses unknown tile gid {obj['gid']}")
                tile_properties = expect(tile["properties"], dict, f"{map_path} entity tile properties")
                image = str(tile["image"])
                image_width = integer(tile["imagewidth"], f"{map_path} object imagewidth")
                image_height = integer(tile["imageheight"], f"{map_path} object imageheight")
                offset_x = integer(tile_properties.get("offset_x", 0), f"{map_path} object offset_x")
                offset_y = integer(tile_properties.get("offset_y", 0), f"{map_path} object offset_y")
                if kind != "entity":
                    alpha = integer(values.get("alpha", 255), f"{map_path} scenery alpha")
                    if alpha < 0 or alpha > 255:
                        raise ContentError(f"{map_path}: scenery alpha must be between 0 and 255")
                    # Tiled tile objects use x at the left edge and y at the
                    # image's baseline. The one-time importer has already
                    # folded source animation/frame offsets into those object
                    # coordinates, so applying the tileset's provenance
                    # offsets again would displace multi-part objects such as
                    # the wall vine. Preserve the authored Tiled position and
                    # compile its baseline as the stable runtime depth.
                    scenery.append({
                        "image": image,
                        "width": image_width,
                        "height": image_height,
                        "x": integer(obj.get("x"), f"{map_path} scenery x"),
                        "y": integer(obj.get("y"), f"{map_path} scenery y") - image_height,
                        "depth": integer(values.get("sort_y", obj.get("y")),
                                         f"{map_path} scenery sort_y"),
                        "alpha": alpha,
                    })
                    resources.add(image)
                    continue
                entity_def = identifier(values.get("entity_def"), f"{map_path} entity_def")
                interaction = values.get("interaction")
                interaction_dialogue = -1
                if interaction is not None:
                    interaction_id = identifier(interaction, f"{map_path} entity interaction")
                    if interaction_id in script_dialogues:
                        interaction_dialogue = dialogue_index[script_dialogues[interaction_id]]
                source_character = tile_properties.get("source_character")
                if not isinstance(source_character, str):
                    if entity_definitions[entity_def]["kind"] != "object":
                        raise ContentError(f"{map_path}: actor entity {entity_def!r} has no character visual")
                    alpha = integer(values.get("alpha", 255), f"{map_path} scenery alpha")
                    if alpha < 0 or alpha > 255:
                        raise ContentError(f"{map_path}: scenery alpha must be between 0 and 255")
                    source_object = tile_properties.get("source_object")
                    source_animation = tile_properties.get("source_animation", 0)
                    if not isinstance(source_object, str):
                        raise ContentError(f"{map_path}: object entity {entity_def!r} has no source_object")
                    object_id = identifier(values.get("id"), f"{map_path} object entity id")
                    state_names = list(entity_definitions[entity_def]["states"])
                    initial_state_name = identifier(
                        values.get("initial_state", state_names[0]),
                        f"{map_path} object entity {object_id} initial_state")
                    if initial_state_name not in state_names:
                        raise ContentError(
                            f"{map_path}: object entity {object_id!r} has unknown initial state "
                            f"{initial_state_name!r}")
                    visual_key = f"{source_object}#{source_animation}"
                    if visual_key not in raw_object_visuals:
                        frames: list[dict[str, object]] = []
                        candidates: list[tuple[int, dict[str, object]]] = []
                        for candidate in tiles.values():
                            candidate_properties = expect(candidate["properties"], dict,
                                                          f"{map_path} object tile properties")
                            if (candidate_properties.get("source_object") != source_object or
                                candidate_properties.get("source_animation", 0) != source_animation):
                                continue
                            image_index = candidate_properties.get("source_image")
                            if isinstance(image_index, int) and image_index >= 0:
                                candidates.append((image_index, candidate))
                        for image_index, candidate in sorted(candidates, key=lambda item: item[0]):
                            if image_index != len(frames):
                                raise ContentError(f"{map_path}: object visual {source_object!r} has a frame gap")
                            candidate_properties = expect(candidate["properties"], dict,
                                                          f"{map_path} object tile properties")
                            frame = {
                                "image": str(candidate["image"]),
                                "width": integer(candidate["imagewidth"], f"{map_path} object frame width"),
                                "height": integer(candidate["imageheight"], f"{map_path} object frame height"),
                                "offset_x": integer(candidate_properties.get("offset_x", 0), f"{map_path} object offset_x"),
                                "offset_y": integer(candidate_properties.get("offset_y", 0), f"{map_path} object offset_y"),
                            }
                            frames.append(frame)
                            resources.add(str(frame["image"]))
                        if not frames:
                            raise ContentError(f"{map_path}: object visual {source_object!r} has no frames")
                        state_count = len(entity_definitions[entity_def]["states"])
                        if len(frames) > state_count:
                            raise ContentError(
                                f"{map_path}: object visual {source_object!r} has more images than "
                                f"entity {entity_def!r} has states"
                            )
                        # Legacy state animations may use a zero-sized image to
                        # mean "draw nothing". Such images cannot be Tiled
                        # collection tiles, so missing trailing states compile
                        # to an explicit empty runtime frame.
                        frames.extend([None] * (state_count - len(frames)))
                        raw_object_visuals[visual_key] = {"frames": frames}
                    scenery.append({
                        "id": object_id,
                        "image": image,
                        "width": image_width,
                        "height": image_height,
                        "x": integer(obj.get("x"), f"{map_path} scenery x"),
                        "y": integer(obj.get("y"), f"{map_path} scenery y") - image_height,
                        "depth": integer(values.get("sort_y", obj.get("y")),
                                         f"{map_path} scenery sort_y"),
                        "alpha": alpha,
                        "definition": entity_index[entity_def],
                        "state_slot": stateful_scenery_count,
                        "initial_state": state_names.index(initial_state_name),
                        "states": state_names,
                        "visual_key": visual_key,
                    })
                    stateful_scenery_count += 1
                    continue
                if source_character not in raw_visuals:
                    raw_visuals[source_character] = {
                        "frames": [[None] * 4, [None] * 16],
                        "walk_count": [0, 0, 0, 0],
                        "base_x": integer(tile_properties.get("base_x"), f"{map_path} actor base_x"),
                        "base_y": integer(tile_properties.get("base_y"), f"{map_path} actor base_y"),
                    }
                visual = raw_visuals[source_character]
                # Gather all frames in this character collection, not just the
                # one used by the scene object.
                for candidate in tiles.values():
                    candidate_properties = expect(candidate["properties"], dict, f"{map_path} actor tile properties")
                    if candidate_properties.get("source_character") != source_character:
                        continue
                    move = candidate_properties.get("source_move")
                    image_index = candidate_properties.get("source_image")
                    if move not in move_index or not isinstance(image_index, int) or image_index < 0 or image_index >= 4:
                        continue
                    frame = {
                        "image": str(candidate["image"]),
                        "width": integer(candidate["imagewidth"], f"{map_path} actor frame width"),
                        "height": integer(candidate["imageheight"], f"{map_path} actor frame height"),
                        "offset_x": integer(candidate_properties.get("offset_x"), f"{map_path} actor offset_x"),
                        "offset_y": integer(candidate_properties.get("offset_y"), f"{map_path} actor offset_y"),
                    }
                    resources.add(frame["image"])
                    direction = move_index[move]
                    if move.startswith("stand_"):
                        visual["frames"][0][direction] = frame
                    else:
                        visual["frames"][1][direction * 4 + image_index] = frame
                        visual["walk_count"][direction] = max(visual["walk_count"][direction], image_index + 1)
                facing_name = values.get("facing", "south")
                facing = {"north": 0, "south": 1, "west": 2, "east": 3}.get(facing_name)
                if facing is None:
                    raise ContentError(f"{map_path}: unsupported entity facing {facing_name!r}")
                initially_present = values.get("initially_present", True)
                if not isinstance(initially_present, bool):
                    raise ContentError(f"{map_path}: entity initially_present must be a bool")
                entities.append({
                    "visual_key": source_character,
                    "tile_x": (integer(obj.get("x"), f"{map_path} entity x") - integer(tile_properties.get("offset_x"), f"{map_path} entity offset_x")) // tile_size + integer(tile_properties.get("base_x"), f"{map_path} entity base_x"),
                    "tile_y": (integer(obj.get("y"), f"{map_path} entity y") - image_height - integer(tile_properties.get("offset_y"), f"{map_path} entity offset_y")) // tile_size + integer(tile_properties.get("base_y"), f"{map_path} entity base_y"),
                    "facing": facing,
                    "blocks_movement": int(bool(entity_definitions[entity_def]["blocks_movement"])),
                    "player_controlled": int(bool(entity_definitions[entity_def]["player_controlled"])),
                    "interaction_dialogue": interaction_dialogue,
                    "definition": entity_index[entity_def],
                    "initially_present": int(initially_present),
                })
        scenery.sort(key=lambda item: item["depth"])
        spawns.sort(key=lambda item: str(item["id"]))
        raw_scenes.append({"id": scene_id, "width": width, "height": height, "tile_size": tile_size,
                           "entry_dialogue": entry_dialogue, "terrain": terrain,
                           "collisions": collisions, "scenery": scenery, "entities": entities,
                           "spawns": spawns, "triggers": triggers,
                           "stateful_scenery_count": stateful_scenery_count})

    # Runtime indices must match the alphabetical IDs emitted in content_ids.h.
    # Keeping this independent of manifest order also makes reordering inputs a
    # no-op for scripts and schedules that reference scenes by stable name.
    raw_scenes.sort(key=lambda item: str(item["id"]))

    # Object state belongs to the world, not to the currently displayed scene.
    # Assign stable global slots so off-screen schedules can change scenery and
    # the result remains visible after a later scene transition.
    global_state_slot = 0
    for scene in raw_scenes:
        object_ids: set[str] = set()
        for item in scene["scenery"]:
            if int(item.get("state_slot", -1)) < 0:
                continue
            object_id = str(item["id"])
            if object_id in object_ids:
                raise ContentError(
                    f"scene {scene['id']!r} duplicates stateful object id {object_id!r}")
            object_ids.add(object_id)
            item["state_slot"] = global_state_slot
            global_state_slot += 1

    initial_actor_placements: dict[int, str] = {}
    for scene in raw_scenes:
        for entity in scene["entities"]:
            if entity["player_controlled"] or not entity["initially_present"]:
                continue
            definition = int(entity["definition"])
            if definition in initial_actor_placements:
                raise ContentError(
                    f"actor definition {definition} is initially present in both "
                    f"{initial_actor_placements[definition]!r} and {scene['id']!r}")
            initial_actor_placements[definition] = str(scene["id"])

    visuals = []
    for source_character in sorted(raw_visuals):
        visual = raw_visuals[source_character]
        for direction, frame in enumerate(visual["frames"][0]):
            if frame is None:
                raise ContentError(f"actor tileset {source_character!r} is missing standing direction {direction}")
        visuals.append({"source_character": source_character, **visual})
    visual_index = {str(visual["source_character"]): index for index, visual in enumerate(visuals)}
    for scene in raw_scenes:
        for entity in scene["entities"]:
            entity["visual"] = visual_index[str(entity.pop("visual_key"))]
    object_visuals = [{"visual_key": key, **raw_object_visuals[key]} for key in sorted(raw_object_visuals)]
    object_visual_index = {str(visual["visual_key"]): index for index, visual in enumerate(object_visuals)}
    for scene in raw_scenes:
        for item in scene["scenery"]:
            if "visual_key" in item:
                item["visual"] = object_visual_index[str(item.pop("visual_key"))]
    return raw_scenes, visuals, object_visuals, sorted(resources)


def runtime_header(flag_count: int, stateful_scenery_count: int, entity_count: int) -> str:
    return """/* Generated by tools/content_compiler/compile_content.py; do not edit. */
#ifndef WASTESEDGE_GENERATED_CONTENT_SCENE_DATA_H
#define WASTESEDGE_GENERATED_CONTENT_SCENE_DATA_H

struct generated_runtime_frame { int texture, source_x, source_y, width, height, offset_x, offset_y; };
struct generated_runtime_actor_visual {
    int base_x, base_y;
    struct generated_runtime_frame standing[4];
    struct generated_runtime_frame walking[4][4];
    int walking_count[4];
};
struct generated_runtime_collision { int x, y, width, height, walkability; };
struct generated_runtime_background { struct generated_runtime_frame frame; int x, y; };
struct generated_runtime_object_visual { const struct generated_runtime_frame *frames; int frame_count; };
struct generated_runtime_scenery { struct generated_runtime_frame frame; int x, y, depth, alpha, definition, state_slot, initial_state, visual; };
struct generated_runtime_entity { int visual, tile_x, tile_y, facing, blocks_movement, player_controlled, interaction_dialogue, definition, initially_present; };
struct generated_runtime_spawn { int tile_x, tile_y, facing; };
struct generated_runtime_trigger { int x, y, width, height, script, when, facing; };
struct generated_runtime_music { int sound, loop; float speed, volume; };
struct generated_runtime_script_action { int op, a, b, c, d; };
struct generated_runtime_script { const struct generated_runtime_script_action *actions; int action_count; };
struct generated_runtime_schedule_step { int op, scene, x, y, facing, delay_min, delay_max, target, value; };
struct generated_runtime_schedule {
    const struct generated_runtime_schedule_step *steps;
    const int *branches;
    const struct generated_runtime_text *speech;
    int step_count, branch_count, entity, loop, next_schedule;
    int speech_count, ambient_speech_count;
    int speech_delay_min, speech_delay_max, speech_duration;
};
struct generated_runtime_dialogue_action { int flag, value; };
struct generated_runtime_glyph { int texture, min_x, min_y, max_x, max_y, hotspot_x, advance; };
struct generated_runtime_text { const int *glyphs; int glyph_count; };
struct generated_runtime_dialogue_speaker {
    const char *name;
    struct generated_runtime_text name_text;
    struct generated_runtime_frame portrait;
    int is_player, is_narrator;
};
struct generated_runtime_dialogue_choice {
    const char *text;
    struct generated_runtime_text display;
    const struct generated_runtime_dialogue_action *actions;
    int action_count, next_node, condition_flag, condition_value;
};
struct generated_runtime_dialogue_node {
    const char *text;
    struct generated_runtime_text display;
    const struct generated_runtime_dialogue_choice *choices;
    int choice_count, next_node, speaker, end_script;
};
struct generated_runtime_dialogue_start { int node, flag, value; };
struct generated_runtime_dialogue {
    const struct generated_runtime_dialogue_speaker *speakers;
    int speaker_count;
    const struct generated_runtime_dialogue_node *nodes;
    int node_count;
    const struct generated_runtime_dialogue_start *starts;
    int start_count, end_script;
};
struct generated_runtime_scene {
    int width, height, tile_size, entry_dialogue;
    const struct generated_runtime_background *background; int background_count;
    const struct generated_runtime_collision *collisions; int collision_count;
    const struct generated_runtime_scenery *scenery; int scenery_count, stateful_scenery_count;
    const struct generated_runtime_entity *entities; int entity_count;
    const struct generated_runtime_spawn *spawns; int spawn_count;
    const struct generated_runtime_trigger *triggers; int trigger_count;
};
extern const struct generated_runtime_actor_visual generated_runtime_actor_visuals[];
extern const int generated_runtime_actor_visual_count;
extern const struct generated_runtime_object_visual generated_runtime_object_visuals[];
extern const int generated_runtime_object_visual_count;
extern const struct generated_runtime_script generated_runtime_scripts[];
extern const int generated_runtime_script_count;
extern const struct generated_runtime_schedule generated_runtime_schedules[];
extern const int generated_runtime_schedule_count;
extern const struct generated_runtime_music generated_runtime_music[];
extern const int generated_runtime_music_count;
extern const struct generated_runtime_scene generated_runtime_scenes[];
extern const int generated_runtime_scene_count;
extern const int generated_runtime_initial_scene;
extern const struct generated_runtime_dialogue generated_runtime_dialogues[];
extern const int generated_runtime_dialogue_count;
extern const struct generated_runtime_glyph generated_runtime_glyphs[];
extern const int generated_runtime_glyph_count;
extern const int generated_runtime_space_advance;
#define GENERATED_RUNTIME_FLAG_COUNT %d
#define GENERATED_RUNTIME_FLAG_STORAGE_COUNT (GENERATED_RUNTIME_FLAG_COUNT > 0 ? GENERATED_RUNTIME_FLAG_COUNT : 1)
#define GENERATED_RUNTIME_STATEFUL_SCENERY_COUNT %d
#define GENERATED_RUNTIME_STATEFUL_STORAGE_COUNT (GENERATED_RUNTIME_STATEFUL_SCENERY_COUNT > 0 ? GENERATED_RUNTIME_STATEFUL_SCENERY_COUNT : 1)
#define GENERATED_RUNTIME_ENTITY_COUNT %d
#define GENERATED_RUNTIME_ENTITY_STORAGE_COUNT (GENERATED_RUNTIME_ENTITY_COUNT > 0 ? GENERATED_RUNTIME_ENTITY_COUNT : 1)
#define GENERATED_SCRIPT_SET_CONTROL 0
#define GENERATED_SCRIPT_SET_ENTITY_STATE 1
#define GENERATED_SCRIPT_MOVE_ENTITY 2
#define GENERATED_SCRIPT_WAIT 3
#define GENERATED_SCRIPT_SHOW_DIALOGUE 4
#define GENERATED_SCRIPT_CHANGE_SCENE 5
#define GENERATED_SCRIPT_PLAY_MUSIC 6
#define GENERATED_SCRIPT_START_SCHEDULE 7
#define GENERATED_SCRIPT_STOP_SCHEDULE 8
#define GENERATED_SCRIPT_SET_ENTITY_DIALOGUE 9
#define GENERATED_SCHEDULE_MOVE 0
#define GENERATED_SCHEDULE_PLACE 1
#define GENERATED_SCHEDULE_WAIT 2
#define GENERATED_SCHEDULE_SET_OBJECT_STATE 3
#define GENERATED_SCHEDULE_TOGGLE_OBJECT_STATE 4
#define GENERATED_SCHEDULE_SPEAK 5
#define GENERATED_SCHEDULE_JUMP 6
#define GENERATED_SCHEDULE_RANDOM_JUMP 7
#define GENERATED_TRIGGER_ENTER 0
#define GENERATED_TRIGGER_LEAVE 1
#endif
""" % (flag_count, stateful_scenery_count, entity_count)


def frame_initializer(frame: object, resource_index: dict[str, int]) -> str:
    if frame is None:
        return "{ -1, 0, 0, 0, 0, 0, 0 }"
    frame = expect(frame, dict, "runtime frame")
    return "{ %d, %d, %d, %d, %d, %d, %d }" % (
        resource_index[str(frame["image"])], integer(frame.get("source_x", 0), "runtime frame source_x"),
        integer(frame.get("source_y", 0), "runtime frame source_y"),
        integer(frame["width"], "runtime frame width"),
        integer(frame["height"], "runtime frame height"), integer(frame["offset_x"], "runtime frame offset_x"),
        integer(frame["offset_y"], "runtime frame offset_y"),
    )


def runtime_source(scenes: list[dict[str, object]], visuals: list[dict[str, object]],
                   object_visuals: list[dict[str, object]], resources: list[str], initial_scene: str,
                   dialogues: list[dict[str, object]], scripts: list[dict[str, object]],
                   glyphs: list[dict[str, object]], space_advance: int,
                   music: list[dict[str, object]], schedules: list[dict[str, object]]) -> str:
    resource_index = {path: index for index, path in enumerate(resources)}
    lines = ["/* Generated by tools/content_compiler/compile_content.py; do not edit. */", '#include "content_scene_data.h"', ""]
    lines.append("const struct generated_runtime_music generated_runtime_music[] = {")
    for sound, track in enumerate(music):
        lines.append("    { %d, %d, %.8gf, %.8gf }," % (
            sound, track["loop"], track["speed"], track["volume"]))
    lines.append("};")
    lines.append(f"const int generated_runtime_music_count = {len(music)};\n")
    for index, scene in enumerate(scenes):
        lines.append(f"static const struct generated_runtime_background generated_runtime_background_{index}[] = {{")
        for item in scene["background"]:
            frame = {"image": item["image"], "width": item["width"], "height": item["height"], "offset_x": 0, "offset_y": 0}
            lines.append("    { %s, %d, %d }," % (frame_initializer(frame, resource_index), integer(item["x"], "runtime background x"), integer(item["y"], "runtime background y")))
        lines.append("};")
        lines.append(f"static const struct generated_runtime_collision generated_runtime_collisions_{index}[] = {{")
        for collision in scene["collisions"]:
            lines.append("    { %d, %d, %d, %d, %d }," % (collision["x"], collision["y"], collision["width"], collision["height"], collision["walkability"]))
        lines.append("};")
        lines.append(f"static const struct generated_runtime_scenery generated_runtime_scenery_{index}[] = {{")
        for item in scene["scenery"]:
            frame = {"image": item["image"], "width": item["width"], "height": item["height"], "offset_x": 0, "offset_y": 0}
            lines.append("    { %s, %d, %d, %d, %d, %d, %d, %d, %d }," % (
                frame_initializer(frame, resource_index), integer(item["x"], "runtime scenery x"),
                integer(item["y"], "runtime scenery y"), integer(item["depth"], "runtime scenery depth"),
                integer(item.get("alpha", 255), "runtime scenery alpha"),
                integer(item.get("definition", -1), "runtime scenery definition"),
                integer(item.get("state_slot", -1), "runtime scenery state slot"),
                integer(item.get("initial_state", 0), "runtime scenery initial state"),
                integer(item.get("visual", -1), "runtime scenery visual")))
        lines.append("};")
        lines.append(f"static const struct generated_runtime_entity generated_runtime_entities_{index}[] = {{")
        for entity in scene["entities"]:
            lines.append("    { %d, %d, %d, %d, %d, %d, %d, %d, %d }," % (
                entity["visual"], entity["tile_x"], entity["tile_y"], entity["facing"],
                entity["blocks_movement"], entity["player_controlled"],
                entity["interaction_dialogue"], entity["definition"], entity["initially_present"]))
        lines.append("};\n")
        lines.append(f"static const struct generated_runtime_spawn generated_runtime_spawns_{index}[] = {{")
        for spawn in scene["spawns"]:
            lines.append("    { %d, %d, %d }," % (
                spawn["tile_x"], spawn["tile_y"], spawn["facing"]))
        lines.append("};")
        lines.append(f"static const struct generated_runtime_trigger generated_runtime_triggers_{index}[] = {{")
        for trigger in scene["triggers"]:
            lines.append("    { %d, %d, %d, %d, %d, %d, %d }," % (
                trigger["x"], trigger["y"], trigger["width"], trigger["height"],
                trigger["script"], trigger["when"], trigger["facing"]))
        lines.append("};\n")
    lines.append("const struct generated_runtime_actor_visual generated_runtime_actor_visuals[] = {")
    for visual in visuals:
        lines.append(f"    {{ {visual['base_x']}, {visual['base_y']},")
        lines.append("      { " + ", ".join(frame_initializer(frame, resource_index) for frame in visual["frames"][0]) + " },")
        lines.append("      {")
        for direction in range(4):
            frames = visual["frames"][1][direction * 4:direction * 4 + 4]
            lines.append("        { " + ", ".join(frame_initializer(frame, resource_index) for frame in frames) + " },")
        lines.append("      },")
        lines.append("      { " + ", ".join(str(value) for value in visual["walk_count"]) + " } },")
    lines.append("};")
    lines.append(f"const int generated_runtime_actor_visual_count = {len(visuals)};\n")
    for index, visual in enumerate(object_visuals):
        lines.append(f"static const struct generated_runtime_frame generated_runtime_object_frames_{index}[] = {{")
        lines.extend("    %s," % frame_initializer(frame, resource_index) for frame in visual["frames"])
        lines.append("};")
    lines.append("const struct generated_runtime_object_visual generated_runtime_object_visuals[] = {")
    for index, visual in enumerate(object_visuals):
        lines.append(f"    {{ generated_runtime_object_frames_{index}, {len(visual['frames'])} }},")
    lines.append("};")
    lines.append(f"const int generated_runtime_object_visual_count = {len(object_visuals)};\n")
    for index, script in enumerate(scripts):
        lines.append(f"static const struct generated_runtime_script_action generated_runtime_script_actions_{index}[] = {{")
        lines.extend("    { %d, %d, %d, %d, %d }," % action for action in script["actions"])
        lines.append("};")
    lines.append("const struct generated_runtime_script generated_runtime_scripts[] = {")
    for index, script in enumerate(scripts):
        lines.append(f"    {{ generated_runtime_script_actions_{index}, {len(script['actions'])} }},")
    lines.append("};")
    lines.append(f"const int generated_runtime_script_count = {len(scripts)};\n")
    for index, schedule in enumerate(schedules):
        lines.append(f"static const struct generated_runtime_schedule_step generated_runtime_schedule_steps_{index}[] = {{")
        lines.extend("    { %d, %d, %d, %d, %d, %d, %d, %d, %d }," % step
                     for step in schedule["steps"])
        lines.append("};")
        if schedule["branches"]:
            lines.append(f"static const int generated_runtime_schedule_branches_{index}[] = {{ " +
                         ", ".join(str(branch) for branch in schedule["branches"]) + " };")
        speech = schedule.get("speech")
        if speech is not None:
            for line_index, line_glyphs in enumerate(speech["line_glyphs"]):
                lines.append(
                    f"static const int generated_runtime_schedule_speech_glyphs_{index}_{line_index}[] = {{ " +
                    ", ".join(str(glyph) for glyph in line_glyphs) + " };")
            lines.append(f"static const struct generated_runtime_text generated_runtime_schedule_speech_{index}[] = {{")
            for line_index, line_glyphs in enumerate(speech["line_glyphs"]):
                lines.append(
                    f"    {{ generated_runtime_schedule_speech_glyphs_{index}_{line_index}, {len(line_glyphs)} }},")
            lines.append("};")
    lines.append("const struct generated_runtime_schedule generated_runtime_schedules[] = {")
    for index, schedule in enumerate(schedules):
        speech = schedule.get("speech")
        branches_name = (f"generated_runtime_schedule_branches_{index}"
                         if schedule["branches"] else "0")
        speech_name = f"generated_runtime_schedule_speech_{index}" if speech is not None else "0"
        speech_count = len(speech["lines"]) if speech is not None else 0
        ambient_count = speech["ambient_count"] if speech is not None else 0
        delay_min = speech["delay_min"] if speech is not None else 0
        delay_max = speech["delay_max"] if speech is not None else 0
        duration = speech["duration"] if speech is not None else 0
        lines.append(
            f"    {{ generated_runtime_schedule_steps_{index}, {branches_name}, {speech_name}, "
            f"{len(schedule['steps'])}, {len(schedule['branches'])}, "
            f"{schedule['entity']}, {schedule['loop']}, {schedule['next']}, "
            f"{speech_count}, {ambient_count}, {delay_min}, {delay_max}, {duration} }},")
    lines.append("};")
    lines.append(f"const int generated_runtime_schedule_count = {len(schedules)};\n")
    lines.append("const struct generated_runtime_glyph generated_runtime_glyphs[] = {")
    for glyph in glyphs:
        lines.append("    { %d, %d, %d, %d, %d, %d, %d }," % (
            resource_index[str(glyph["image"])], glyph["x"], glyph["y"],
            glyph["x"] + glyph["width"] - 1, glyph["y"] + glyph["height"] - 1,
            glyph["x"] + glyph["origin_x"],
            glyph["advance"],
        ))
    lines.append("};")
    lines.append(f"const int generated_runtime_glyph_count = {len(glyphs)};")
    lines.append(f"const int generated_runtime_space_advance = {space_advance};\n")
    for dialogue_index, dialogue in enumerate(dialogues):
        lines.append(f"static const struct generated_runtime_dialogue_start generated_runtime_dialogue_starts_{dialogue_index}[] = {{")
        for start in dialogue["starts"]:
            flag = -1 if start["flag"] is None else start["flag"]
            lines.append("    { %d, %d, %d }," % (start["node"], flag, start["value"]))
        lines.append("};")
        for speaker_index, speaker in enumerate(dialogue["speakers"]):
            lines.append(f"static const int generated_runtime_dialogue_speaker_glyphs_{dialogue_index}_{speaker_index}[] = {{ " +
                         ", ".join(str(glyph) for glyph in speaker["name_glyphs"]) + " };")
        lines.append(f"static const struct generated_runtime_dialogue_speaker generated_runtime_dialogue_speakers_{dialogue_index}[] = {{")
        for speaker_index, speaker in enumerate(dialogue["speakers"]):
            portrait = None
            if speaker["portrait"] is not None:
                portrait = {
                    "image": speaker["portrait"],
                    "width": speaker["portrait_width"],
                    "height": speaker["portrait_height"],
                    "offset_x": 0,
                    "offset_y": 0,
                }
            lines.append("    { %s, { generated_runtime_dialogue_speaker_glyphs_%d_%d, %d }, %s, %d, %d }," % (
                json.dumps(speaker["name"]), dialogue_index, speaker_index, len(speaker["name_glyphs"]),
                frame_initializer(portrait, resource_index), int(speaker["role"] == "player"),
                int(speaker["role"] == "narrator"),
            ))
        lines.append("};")
        for node_index, node in enumerate(dialogue["nodes"]):
            lines.append(f"static const int generated_runtime_dialogue_node_glyphs_{dialogue_index}_{node_index}[] = {{ " +
                         ", ".join(str(glyph) for glyph in node["text_glyphs"]) + " };")
            for choice_index, choice in enumerate(node["choices"]):
                lines.append(f"static const int generated_runtime_dialogue_choice_glyphs_{dialogue_index}_{node_index}_{choice_index}[] = {{ " +
                             ", ".join(str(glyph) for glyph in choice["text_glyphs"]) + " };")
                actions = choice["actions"]
                if not actions:
                    continue
                lines.append(f"static const struct generated_runtime_dialogue_action generated_runtime_dialogue_actions_{dialogue_index}_{node_index}_{choice_index}[] = {{")
                lines.extend("    { %d, %d }," % (action["flag"], action["value"]) for action in actions)
                lines.append("};")
            if node["choices"]:
                lines.append(f"static const struct generated_runtime_dialogue_choice generated_runtime_dialogue_choices_{dialogue_index}_{node_index}[] = {{")
                for choice_index, choice in enumerate(node["choices"]):
                    actions = choice["actions"]
                    action_name = (f"generated_runtime_dialogue_actions_{dialogue_index}_{node_index}_{choice_index}"
                                   if actions else "0")
                    condition_flag = (-1 if choice["condition_flag"] is None
                                      else choice["condition_flag"])
                    lines.append("    { %s, { generated_runtime_dialogue_choice_glyphs_%d_%d_%d, %d }, %s, %d, %d, %d, %d }," % (
                        json.dumps(choice["text"]), dialogue_index, node_index, choice_index,
                        len(choice["text_glyphs"]), action_name, len(actions), choice["next"],
                        condition_flag, choice["condition_value"]))
                lines.append("};")
        lines.append(f"static const struct generated_runtime_dialogue_node generated_runtime_dialogue_nodes_{dialogue_index}[] = {{")
        for node_index, node in enumerate(dialogue["nodes"]):
            choices_name = (f"generated_runtime_dialogue_choices_{dialogue_index}_{node_index}"
                            if node["choices"] else "0")
            lines.append("    { %s, { generated_runtime_dialogue_node_glyphs_%d_%d, %d }, %s, %d, %d, %d, %d }," % (
                json.dumps(node["text"]), dialogue_index, node_index, len(node["text_glyphs"]),
                choices_name, len(node["choices"]), node["next"], node["speaker"],
                node["on_end"]))
        lines.append("};")
    lines.append("const struct generated_runtime_dialogue generated_runtime_dialogues[] = {")
    for index, dialogue in enumerate(dialogues):
        lines.append(f"    {{ generated_runtime_dialogue_speakers_{index}, {len(dialogue['speakers'])}, generated_runtime_dialogue_nodes_{index}, {len(dialogue['nodes'])}, generated_runtime_dialogue_starts_{index}, {len(dialogue['starts'])}, {dialogue['on_end']} }},")
    lines.append("};")
    lines.append(f"const int generated_runtime_dialogue_count = {len(dialogues)};\n")
    lines.append("const struct generated_runtime_scene generated_runtime_scenes[] = {")
    for index, scene in enumerate(scenes):
        lines.append("    { %d, %d, %d, %d, generated_runtime_background_%d, %d, generated_runtime_collisions_%d, %d, generated_runtime_scenery_%d, %d, %d, generated_runtime_entities_%d, %d, generated_runtime_spawns_%d, %d, generated_runtime_triggers_%d, %d }," % (
            scene["width"], scene["height"], scene["tile_size"], scene["entry_dialogue"],
            index, len(scene["background"]), index, len(scene["collisions"]),
            index, len(scene["scenery"]), scene["stateful_scenery_count"],
            index, len(scene["entities"]), index, len(scene["spawns"]),
            index, len(scene["triggers"])))
    lines.append("};")
    lines.append(f"const int generated_runtime_scene_count = {len(scenes)};")
    lines.append(f"const int generated_runtime_initial_scene = {[scene['id'] for scene in scenes].index(initial_scene)};")
    lines.append("")
    return "\n".join(lines)


def compile_content(project_root: Path) -> None:
    game_root = project_root / "gamedata"
    manifest = expect(load_jsonc(game_root / "manifest.jsonc"), dict, "manifest")
    if manifest.get("format") != "wastesedge-content-v1":
        raise ContentError("manifest format must be 'wastesedge-content-v1'")
    inputs = {category: [relative_input(game_root, item, category) for item in expect(manifest.get(category), list, f"manifest {category}")]
              for category in ("maps", "entities", "audio", "dialogue", "scripts", "schedules")}
    entities = collect_entities(inputs["entities"])
    schedules = collect_schedules(inputs["schedules"], entities)
    music = collect_audio(inputs["audio"], game_root)
    music_ids = {str(track["id"]) for track in music}
    dialogue = collect_dialogue(inputs["dialogue"], entities, game_root)
    scripts = collect_scripts(inputs["scripts"], entities, dialogue, music_ids, schedules)
    for path in inputs["dialogue"]:
        document = expect(load_jsonc(path), dict, str(path))
        if "on_end" in document:
            on_end = identifier(document["on_end"], f"{path} on_end")
            if on_end not in scripts:
                raise ContentError(f"{path}: unknown on_end script {on_end!r}")
        for raw_node in expect(document.get("nodes"), list, f"{path}.nodes"):
            node = expect(raw_node, dict, f"{path}.nodes entry")
            if "on_end" not in node:
                continue
            node_id = identifier(node.get("id"), f"{path} node id")
            on_end = identifier(node["on_end"], f"{path} node {node_id} on_end")
            if on_end not in scripts:
                raise ContentError(f"{path}: node {node_id!r} has unknown on_end script {on_end!r}")
    scenes, tilesets = collect_scenes(inputs["maps"], entities, scripts, game_root)
    initial_scene = identifier(manifest.get("initial_scene"), "manifest initial_scene")
    if initial_scene not in scenes:
        raise ContentError(f"manifest initial_scene refers to unknown scene {initial_scene!r}")
    script_index = {script_id: index for index, script_id in enumerate(sorted(scripts))}
    runtime_dialogue, script_dialogues, runtime_flags = runtime_dialogues(
        inputs["dialogue"], inputs["scripts"], entities, game_root, script_index)
    dialogue_index = {str(item["id"]): index for index, item in enumerate(runtime_dialogue)}
    runtime_scenes, runtime_visuals, runtime_object_visuals, _ = runtime_scene_data(
        inputs["maps"], entities, game_root, script_dialogues, dialogue_index, script_index)
    runtime_schedule_data = runtime_schedules(schedules, entities, runtime_scenes)
    schedule_index = {str(item["id"]): index for index, item in enumerate(runtime_schedule_data)}
    music_index = {str(track["id"]): index for index, track in enumerate(music)}
    runtime_script_data = runtime_scripts(
        inputs["scripts"], entities, dialogue_index, runtime_scenes, music_index, schedule_index)
    ordered = {"scenes": sorted(scenes), "tilesets": sorted(tilesets),
               "entities": sorted(entities), "music": sorted(music_ids),
               "dialogue": sorted(dialogue), "scripts": sorted(scripts),
               "schedules": sorted(schedules)}
    canonical = json.dumps(ordered, separators=(",", ":"), sort_keys=True)
    digest = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    output_root = project_root / "generated"
    materialize_audio(music, game_root, output_root)
    materialize_background_chunks(runtime_scenes, game_root, output_root)
    materialize_scenery_strips(runtime_scenes, game_root, output_root)
    materialize_character_sheets(runtime_visuals, game_root, output_root)
    runtime_glyphs, runtime_space_advance = materialize_dialogue_glyph_atlas(
        runtime_dialogue, runtime_schedule_data, game_root, output_root)
    runtime_resources = collect_runtime_resources(runtime_scenes, runtime_visuals,
                                                  runtime_object_visuals,
                                                  runtime_dialogue, runtime_glyphs)
    maximum_cartridge_textures = 256
    if len(runtime_resources) > maximum_cartridge_textures:
        raise ContentError(
            f"compiled content needs {len(runtime_resources)} textures, but Vircon32 "
            f"cartridges support at most {maximum_cartridge_textures}; combine related "
            "images into atlases or remove unused visuals"
        )
    manifest_output = json.dumps({"format": "wastesedge-generated-v1", "signature": digest, **ordered}, indent=2, sort_keys=True) + "\n"
    header_lines = ["/* Generated by tools/content_compiler/compile_content.py; do not edit. */", "#ifndef WASTESEDGE_GENERATED_CONTENT_IDS_H", "#define WASTESEDGE_GENERATED_CONTENT_IDS_H", ""]
    for category, prefix in (("scenes", "GENERATED_SCENE"), ("tilesets", "GENERATED_TILESET"),
                             ("entities", "GENERATED_ENTITY"), ("music", "GENERATED_MUSIC"),
                             ("dialogue", "GENERATED_DIALOGUE"), ("scripts", "GENERATED_SCRIPT"),
                             ("schedules", "GENERATED_SCHEDULE")):
        header_lines.append(f"enum {prefix.lower()}_id {{")
        header_lines.extend(f"    {enum_name(prefix, value)} = {index}," for index, value in enumerate(ordered[category]))
        header_lines.append(f"    {prefix}_COUNT = {len(ordered[category])}")
        header_lines.append("};\n")
    header_lines.extend(["extern const char generated_content_signature[];", "#endif", ""])
    c_lines = ["/* Generated by tools/content_compiler/compile_content.py; do not edit. */", "#include \"content_ids.h\"", "", f"const char generated_content_signature[] = \"{digest}\";", ""]
    for category in ("scenes", "tilesets", "entities", "music", "dialogue", "scripts", "schedules"):
        c_lines.append(f"const unsigned int generated_{category}_count = {len(ordered[category])};")
    c_lines.append("")
    write_output(output_root / "content_manifest.json", manifest_output)
    write_output(output_root / "content_ids.h", "\n".join(header_lines))
    write_output(output_root / "content_data.c", "\n".join(c_lines))
    stateful_scenery_count = sum(int(scene["stateful_scenery_count"])
                                 for scene in runtime_scenes)
    write_output(output_root / "content_scene_data.h", runtime_header(
        len(runtime_flags), stateful_scenery_count, len(entities)))
    write_output(output_root / "content_scene_data.c", runtime_source(
        runtime_scenes, runtime_visuals, runtime_object_visuals, runtime_resources,
        initial_scene, runtime_dialogue, runtime_script_data,
        runtime_glyphs, runtime_space_advance, music, runtime_schedule_data))
    source_textures = [path for path in runtime_resources if not path.startswith(GENERATED_RESOURCE_PREFIX)]
    generated_textures = [path.removeprefix(GENERATED_RESOURCE_PREFIX) for path in runtime_resources if path.startswith(GENERATED_RESOURCE_PREFIX)]
    cmake_lists = {
        "WASTESEDGE_CONTENT_TEXTURES": ["gamedata/" + path for path in source_textures],
        "WASTESEDGE_GENERATED_TEXTURES": ["generated/" + path for path in generated_textures],
        "WASTESEDGE_GENERATED_SOUNDS": [str(track["generated"]) for track in music],
    }
    cmake_lines = ["# Generated by tools/content_compiler/compile_content.py; do not edit."]
    for variable, paths in cmake_lists.items():
        cmake_lines.append(f"set({variable}")
        for path in paths:
            escaped = path.replace("\\", "\\\\").replace('"', '\\"')
            cmake_lines.append(f'  "{escaped}"')
        cmake_lines.append(")")
    cmake_lines.append("")
    write_output(output_root / "content_assets.cmake", "\n".join(cmake_lines))
    print(f"content compiler: {len(scenes)} scenes, {len(tilesets)} tilesets, "
          f"{len(entities)} entities, {len(music)} music, {len(dialogue)} dialogue, "
          f"{len(scripts)} scripts, {len(schedules)} schedules, {len(runtime_resources)}/"
          f"{maximum_cartridge_textures} textures")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        compile_content(arguments.project_root.resolve())
    except ContentError as error:
        print(f"content compiler: error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
