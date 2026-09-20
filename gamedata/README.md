# Editable game content

Everything in this directory is a normal build input. Maps are edited in Tiled;
dialogue, scripts, entities, and the manifest are JSONC. `make` compiles these
inputs into `generated/`; do not hand-edit generated output.

Open `WastesEdge.tiled-project` in Tiled to load this directory as the project.
The project indexes `maps/`, `tilesets/`, and `assets/`; those latter folders
will become visible as their first converted resources are added.

`maps/intro_exterior.tmj` and `tilesets/intro_exterior.tsj` remain small
contract-testing fixtures. `maps/redwyne_exterior.tmj` is the first converted
scene: `background_terrain` is a Tile Layer for static stone/grass/road
surfaces, while `map_objects` retains depth-capable art. It also has an editable
directional collision layer and separate terrain/object tilesets. Interior art,
UI, and remaining scenes are still pending conversion. The exterior includes
Player and Talan as `entity`
objects in `map_objects`, so Tiled's top-down ordering can sort them with the
depth-capable scenery. Their collection tileset stores all standing and walking
frames; the runtime animation player is the next required capability.
