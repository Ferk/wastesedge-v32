# V32 Adonthell - Wastes Edge

This is a port to [Vircon32](https://www.vircon32.com/) of the RPG investigation
story from [*Adonthell - Waste's Edge*](https://adonthell.nongnu.org/).

This is not an attempt to make a fully compatible Adonthell engine recreation.
It's a completely new engine that attempts to reproduce the same story, dialogue
and mechanics following a data-driven approach. The build will compile the game data
into a representation that can be run in the console without the need for the
python runtime that the original used.


## Building

```sh
cmake -S . -B build
cmake --build build
```

The result is `build/WastesEdge.v32`. CMake directly owns every build stage;
there is no secondary Makefile or shell build wrapper.

By default CMake finds `wasm2vircon` and the Vircon32 development tools on
`PATH`. Override its location when needed with `WASM2VIRCON_EXECUTABLE` cmake
setting.

The build first compiles editable Tiled/JSONC content and source assets from
`gamedata/` into reproducible files under `generated/`.

## Controls

- D-pad: move, or select a menu/dialogue choice.
- A or X: interact and confirm.
- B or Y: cancel a save/load slot screen.
- Start: pause during play; close the pause or slot menu.
