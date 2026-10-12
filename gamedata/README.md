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
Redwyne inn, kitchen, parlour, first-floor landing, and player's bedroom, portrait
dialogue UI, and room transitions are also present. Their translucent front
walls, door frames, and windows use
the per-object integer `alpha` property (`0` transparent, `255` opaque). The
kitchen's terracotta floor and doorway transition are authored on its
`background_details` Tile Layer, keeping them out of actor depth sorting. The
exterior includes Player and Talan as `entity`
objects in `map_objects`, so Tiled's top-down ordering can sort them with the
depth-capable scenery. Their collection tilesets store all standing and walking
frames. NPC routes are authored separately in `schedules/`; scene-local actor
objects provide the visual/interaction template for every scene a route visits.
All NPCs whose default positions belong to the converted exterior, common
room, landing, and kitchen are present: Talan, Tristan, Orloth, Alek, Erek,
Oliver, Rhayne, Jelom, and Lucia. Their interaction dialogue is compiled from JSONC when
available; Jelom's currently reachable guard-at-the-door conversation is
authored in `dialogue/jelom_start.jsonc`, and its Oliver/Alek evidence choices
are unlocked by flags written in those characters' dialogue data. Once Oliver
has made Jelom doubt Talan and Talan admits leaving his post, the original
compound rebuttal becomes available and sets the persistent Silverhair-release
state. That state selects the guarded first-floor doorway's room transition;
before it is set, the same tile automatically opens Jelom's warning. The
converted room contains Silverhair, Sarin, and Janesta at their reference
anchors, with Silverhair's initial and repeat dialogue. Silverhair alternates
between her place and the window, Janesta randomly visits her four original
work points, and Sarin paces the room before randomly changing direction;
their source remarks and timing ranges are schedule data. The room's original
directional examine event is an `interactions` hotspot at `(1,6)`, and later
questions about the Yeti, the night noise, and the argument are unlocked by
facts written by Frostbloom's and Erek's dialogue. The
Orloth-to-Alek-to-Erek packed-gems lead likewise uses one staged authored flag,
so Jelom only offers the accusation before Banec checks it with Erek. Rhayne's
account of a noise also unlocks Lucia's confidence about Talan's singing; after
Talan admits leaving his post, the evidence can be reported to Jelom. All twelve have data-authored schedules and autonomous floating
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

The cellar is reachable by both original inn routes: the kitchen stair and
the separate common-room stair. Its pantry and Fingolson-room passages are
reciprocal two-cell thresholds. Erek's conditional dialogue performs the
original escort into Fingolson's room; Bjarn then follows his room schedule,
offers the converted investigation branches, and guards the Dwarven chest
with its original examination remark. The bath, Alek-room, and barn cellar
thresholds remain inactive until those destination scenes are converted.

Schedule `move` targets are direct destinations: the runtime follows the
original engine's cardinal mapsquare A* rules and replans if another character
temporarily occupies the next square. Add waypoints only for authored stops or
scene boundaries, not merely to steer around collision.

Map examination points live in an `interactions` object layer as `hotspot`
objects. Their rectangles mark player activation squares; optional `facing`
and `priority` properties refine activation, while `interaction` references a
script in `scripts/`. Short non-modal remarks use that script's `speak` action.
The compiler converts the text to the same glyph data as other speech, so the
ROM needs neither JSON parsing nor runtime font rendering.

The exterior collision layer retains the original per-square directional
masks. Objects named `unported_*` close thresholds where the original map
immediately changed to a scene that has not been converted yet (the barn,
veranda/Silverhair route, guardhouse, and kitchen pantry). Remove the corresponding guard when
that transition and destination scene are added; leaving the threshold as
ordinary walkable exterior terrain permits walking through the building art.
