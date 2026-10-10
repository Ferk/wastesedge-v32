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
Redwyne inn, parlour, first-floor landing, and player's bedroom, portrait
dialogue UI, and room transitions are also present. Their translucent front
walls, door frames, and windows use
the per-object integer `alpha` property (`0` transparent, `255` opaque). The
exterior includes Player and Talan as `entity`
objects in `map_objects`, so Tiled's top-down ordering can sort them with the
depth-capable scenery. Their collection tilesets store all standing and walking
frames. NPC routes are authored separately in `schedules/`; scene-local actor
objects provide the visual/interaction template for every scene a route visits.
All NPCs whose default positions belong to the converted exterior, common
room, and landing are present: Talan, Tristan, Orloth, Alek, Erek, Oliver,
Rhayne, and Jelom. Their interaction dialogue is compiled from JSONC when
available. All eight have data-authored schedules and autonomous floating
remarks. Talan, Tristan, Orloth, Alek, Jelom, and Rhayne move along converted
routes; Erek now crosses between the common room and parlour. Orloth's room
offer summons Oliver, switches both characters to the player's bedroom, and
starts Oliver's room-specific conversation. On leaving, Oliver follows a
finite route through the landing and common room; that route then chains back
to his ordinary yard schedule. Orloth's common-room route also demonstrates
timed schedule events: his clock remark occurs only at the clock, and stable-ID
stateful mug objects change in two stages at each table even when the inn is
off screen. Labeled schedule branches select Orloth's next job, Erek's room
position, and Rhayne's next safe yard point at runtime using engine randomness
seeded by Vircon32's hardware generator.
