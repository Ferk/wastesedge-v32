# Game data

This directory contains all editable Waste's Edge content. It is compiled at
build time into the compact resources used by the Vircon32 ROM; never edit
`generated/` directly.

- `maps/` and `tilesets/` are edited in Tiled. Open `WastesEdge.tiled-project`
  from this directory.
- `entities/`, `dialogue/`, `scripts/`, `schedules/`, and `audio/` are JSONC
  game data.
- `assets/world/` contains scenery, props, and effects; `assets/actors/`
  contains character frames.
- `manifest.jsonc` lists the content compiled into the game.
