# Editable game content

Everything in this directory is a normal build input. Maps are edited in Tiled;
dialogue, scripts, entities, schedules, and the manifest are JSONC. The CMake
build compiles these inputs into `generated/`; do not hand-edit generated
output.

Open `WastesEdge.tiled-project` in Tiled to load this directory as the project.
The project indexes `maps/`, `tilesets/`, and `assets/`; those latter folders
will become visible as their first converted resources are added.

`maps/intro_exterior.tmj` and `tilesets/intro_exterior.tsj` remain small
contract-testing fixtures. `maps/redwyne_exterior.tmj` is the first converted
scene: `background_terrain` is a Tile Layer for static stone/grass/road
surfaces, while `map_objects` retains depth-capable art. It also has an editable
directional collision layer and separate terrain/object tilesets. The converted
Redwyne inn and parlour, portrait dialogue UI, and reciprocal room transitions
are also present. The exterior includes Player and Talan as `entity`
objects in `map_objects`, so Tiled's top-down ordering can sort them with the
depth-capable scenery. Their collection tilesets store all standing and walking
frames. NPC routes are authored separately in `schedules/`; scene-local actor
objects provide the visual/interaction template for every scene a route visits.
All NPCs whose default positions belong to the converted exterior and common
room are present: Talan, Tristan, Orloth, Alek, Erek, Oliver, and Rhayne. Their
interaction dialogue is compiled from JSONC. All seven have data-authored
schedules and autonomous floating remarks. Talan, Tristan, Orloth, Alek, and
Rhayne move along converted routes; Erek now crosses between the common room
and parlour. Oliver remains in the yard until a script summons him.
