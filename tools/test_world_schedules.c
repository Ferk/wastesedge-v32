#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "content_ids.h"
#include "engine/savegame.h"
#include "engine/world.h"

static void tick(struct world_state *world, int frames)
{
    struct world_actions actions = { 0, 0, 0, 0, 0, 0 };
    int transitioned = 0;
    while (frames-- > 0)
        world_update(world, &actions);
    /* Scene changes deliberately span a fade-out and fade-in. Most tests are
     * interested in the settled destination rather than an intermediate
     * transition frame. */
    while (world->transition_phase != WORLD_TRANSITION_NONE) {
        transitioned = 1;
        world_update(world, &actions);
    }
    /* Let the scene-change action's usual one-frame wait and immediate
     * follow-up (often a conversation) settle after the fade. */
    if (transitioned) {
        world_update(world, &actions);
        world_update(world, &actions);
        world_update(world, &actions);
    }
}

static void walk_tile(struct world_state *world, const struct world_actions *direction)
{
    world_update(world, direction);
    tick(world, 16);
}

static void run_script(struct world_state *world, int script)
{
    world->dialogue = -1;
    world->dialogue_actor = -1;
    world->active_script = script;
    world->script_action = 0;
    world->script_wait = 0;
    tick(world, 2);
}

static int count_object_state(const struct world_state *world, int definition, int state)
{
    int count = 0;
    int scene_index;
    for (scene_index = 0; scene_index < generated_runtime_scene_count; ++scene_index) {
        const struct generated_runtime_scene *scene = &generated_runtime_scenes[scene_index];
        int scenery_index;
        for (scenery_index = 0; scenery_index < scene->scenery_count; ++scenery_index) {
            const struct generated_runtime_scenery *scenery = &scene->scenery[scenery_index];
            if (scenery->definition == definition && scenery->state_slot >= 0 &&
                world->object_states[scenery->state_slot] == state)
                ++count;
        }
    }
    return count;
}

static int count_translucent_scenery(const struct generated_runtime_scene *scene)
{
    int count = 0;
    int scenery_index;
    for (scenery_index = 0; scenery_index < scene->scenery_count; ++scenery_index)
        if (scene->scenery[scenery_index].alpha < 255)
            ++count;
    return count;
}

static int find_scene_entity(const struct generated_runtime_scene *scene, int definition)
{
    int index;
    for (index = 0; index < scene->entity_count; ++index)
        if (scene->entities[index].definition == definition)
            return index;
    return -1;
}

static void assert_extro_position(int x, int y, int facing)
{
    int expected_facing;
    assert(x >= 2 && x <= 5);
    assert(y >= 5 && y <= 9);
    assert(y != 5 || x == 2);
    if (x + y < 10)
        expected_facing = x + 3 > y ? WORLD_SOUTH : WORLD_EAST;
    else
        expected_facing = y - (x + 3) > 0 ? WORLD_NORTH : WORLD_WEST;
    assert(facing == expected_facing);
}

static int single_condition_flag(const struct generated_runtime_dialogue_choice *choice)
{
    assert(choice->condition_count == 1);
    return choice->conditions[0].flag;
}

static int single_condition_value(const struct generated_runtime_dialogue_choice *choice)
{
    assert(choice->condition_count == 1);
    return choice->conditions[0].value;
}

static const struct generated_runtime_dialogue_node *find_dialogue_node(
    const struct generated_runtime_dialogue *dialogue, const char *text)
{
    int index;
    for (index = 0; index < dialogue->node_count; ++index)
        if (strcmp(dialogue->nodes[index].text, text) == 0)
            return &dialogue->nodes[index];
    return 0;
}

static const struct generated_runtime_dialogue_choice *find_dialogue_choice(
    const struct generated_runtime_dialogue_node *node, const char *text)
{
    int index;
    for (index = 0; index < node->choice_count; ++index)
        if (strcmp(node->choices[index].text, text) == 0)
            return &node->choices[index];
    return 0;
}

static int choice_has_action(const struct generated_runtime_dialogue_choice *choice,
                             int flag, int value)
{
    int index;
    for (index = 0; index < choice->action_count; ++index)
        if (choice->actions[index].flag == flag && choice->actions[index].value == value)
            return 1;
    return 0;
}

static int choice_has_condition(const struct generated_runtime_dialogue_choice *choice,
                                int flag, int value)
{
    int index;
    for (index = 0; index < choice->condition_count; ++index)
        if (choice->conditions[index].flag == flag && choice->conditions[index].value == value)
            return 1;
    return 0;
}

static void choose_visible_dialogue_option(struct world_state *world, const char *text)
{
    struct world_actions interact = { 0, 0, 1, 0, 0, 0 };
    int index;
    for (index = 0; index < world_dialogue_choice_count(world); ++index) {
        if (strcmp(world_dialogue_choice_at(world, index), text) == 0) {
            world->dialogue_choice = index;
            world_update(world, &interact);
            return;
        }
    }
    fprintf(stderr, "missing dialogue option: %s\ncurrent text: %s\n", text,
            world_dialogue_text(world));
    for (index = 0; index < world_dialogue_choice_count(world); ++index)
        fprintf(stderr, "  visible: %s\n", world_dialogue_choice_at(world, index));
    assert(!"expected visible dialogue option");
}

int main(void)
{
    struct world_state world;
    struct world_actor_state *talan;
    struct world_actor_state *tristan;
    struct world_actor_state *orloth;
    struct world_actor_state *alek;
    struct world_actor_state *erek;
    struct world_actor_state *oliver;
    struct world_actor_state *frostbloom;
    struct world_actor_state *jelom;
    struct world_actor_state *lucia;
    struct world_actor_state *silverhair;
    struct world_actor_state *sarin;
    struct world_actor_state *janesta;
    const struct generated_runtime_dialogue *talan_dialogue;
    const struct generated_runtime_dialogue *oliver_dialogue;
    const struct generated_runtime_dialogue *jelom_dialogue;
    const struct generated_runtime_dialogue_node *jelom_node = 0;
    const struct generated_runtime_dialogue_node *jelom_question_node = 0;
    const struct generated_runtime_dialogue_node *jelom_suspicion_node = 0;
    struct world_actions interact = { 0, 0, 1, 0, 0, 0 };
    struct world_actions move_north = { 0, -1, 0, 0, 0, 0 };
    struct world_actions move_east = { 1, 0, 0, 0, 0, 0 };
    struct world_actions move_west = { -1, 0, 0, 0, 0, 0 };
    struct world_actions move_south = { 0, 1, 0, 0, 0, 0 };
    int frozen_x;
    int frozen_y;
    int frozen_progress;
    int frame;
    int node_index;
    int visual_index;
    int scene_index;
    int entity_index;
    int scenery_index;
    int branch_index;
    int first_random_step;
    int oliver_fact_bindings = 0;
    int alek_fact_bindings = 0;
    int packed_fact_bindings = 0;
    int packed_followups = 0;
    int translucent_scenery = 0;
    int kitchen_smoke_pieces = 0;
    int entrance_back_pieces = 0;
    int entrance_fore_pieces = 0;
    int entrance_back_eaves = 0;
    int barrel_pieces = 0;
    int vine_pieces = 0;
    int packed_scenery_regions = 0;
    int pantry_flag;
    int bjarn_door_flag;
    int get_item_flag;
    int have_gem_flag;
    int silverhair_gem_pending_flag;
    int gem_worthless_flag;
    int direction;
    int animation;

    world_initialize(&world);
    assert(generated_runtime_scene_count == 23);
    assert(count_object_state(&world, GENERATED_ENTITY_TABLE_MUG, 1) == 8);
    {
        struct world_state clock_world;
        struct world_actions idle = { 0, 0, 0, 0, 0, 0 };
        world_initialize(&clock_world);
        assert(clock_world.game_time == generated_runtime_initial_game_time);
        for (frame = 0; frame < 60; ++frame)
            world_update(&clock_world, &idle);
        assert(clock_world.game_time == generated_runtime_initial_game_time + 8);
        assert(clock_world.game_time_subticks == 0);
    }

    /* Examine actions are authored as activation-square hotspots. They retain
     * the facing requirement and produce ordinary non-modal actor speech. */
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_EXTERIOR]
               .hotspot_count == 4);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_INN]
               .hotspot_count == 1);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_PARLOUR]
               .hotspot_count == 4);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_PLAYER_ROOM]
               .hotspot_count == 2);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_SILVERHAIR_ROOM]
               .hotspot_count == 1);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_CELLAR]
               .hotspot_count == 8);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_CELLAR]
               .width == 16);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_CELLAR]
               .height == 15);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_CELLAR]
               .navigation_blocker_count == 11);
    /* Legacy cellar walls are composites: their opaque back and translucent
     * front animations must both survive the one-time Tiled import. */
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_CELLAR]
               .scenery_count == 68);
    assert(count_translucent_scenery(
               &generated_runtime_scenes[GENERATED_SCENE_REDWYNE_CELLAR]) == 17);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_FINGOLSON_ROOM]
               .scenery_count == 31);
    assert(count_translucent_scenery(
               &generated_runtime_scenes[GENERATED_SCENE_REDWYNE_FINGOLSON_ROOM]) == 6);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_PANTRY]
               .scenery_count == 63);
    assert(count_translucent_scenery(
               &generated_runtime_scenes[GENERATED_SCENE_REDWYNE_PANTRY]) == 6);
    /* Walkable kitchen flooring is a Tiled Tile Layer. It must never enter
     * the actor/scenery depth merge, where a lower row could cover feet. */
    /* The compiler packs those 24 authored tiles into one static background
     * texture, so they also consume only one draw command at runtime. */
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_KITCHEN]
               .background_count == 1);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_KITCHEN]
               .scenery_count == 50);
    world.dialogue = -1;
    world.active_script = -1;
    world.control_enabled = 1;
    world.player_x = 6;
    world.player_y = 17;
    world.player_facing = WORLD_NORTH;
    world_update(&world, &interact);
    assert(world_entity_speech(&world, world.player_entity) == 0);
    world.player_facing = WORLD_WEST;
    world_update(&world, &interact);
    assert(world_entity_speech(&world, world.player_entity) != 0);
    assert(world.actors[GENERATED_ENTITY_PLAYER].speech_frames == 180);

    /* Jelom is an ordinary data-authored actor interaction on the landing.
     * His ordered start cases switch from the original hallway warning to
     * the repeat greeting once the conversation has established his identity. */
    world.scene_index = GENERATED_SCENE_REDWYNE_FIRST_FLOOR;
    world.scene = &generated_runtime_scenes[world.scene_index];
    world.player_entity = 0;
    assert(world.scene->entities[world.player_entity].player_controlled);
    world.actors[GENERATED_ENTITY_JELOM].schedule = -1;
    world.actors[GENERATED_ENTITY_JELOM].scene = world.scene_index;
    world.actors[GENERATED_ENTITY_JELOM].x = 2;
    world.actors[GENERATED_ENTITY_JELOM].y = 6;
    world.player_x = 2;
    world.player_y = 7;
    world.player_facing = WORLD_NORTH;
    world.dialogue = -1;
    world.dialogue_actor = -1;
    world_update(&world, &interact);
    assert(world.dialogue == GENERATED_DIALOGUE_JELOM_START);
    assert(strcmp(world_dialogue_text(&world),
                  "No-one is allowed into the prisoner's room, so please leave this hallway!") == 0);
    assert(world_dialogue_choice_count(&world) == 2);
    world.dialogue = -1;
    world.dialogue_actor = -1;
    world.flags[generated_runtime_dialogues[GENERATED_DIALOGUE_JELOM_START]
                    .starts[1].flag] = 2;
    world_update(&world, &interact);
    assert(strcmp(world_dialogue_text(&world),
                  "You again, Half-Elf? What do you want this time?") == 0);

    /* Releasing Silverhair permanently switches Jelom to the second,
     * evidence-driven conversation. Erek's agreement then reproduces the
     * original submap-9 staging before opening their joint testimony. */
    world.dialogue = -1;
    world.dialogue_actor = -1;
    world.active_script = GENERATED_SCRIPT_ACTIVATE_JELOM_SECOND_DIALOGUE;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 1);
    assert(world.actors[GENERATED_ENTITY_JELOM].dialogue ==
           GENERATED_DIALOGUE_JELOM_2ND);

    world.flags[generated_runtime_scripts[
        GENERATED_SCRIPT_RECORD_JELOM_NEEDS_EREK].actions[0].a] = 2;
    world.active_script = GENERATED_SCRIPT_ESCORT_EREK_TO_JELOM;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 3);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_FIRST_FLOOR);
    assert(world.player_x == 1 && world.player_y == 3);
    assert(world.player_facing == WORLD_EAST);
    assert(world.actors[GENERATED_ENTITY_JELOM].scene == world.scene_index);
    assert(world.actors[GENERATED_ENTITY_JELOM].x == 2 &&
           world.actors[GENERATED_ENTITY_JELOM].y == 3);
    assert(world.actors[GENERATED_ENTITY_EREK].scene == world.scene_index);
    assert(world.actors[GENERATED_ENTITY_EREK].x == 2 &&
           world.actors[GENERATED_ENTITY_EREK].y == 4);
    assert(world.actors[GENERATED_ENTITY_EREK].schedule == -1);
    /* Runtime actors are world-level instances. Erek reaches this scene even
     * though the Tiled map deliberately has no destination marker for him. */
    entity_index = find_scene_entity(world.scene, GENERATED_ENTITY_EREK);
    assert(entity_index < 0);
    assert(world_actor_is_present(&world, GENERATED_ENTITY_EREK));
    assert(generated_runtime_actors[GENERATED_ENTITY_EREK].interaction_dialogue ==
           GENERATED_DIALOGUE_EREK_START);
    assert(generated_runtime_actors[GENERATED_ENTITY_PLAYER].speech_color == 0xFF7FEEFFu);
    assert(generated_runtime_actors[GENERATED_ENTITY_BJARN].speech_color == 0xFF7BACFFu);
    assert(generated_runtime_actors[GENERATED_ENTITY_EREK].speech_color == 0xFFE086D8u);
    assert(generated_runtime_actors[GENERATED_ENTITY_FROSTBLOOM].speech_color == 0xFFEDB989u);
    assert(generated_runtime_actors[GENERATED_ENTITY_JELOM].speech_color == 0xFF91FFCFu);
    assert(world.dialogue == GENERATED_DIALOGUE_JELOM_2ND);
    assert(strcmp(world_dialogue_text(&world),
                  "What's going on now? What are you doing here, Erek Stonebreaker?") == 0);

    /* Once Fingolson's lie is known, Erek corroborates Banec's evidence and
     * Jelom advances the original quest flags to the final gathering. */
    world.flags[find_dialogue_choice(
        find_dialogue_node(&generated_runtime_dialogues[GENERATED_DIALOGUE_JELOM_2ND],
                           "If this gem really belongs to Master Fingolson, why don't I hear what he has to say about it?"),
        "Well, Master Fingolson is pretending that this gem is a valuable Catseye, despite the fact that it isn't.")
        ->conditions[0].flag] = 1;
    world_update(&world, &interact);
    world_update(&world, &interact);
    choose_visible_dialogue_option(&world,
        "Listen Jelom, I am getting sick of you. What is it you want?");
    world_update(&world, &interact);
    choose_visible_dialogue_option(&world,
        "Well, Master Fingolson is pretending that this gem is a valuable Catseye, despite the fact that it isn't.");
    world_update(&world, &interact);
    world_update(&world, &interact);
    world_update(&world, &interact);
    tick(&world, 3);
    assert(world.flags[generated_runtime_scripts[
               GENERATED_SCRIPT_FINISH_JELOM_INVESTIGATION].actions[0].a] == 3);
    assert(world.flags[generated_runtime_scripts[
               GENERATED_SCRIPT_FINISH_JELOM_INVESTIGATION].actions[1].a] == 1);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_FINGOLSON_ROOM);
    assert_extro_position(world.player_x, world.player_y, world.player_facing);
    assert(world.dialogue == GENERATED_DIALOGUE_EXTRO);
    assert(world.actors[GENERATED_ENTITY_EREK].scene == world.scene_index);
    assert(world.actors[GENERATED_ENTITY_JELOM].scene == world.scene_index);
    assert(world.actors[GENERATED_ENTITY_SILVERHAIR].scene == world.scene_index);
    assert(world.actors[GENERATED_ENTITY_TRISTAN].scene == world.scene_index);
    assert(world.actors[GENERATED_ENTITY_FELLNIR].scene == world.scene_index);
    /* Fingolson's await_player callback fixed him at (3,6), facing west.
     * The player and visitors occupied free cells in the original (2..5, 5..9)
     * gathering area and faced inward according to their chosen cell. */
    assert(world.actors[GENERATED_ENTITY_BJARN].x == 3 &&
           world.actors[GENERATED_ENTITY_BJARN].y == 6 &&
           world.actors[GENERATED_ENTITY_BJARN].facing == WORLD_WEST);
    assert_extro_position(world.actors[GENERATED_ENTITY_EREK].x,
                          world.actors[GENERATED_ENTITY_EREK].y,
                          world.actors[GENERATED_ENTITY_EREK].facing);
    assert_extro_position(world.actors[GENERATED_ENTITY_JELOM].x,
                          world.actors[GENERATED_ENTITY_JELOM].y,
                          world.actors[GENERATED_ENTITY_JELOM].facing);
    assert_extro_position(world.actors[GENERATED_ENTITY_SILVERHAIR].x,
                          world.actors[GENERATED_ENTITY_SILVERHAIR].y,
                          world.actors[GENERATED_ENTITY_SILVERHAIR].facing);
    assert_extro_position(world.actors[GENERATED_ENTITY_TRISTAN].x,
                          world.actors[GENERATED_ENTITY_TRISTAN].y,
                          world.actors[GENERATED_ENTITY_TRISTAN].facing);
    assert_extro_position(world.actors[GENERATED_ENTITY_FELLNIR].x,
                          world.actors[GENERATED_ENTITY_FELLNIR].y,
                          world.actors[GENERATED_ENTITY_FELLNIR].facing);
    {
        const int visitors[] = { GENERATED_ENTITY_EREK, GENERATED_ENTITY_JELOM,
                                 GENERATED_ENTITY_SILVERHAIR, GENERATED_ENTITY_TRISTAN,
                                 GENERATED_ENTITY_FELLNIR };
        int left;
        int right;
        for (left = 0; left < 5; ++left) {
            const struct world_actor_state *actor = &world.actors[visitors[left]];
            assert(actor->x != world.player_x || actor->y != world.player_y);
            assert(actor->x != world.actors[GENERATED_ENTITY_BJARN].x ||
                   actor->y != world.actors[GENERATED_ENTITY_BJARN].y);
            for (right = left + 1; right < 5; ++right)
                assert(actor->x != world.actors[visitors[right]].x ||
                       actor->y != world.actors[visitors[right]].y);
        }
    }

    /* A direct accusation path must converge on Fingolson's confession and
     * hand control to the authored aftermath without restoring player input. */
    world_update(&world, &interact);
    world_update(&world, &interact);
    world_update(&world, &interact);
    world_update(&world, &interact);
    world_update(&world, &interact);
    choose_visible_dialogue_option(&world,
        "It's obvious that the thief tried to frame Lady Silverhair.");
    choose_visible_dialogue_option(&world,
        "No. I mean the theft's sole purpose was to discredit my mistress.");
    world_update(&world, &interact);
    choose_visible_dialogue_option(&world,
        "You do! And you knew of her business.");
    world_update(&world, &interact);
    choose_visible_dialogue_option(&world,
        "But your stones are of poor quality indeed!");
    world_update(&world, &interact);
    world_update(&world, &interact);
    world_update(&world, &interact);
    world_update(&world, &interact);
    world_update(&world, &interact);
    world_update(&world, &interact);
    world_update(&world, &interact);
    choose_visible_dialogue_option(&world,
        "Then you confess that the theft was just pretended?");
    tick(&world, 1);
    assert(world.dialogue < 0);
    assert(world.active_script == GENERATED_SCRIPT_BEGIN_EXTRO_AFTERMATH);
    assert(world_actor_speech(&world, GENERATED_ENTITY_BJARN) != 0);
    assert(world.actors[GENERATED_ENTITY_BJARN].speech_frames == 180);
    /* The post-confession exchange remains a world-view cutscene. Its only
     * modal action is the final epilogue after all scripted bubbles finish. */
    for (frame = 0;
         frame < generated_runtime_scripts[GENERATED_SCRIPT_BEGIN_EXTRO_AFTERMATH]
                     .action_count - 1;
         ++frame)
        assert(generated_runtime_scripts[GENERATED_SCRIPT_BEGIN_EXTRO_AFTERMATH]
                   .actions[frame].op != GENERATED_SCRIPT_SHOW_DIALOGUE);
    assert(world.control_enabled == 0);
    {
        struct world_state completed_extro = world;
        tick(&completed_extro, 12000);
        assert(completed_extro.dialogue == GENERATED_DIALOGUE_EXTRO_EPILOGUE);
        assert(completed_extro.active_script ==
               GENERATED_SCRIPT_BEGIN_EXTRO_AFTERMATH);
        assert(completed_extro.actors[GENERATED_ENTITY_BJARN].x == 7 &&
               completed_extro.actors[GENERATED_ENTITY_BJARN].y == 4);
        assert(completed_extro.actors[GENERATED_ENTITY_TALAN].x == 2 &&
               completed_extro.actors[GENERATED_ENTITY_TALAN].y == 6);
        assert(completed_extro.player_facing == WORLD_WEST);
        assert(completed_extro.actors[GENERATED_ENTITY_EREK].facing == WORLD_WEST);
        assert(completed_extro.actors[GENERATED_ENTITY_JELOM].facing == WORLD_WEST);
        assert(completed_extro.actors[GENERATED_ENTITY_SILVERHAIR].facing == WORLD_WEST);
        assert(world_actor_visual(&completed_extro, GENERATED_ENTITY_BJARN) !=
               generated_runtime_actors[GENERATED_ENTITY_BJARN].visual);
        assert(world_actor_visual(&completed_extro, GENERATED_ENTITY_TALAN) !=
               generated_runtime_actors[GENERATED_ENTITY_TALAN].visual);
    }

    world.dialogue = -1;
    world.active_script = GENERATED_SCRIPT_REVEAL_EXTRO_CHEST;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 3);
    assert(world.dialogue < 0);
    assert(world.actors[GENERATED_ENTITY_BJARN].moving);
    assert(world.actors[GENERATED_ENTITY_BJARN].x != 7 ||
           world.actors[GENERATED_ENTITY_BJARN].y != 3);
    {
        int saw_diagonal = 0;
        int path_frame;
        for (path_frame = 0; path_frame < 200 && world.dialogue < 0; ++path_frame) {
            const struct world_actor_state *bjarn =
                &world.actors[GENERATED_ENTITY_BJARN];
            if (bjarn->moving) {
                if (bjarn->move_x != 0 && bjarn->move_y != 0)
                    saw_diagonal = 1;
                /* The room's east wall occupies tile column 8. It is not a
                 * corridor merely because its old directional mask permits
                 * travel parallel to the wall. */
                assert(bjarn->x != 8);
                assert(bjarn->x + bjarn->move_x != 8);
            }
            tick(&world, 1);
        }
        assert(saw_diagonal);
    }
    assert(world.dialogue == GENERATED_DIALOGUE_EXTRO_CHEST);
    assert(world.actors[GENERATED_ENTITY_BJARN].x == 7 &&
           world.actors[GENERATED_ENTITY_BJARN].y == 3);

    world.dialogue = -1;
    world.active_script = GENERATED_SCRIPT_TALAN_ENTERS_EXTRO;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 3);
    assert(world.dialogue == GENERATED_DIALOGUE_EXTRO_TALAN);
    assert(world_actor_visual(&world, GENERATED_ENTITY_BJARN) !=
           generated_runtime_actors[GENERATED_ENTITY_BJARN].visual);
    assert(world_actor_visual(&world, GENERATED_ENTITY_TALAN) !=
           generated_runtime_actors[GENERATED_ENTITY_TALAN].visual);
    assert(generated_runtime_actor_visuals[
               world_actor_visual(&world, GENERATED_ENTITY_BJARN)]
               .standing_count[WORLD_SOUTH] == 2);
    assert(generated_runtime_actor_visuals[
               world_actor_visual(&world, GENERATED_ENTITY_BJARN)]
               .standing_delay[WORLD_SOUTH] == 13);
    assert(generated_runtime_actor_visuals[
               world_actor_visual(&world, GENERATED_ENTITY_TALAN)]
               .walking_count[WORLD_EAST] == 4);
    assert(world.actors[GENERATED_ENTITY_TALAN].scene == world.scene_index);
    assert(world.actors[GENERATED_ENTITY_TALAN].x == 1 &&
           world.actors[GENERATED_ENTITY_TALAN].y == 6);
    tick(&world, 11);
    assert(world.actors[GENERATED_ENTITY_BJARN].x == 7 &&
           world.actors[GENERATED_ENTITY_BJARN].y == 4 &&
           world.actors[GENERATED_ENTITY_BJARN].facing == WORLD_SOUTH);
    assert(world.actors[GENERATED_ENTITY_TALAN].x == 2 &&
           world.actors[GENERATED_ENTITY_TALAN].y == 6 &&
           world.actors[GENERATED_ENTITY_TALAN].facing == WORLD_SOUTH);
    {
        int crying_tick = world_actor_visual_tick(&world, GENERATED_ENTITY_BJARN);
        tick(&world, 13);
        assert(world_actor_visual_tick(&world, GENERATED_ENTITY_BJARN) ==
               crying_tick + 13);
    }

    world.dialogue = -1;
    world.active_script = GENERATED_SCRIPT_SHOW_EXTRO_EPILOGUE;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 1);
    assert(world.dialogue == GENERATED_DIALOGUE_EXTRO_EPILOGUE);
    world.dialogue = -1;
    world.active_script = GENERATED_SCRIPT_SHOW_EXTRO_CREDITS;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 1);
    assert(world.dialogue == GENERATED_DIALOGUE_EXTRO_CREDITS);
    world.dialogue = -1;
    world.active_script = GENERATED_SCRIPT_FINISH_EXTRO;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 1);
    assert(world.flags[generated_runtime_scripts[
               GENERATED_SCRIPT_FINISH_EXTRO].actions[0].a] == 1);
    assert(world.control_enabled == 0);

    /* Restore the landing for the independent doorway checks below. */
    world_initialize(&world);
    world.scene_index = GENERATED_SCENE_REDWYNE_FIRST_FLOOR;
    world.scene = &generated_runtime_scenes[world.scene_index];
    world.player_entity = 0;

    /* The Silverhair doorway uses two overlapping data-authored triggers.
     * Before Jelom releases her it opens his warning; afterwards the same
     * step changes to the original room and its reciprocal doorway. */
    assert(world.scene->trigger_count == 7);
    assert(world.scene->triggers[2].x == 1 && world.scene->triggers[2].y == 7);
    assert(world.scene->triggers[2].condition_flag >= 0);
    assert(world.scene->triggers[2].condition_value == 1);
    assert(world.scene->triggers[3].condition_flag ==
           world.scene->triggers[2].condition_flag);
    assert(world.scene->triggers[3].condition_value == 0);
    world.dialogue = -1;
    world.dialogue_actor = -1;
    world.flags[world.scene->triggers[2].condition_flag] = 0;
    world.player_x = 1;
    world.player_y = 6;
    world.player_facing = WORLD_SOUTH;
    world.moving = 0;
    world.trigger_latch = -1;
    world_update(&world, &move_south);
    tick(&world, 11);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_FIRST_FLOOR);
    assert(world.dialogue == GENERATED_DIALOGUE_JELOM_START);

    world.dialogue = -1;
    world.dialogue_actor = -1;
    world.active_script = -1;
    world.flags[world.scene->triggers[2].condition_flag] = 1;
    world.player_x = 1;
    world.player_y = 6;
    world.player_facing = WORLD_SOUTH;
    world.moving = 0;
    world.trigger_latch = -1;
    world_update(&world, &move_south);
    tick(&world, 11);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_SILVERHAIR_ROOM);
    assert(world.player_x == 5 && world.player_y == 2);
    assert(world.scene->entity_count == 4);
    assert(world.scene->trigger_count == 3);

    /* Submap 13 has one original positional event: from (1,6), facing north,
     * Banec refuses to touch his mistress' belongings. The chest-search event
     * is in another submap and must not be attached to this room's chest. */
    world.dialogue = -1;
    world.dialogue_actor = -1;
    world.active_script = -1;
    world.control_enabled = 1;
    world.player_x = 1;
    world.player_y = 6;
    world.player_facing = WORLD_SOUTH;
    world.actors[GENERATED_ENTITY_PLAYER].speech_line = -1;
    world.actors[GENERATED_ENTITY_PLAYER].speech_frames = 0;
    world.actors[GENERATED_ENTITY_PLAYER].speech_override = 0;
    world_update(&world, &interact);
    assert(world_entity_speech(&world, world.player_entity) == 0);
    world.player_facing = WORLD_NORTH;
    world_update(&world, &interact);
    assert(world_entity_speech(&world, world.player_entity) ==
           &generated_runtime_scripts[GENERATED_SCRIPT_EXAMINE_SILVERHAIR_BELONGINGS]
                .speech[0]);

    /* Restore pristine schedules, flags and introductory script state. */
    world_initialize(&world);

    /* Authored character frames remain separate PNGs, but every runtime actor
     * visual must occupy one generated texture and address distinct regions. */
    for (visual_index = 0; visual_index < generated_runtime_actor_visual_count; ++visual_index) {
        const struct generated_runtime_actor_visual *visual =
            &generated_runtime_actor_visuals[visual_index];
        int texture = visual->standing[0][0].texture;
        assert(texture >= 0);
        for (direction = 0; direction < 4; ++direction) {
            int standing_frame;
            assert(visual->standing_count[direction] > 0);
            for (standing_frame = 0;
                 standing_frame < visual->standing_count[direction];
                 ++standing_frame)
                assert(visual->standing[direction][standing_frame].texture == texture);
            for (animation = 0; animation < visual->walking_count[direction]; ++animation)
                assert(visual->walking[direction][animation].texture == texture);
        }
        assert(visual->standing[0][0].source_x != visual->standing[1][0].source_x ||
               visual->standing[0][0].source_y != visual->standing[1][0].source_y);
        assert(visual->standing[0][0].width > 0 && visual->standing[0][0].height > 0);
    }

    /* Authored scenery opacity survives content compilation. These are the
     * front-facing indoor pieces that reveal actors standing behind them. */
    for (scene_index = 0; scene_index < generated_runtime_scene_count; ++scene_index) {
        const struct generated_runtime_scene *scene = &generated_runtime_scenes[scene_index];
        for (scenery_index = 0; scenery_index < scene->scenery_count; ++scenery_index) {
            const struct generated_runtime_scenery *scenery = &scene->scenery[scenery_index];
            int alpha = scenery->alpha;
            if (scenery->frame.source_x != 0 || scenery->frame.source_y != 0)
                ++packed_scenery_regions;
            assert(alpha >= 0 && alpha <= 255);
            if (alpha < 255) {
                assert(alpha == 128 ||
                       (scene_index == GENERATED_SCENE_REDWYNE_KITCHEN && alpha == 160) ||
                       (scene_index == GENERATED_SCENE_REDWYNE_BATH && alpha == 150));
                /* Imported scenery sorts on its logical source base, not on
                 * an alpha-dependent image-bottom adjustment. */
                assert(scene->scenery[scenery_index].depth % scene->tile_size == 0);
                if (scene_index == GENERATED_SCENE_REDWYNE_KITCHEN && alpha == 160)
                    ++kitchen_smoke_pieces;
                ++translucent_scenery;
            }
            if (scene_index == GENERATED_SCENE_REDWYNE_EXTERIOR &&
                scenery->x == 360 && scenery->y == 200 &&
                scenery->frame.width == 20 && scenery->frame.height == 53) {
                assert(scenery->depth == 240);
                ++entrance_back_pieces;
            }
            if (scene_index == GENERATED_SCENE_REDWYNE_EXTERIOR &&
                scenery->x == 360 && scenery->y == 244 &&
                scenery->frame.width == 20 && scenery->frame.height == 68) {
                assert(scenery->depth == 280);
                ++entrance_fore_pieces;
            }
            if (scene_index == GENERATED_SCENE_REDWYNE_EXTERIOR &&
                scenery->x == 340 && scenery->y == 260 &&
                scenery->frame.width == 60 && scenery->frame.height == 60) {
                /* The broad eave belongs to the rear wall of the doorway.
                 * Keep it behind a player standing on its Y=260 row; the
                 * separate narrow entrance-top-fore gable remains in front. */
                assert(scenery->depth == 260);
                ++entrance_back_eaves;
            }
            if (scene_index == GENERATED_SCENE_REDWYNE_INN &&
                ((scenery->x == 20 && scenery->y == 28) ||
                 (scenery->x == 40 && scenery->y == 48))) {
                assert(scenery->depth == 60);
                ++barrel_pieces;
            }
            if (scene_index == GENERATED_SCENE_REDWYNE_EXTERIOR &&
                ((scenery->x == 200 && scenery->y == 340) ||
                 (scenery->x == 180 && scenery->y == 280) ||
                 (scenery->x == 160 && scenery->y == 220))) {
                assert(scenery->depth == 340);
                ++vine_pieces;
            }
        }
    }
    assert(translucent_scenery >= 24);
    assert(kitchen_smoke_pieces == 24);
    assert(entrance_back_pieces == 1);
    assert(entrance_fore_pieces == 1);
    assert(entrance_back_eaves == 1);
    assert(barrel_pieces == 2);
    assert(vine_pieces == 3);
    assert(packed_scenery_regions > 0);

    /* Exterior squares immediately change scene when their destination is
     * ported; the remaining unported entrances stay closed. The two west-wall checks
     * also prevent walking around the closed gate at either end of the wall.
     * The lower check overlaps an imported mask and therefore exercises the
     * original bitwise-AND composition of mapsquare walkability. */
    world.dialogue = -1;
    world.active_script = -1;
    world.control_enabled = 1;
    world.moving = 0;
    world.player_x = 5;
    world.player_y = 5;
    world_update(&world, &move_east);
    assert(!world.moving && world.player_x == 5 && world.player_y == 5);
    world.player_y = 33;
    world_update(&world, &move_east);
    assert(!world.moving && world.player_x == 5 && world.player_y == 33);
    world.player_x = 24;
    world.player_y = 14;
    world_update(&world, &move_north);
    tick(&world, 12);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_BARN);
    assert(world.player_x == 1 && world.player_y == 6);
    assert(world.player_facing == WORLD_NORTH);
    world.trigger_latch = -1;
    world_update(&world, &move_south);
    tick(&world, 12);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_EXTERIOR);
    assert(world.player_x == 24 && world.player_y == 14);
    assert(world.player_facing == WORLD_SOUTH);
    world.player_x = 12;
    world.player_y = 14;
    world_update(&world, &move_west);
    assert(world.moving);
    tick(&world, 20);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_KITCHEN);
    assert(world.player_x == 6 && world.player_y == 3);
    assert(world.player_facing == WORLD_WEST);

    /* Restore pristine schedules, flags and introductory script state for the
     * broader integration checks below. */
    world_initialize(&world);

    world.dialogue = -1;
    world.active_script = GENERATED_SCRIPT_ENTER_INN;
    world.script_action = 0;
    world.script_wait = 0;
    /* Opening the gate also changes its directional walkability. With the
     * legacy pathfinding exclusions present, the scripted route crosses the
     * gate directly instead of searching through the inn walls. */
    for (frame = 0; frame < 180 && world.active_script >= 0; ++frame) {
        tick(&world, 1);
        assert(world.player_y == 18);
    }
    assert(frame < 180);
    assert(world.active_script < 0);
    assert(world.control_enabled);
    assert(world.player_x == 11 && world.player_y == 18);
    assert(world.music_track == GENERATED_MUSIC_REDWYNE_THEME);
    assert(world.music_revision == 1);
    assert(generated_runtime_scenes[GENERATED_SCENE_REDWYNE_EXTERIOR]
               .navigation_blocker_count == 3);
    tick(&world, 180 - frame);

    talan = &world.actors[GENERATED_ENTITY_TALAN];
    tristan = &world.actors[GENERATED_ENTITY_TRISTAN];
    orloth = &world.actors[GENERATED_ENTITY_ORLOTH];
    alek = &world.actors[GENERATED_ENTITY_ALEK];
    erek = &world.actors[GENERATED_ENTITY_EREK];
    oliver = &world.actors[GENERATED_ENTITY_OLIVER];
    frostbloom = &world.actors[GENERATED_ENTITY_FROSTBLOOM];
    jelom = &world.actors[GENERATED_ENTITY_JELOM];
    lucia = &world.actors[GENERATED_ENTITY_LUCIA];
    silverhair = &world.actors[GENERATED_ENTITY_SILVERHAIR];
    sarin = &world.actors[GENERATED_ENTITY_SARIN];
    janesta = &world.actors[GENERATED_ENTITY_JANESTA];
    assert(talan->schedule == GENERATED_SCHEDULE_TALAN_GATE_PATROL);
    assert(tristan->schedule == GENERATED_SCHEDULE_TRISTAN_INN_AND_GATE);
    assert(orloth->schedule == GENERATED_SCHEDULE_ORLOTH_COMMON_ROOM);
    assert(alek->schedule == GENERATED_SCHEDULE_ALEK_BAR_AND_TABLE);
    assert(frostbloom->schedule == GENERATED_SCHEDULE_FROSTBLOOM_YARD);
    assert(erek->schedule == GENERATED_SCHEDULE_EREK_COMMON_ROOM);
    assert(oliver->schedule == GENERATED_SCHEDULE_OLIVER_YARD);
    assert(jelom->schedule == GENERATED_SCHEDULE_JELOM_FIRST_FLOOR);
    assert(lucia->schedule == GENERATED_SCHEDULE_LUCIA_KITCHEN);
    assert(silverhair->schedule == GENERATED_SCHEDULE_SILVERHAIR_ROOM);
    assert(sarin->schedule == GENERATED_SCHEDULE_SARIN_ROOM);
    assert(janesta->schedule == GENERATED_SCHEDULE_JANESTA_ROOM);
    if (orloth->scene != GENERATED_SCENE_REDWYNE_INN)
        fprintf(stderr, "Orloth initialization mismatch: scene=%d x=%d y=%d schedule=%d step=%d\n",
                orloth->scene, orloth->x, orloth->y,
                orloth->schedule, orloth->schedule_step);
    assert(orloth->scene == GENERATED_SCENE_REDWYNE_INN);
    assert(orloth->x == 2 && orloth->y == 2);
    assert(alek->scene == GENERATED_SCENE_REDWYNE_INN);
    assert(alek->x == 1 && alek->y == 3);
    assert(erek->scene == GENERATED_SCENE_REDWYNE_INN);
    assert(erek->x == 5 && erek->y == 5);
    assert(oliver->scene == GENERATED_SCENE_REDWYNE_EXTERIOR);
    assert(oliver->x == 25 && oliver->y == 15);
    assert(frostbloom->scene == GENERATED_SCENE_REDWYNE_EXTERIOR);
    assert(jelom->scene == GENERATED_SCENE_REDWYNE_FIRST_FLOOR);
    assert(jelom->x == 2 && jelom->y == 6);
    assert(lucia->scene == GENERATED_SCENE_REDWYNE_KITCHEN);
    assert(lucia->x == 4 && lucia->y == 3);
    assert(silverhair->scene == GENERATED_SCENE_REDWYNE_SILVERHAIR_ROOM);
    assert(sarin->scene == GENERATED_SCENE_REDWYNE_SILVERHAIR_ROOM);
    assert(janesta->scene == GENERATED_SCENE_REDWYNE_SILVERHAIR_ROOM);
    assert(talan->dialogue == GENERATED_DIALOGUE_TALAN_START);
    if (talan->x != 11)
        fprintf(stderr, "Talan gate patrol mismatch: x=%d y=%d step=%d wait=%d moving=%d\n",
                talan->x, talan->y, talan->schedule_step,
                talan->schedule_wait, talan->moving);
    assert(talan->x == 11);
    assert(talan->y == 17 || talan->y == 18 || talan->y == 19);

    /* The three prisoners retain their distinct reference routines: Imoen
     * alternates between her place and the window, Sarin chooses a room edge,
     * and Janesta chooses among her four housekeeping points. */
    assert(generated_runtime_schedules[GENERATED_SCHEDULE_SILVERHAIR_ROOM]
               .ambient_speech_count == 4);
    assert(generated_runtime_schedules[GENERATED_SCHEDULE_SILVERHAIR_ROOM]
               .step_count == 3);
    assert(generated_runtime_schedules[GENERATED_SCHEDULE_SARIN_ROOM]
               .branch_count == 4);
    assert(generated_runtime_schedules[GENERATED_SCHEDULE_JANESTA_ROOM]
               .branch_count == 4);

    world.scene_index = GENERATED_SCENE_REDWYNE_SILVERHAIR_ROOM;
    world.scene = &generated_runtime_scenes[world.scene_index];
    world.player_entity = 0;
    world.player_x = 5;
    world.player_y = 2;
    sarin->schedule = -1;
    janesta->schedule = -1;
    silverhair->x = 4;
    silverhair->y = 4;
    silverhair->moving = 0;
    silverhair->schedule = GENERATED_SCHEDULE_SILVERHAIR_ROOM;
    silverhair->schedule_step = 0;
    silverhair->schedule_wait = 0;
    for (frame = 0; frame < 200; ++frame) {
        tick(&world, 1);
        if (silverhair->x == 6 && silverhair->y == 4 &&
            silverhair->speech_line >= 0)
            break;
    }
    assert(frame < 200);
    assert(silverhair->facing == WORLD_EAST);
    silverhair->schedule = -1;
    silverhair->scene = -1;

    janesta->scene = GENERATED_SCENE_REDWYNE_SILVERHAIR_ROOM;
    janesta->x = 6;
    janesta->y = 3;
    janesta->moving = 0;
    janesta->schedule = GENERATED_SCHEDULE_JANESTA_ROOM;
    janesta->schedule_step =
        generated_runtime_schedules[GENERATED_SCHEDULE_JANESTA_ROOM].branches[0];
    janesta->schedule_wait = 0;
    for (frame = 0; frame < 300; ++frame) {
        tick(&world, 1);
        if (janesta->x == 1 && janesta->y == 3 && !janesta->moving)
            break;
    }
    assert(frame < 300);
    tick(&world, 1);
    assert(janesta->facing == WORLD_NORTH);
    janesta->schedule = -1;
    janesta->scene = -1;

    world.player_x = 7;
    world.player_y = 7;
    sarin->scene = GENERATED_SCENE_REDWYNE_SILVERHAIR_ROOM;
    sarin->x = 5;
    sarin->y = 3;
    sarin->moving = 0;
    sarin->schedule = GENERATED_SCHEDULE_SARIN_ROOM;
    sarin->schedule_step =
        generated_runtime_schedules[GENERATED_SCHEDULE_SARIN_ROOM].branches[0];
    sarin->schedule_wait = 0;
    for (frame = 0; frame < 120; ++frame) {
        tick(&world, 1);
        if (sarin->x == 5 && sarin->y == 2 && !sarin->moving)
            break;
    }
    assert(frame < 120);
    tick(&world, 1);
    assert(sarin->facing == WORLD_SOUTH);
    sarin->schedule = -1;
    sarin->scene = -1;

    world.scene_index = GENERATED_SCENE_REDWYNE_EXTERIOR;
    world.scene = &generated_runtime_scenes[world.scene_index];
    world.player_entity = 0;

    /* Ambient remarks are schedule data, appear without entering modal
     * dialogue, and expire independently of the NPC's movement route. */
    world.dialogue = -1;
    alek->speech_wait = 0;
    tick(&world, 1);
    assert(world.dialogue < 0);
    assert(alek->speech_line >= 0);
    assert(world_entity_speech(&world, 0) == 0); /* scene entity 0 is the player */
    for (frame = 0; frame < 180; ++frame)
        tick(&world, 1);
    assert(alek->speech_line < 0);

    /* Conditional choices expose only entries whose compiled flag comparison
     * currently matches, while preserving their authored visible order. */
    talan_dialogue = &generated_runtime_dialogues[GENERATED_DIALOGUE_TALAN_START];
    for (node_index = 0; node_index < talan_dialogue->node_count; ++node_index) {
        if (strcmp(talan_dialogue->nodes[node_index].text,
                   "According to Jelom there is no doubt that Lady Silverhair is the thief.") == 0) {
            jelom_node = &talan_dialogue->nodes[node_index];
            break;
        }
    }
    assert(jelom_node != 0);
    world.dialogue = GENERATED_DIALOGUE_TALAN_START;
    world.dialogue_node = node_index;
    world.dialogue_choice = 0;
    assert(world_dialogue_choice_count(&world) == 1);
    assert(strcmp(world_dialogue_choice_at(&world, 0),
                  "Who is Jelom? Your superiour?") == 0);
    assert(single_condition_flag(&jelom_node->choices[0]) >= 0);
    world.flags[single_condition_flag(&jelom_node->choices[0])] = 1;
    assert(world_dialogue_choice_count(&world) == 1);
    assert(strcmp(world_dialogue_choice_at(&world, 0),
                  "Ah, Jelom said that, did he? What else did he say?") == 0);
    world.flags[single_condition_flag(&jelom_node->choices[0])] = 0;
    world.dialogue = -1;

    /* Facts learned in the converted Oliver and Alek conversations use the
     * same compiled flag IDs as Jelom's evidence-gated questions. */
    jelom_dialogue = &generated_runtime_dialogues[GENERATED_DIALOGUE_JELOM_START];
    for (node_index = 0; node_index < jelom_dialogue->node_count; ++node_index) {
        if (strcmp(jelom_dialogue->nodes[node_index].text,
                   "And what might those questions be, Half-Elf?") == 0) {
            jelom_question_node = &jelom_dialogue->nodes[node_index];
            break;
        }
    }
    assert(jelom_question_node != 0);
    assert(jelom_question_node->choice_count == 6);
    assert(jelom_question_node->choices[0].condition_count == 2);
    assert(jelom_question_node->choices[0].conditions[0].flag >= 0);
    assert(single_condition_flag(&jelom_question_node->choices[1]) >= 0);
    assert(single_condition_flag(&jelom_question_node->choices[2]) >= 0);
    assert(single_condition_flag(&jelom_question_node->choices[3]) >= 0);
    world.dialogue = GENERATED_DIALOGUE_JELOM_START;
    world.dialogue_node = node_index;
    assert(world_dialogue_choice_count(&world) == 2);
    world.flags[single_condition_flag(&jelom_question_node->choices[1])] = 1;
    world.flags[single_condition_flag(&jelom_question_node->choices[2])] = 1;
    assert(world_dialogue_choice_count(&world) == 4);
    world.flags[single_condition_flag(&jelom_question_node->choices[3])] = 1;
    assert(world_dialogue_choice_count(&world) == 5);

    /* Jelom only accepts Talan's absence after Oliver's report has already
     * made him doubt his guard. This is the first authored all-of condition. */
    world.flags[jelom_question_node->choices[0].conditions[0].flag] = 2;
    assert(world_dialogue_choice_count(&world) == 5);
    world.flags[jelom_question_node->choices[0].conditions[1].flag] = 1;
    assert(world_dialogue_choice_count(&world) == 6);

    oliver_dialogue = &generated_runtime_dialogues[GENERATED_DIALOGUE_OLIVER_START];
    for (node_index = 0; node_index < oliver_dialogue->node_count; ++node_index) {
        const struct generated_runtime_dialogue_node *node = &oliver_dialogue->nodes[node_index];
        int choice_index;
        for (choice_index = 0; choice_index < node->choice_count; ++choice_index) {
            const struct generated_runtime_dialogue_choice *choice = &node->choices[choice_index];
            if (strstr(choice->text, "you heard something that night?") != 0 ||
                strstr(choice->text, "you noticed something that night?") != 0) {
                assert(choice->action_count == 1);
                assert(choice->actions[0].flag ==
                       single_condition_flag(&jelom_question_node->choices[1]));
                ++oliver_fact_bindings;
            }
        }
    }
    assert(oliver_fact_bindings == 2);

    for (node_index = 0;
         node_index < generated_runtime_dialogues[GENERATED_DIALOGUE_ALEK_START].node_count;
         ++node_index) {
        const struct generated_runtime_dialogue_node *node =
            &generated_runtime_dialogues[GENERATED_DIALOGUE_ALEK_START].nodes[node_index];
        int choice_index;
        for (choice_index = 0; choice_index < node->choice_count; ++choice_index) {
            const struct generated_runtime_dialogue_choice *choice = &node->choices[choice_index];
            if (strstr(choice->text, "outside the parlour during negotiations") != 0) {
                assert(choice->action_count == 1);
                assert(choice->actions[0].flag ==
                       single_condition_flag(&jelom_question_node->choices[2]));
                ++alek_fact_bindings;
            }
            if (strstr(choice->text, "so good in finding stuff out") != 0 &&
                choice->action_count == 1) {
                assert(choice->actions[0].flag ==
                       single_condition_flag(&jelom_question_node->choices[3]));
                assert(choice->actions[0].value == 1);
                ++packed_fact_bindings;
            }
        }
    }
    assert(alek_fact_bindings == 1);
    assert(packed_fact_bindings == 1);

    for (node_index = 0;
         node_index < generated_runtime_dialogues[GENERATED_DIALOGUE_EREK_START].node_count;
         ++node_index) {
        const struct generated_runtime_dialogue_node *node =
            &generated_runtime_dialogues[GENERATED_DIALOGUE_EREK_START].nodes[node_index];
        int choice_index;
        for (choice_index = 0; choice_index < node->choice_count; ++choice_index) {
            const struct generated_runtime_dialogue_choice *choice = &node->choices[choice_index];
            if (strstr(choice->text, "Did you pack Master Fingolson's gems") != 0) {
                assert(single_condition_flag(choice) ==
                       single_condition_flag(&jelom_question_node->choices[3]));
                assert(single_condition_value(choice) == 1);
                assert(choice->action_count == 1);
                assert(choice->actions[0].flag == single_condition_flag(choice));
                assert(choice->actions[0].value == 2);
                ++packed_followups;
            }
        }
    }
    assert(packed_followups == 1);

    /* The same two discovered facts unlock the original final rebuttal and
     * selecting it releases Silverhair for subsequent conversations. */
    for (node_index = 0; node_index < jelom_dialogue->node_count; ++node_index) {
        if (strstr(jelom_dialogue->nodes[node_index].text,
                   "how do you know that it was not Silverhair herself") != 0) {
            jelom_suspicion_node = &jelom_dialogue->nodes[node_index];
            break;
        }
    }
    assert(jelom_suspicion_node != 0);
    assert(jelom_suspicion_node->choice_count == 2);
    assert(jelom_suspicion_node->choices[1].condition_count == 2);
    world.dialogue_node = node_index;
    world.dialogue_choice = 1;
    assert(world_dialogue_choice_count(&world) == 2);
    world_update(&world, &interact);
    assert(strcmp(world_dialogue_text(&world),
                  "Hm ... yes, that's a fact. You are right, Half-Elf. Something strange is going on.") == 0);
    assert(jelom_suspicion_node->choices[1].action_count == 1);
    assert(world.flags[jelom_suspicion_node->choices[1].actions[0].flag] == 1);
    assert(jelom_dialogue->starts[0].flag ==
           jelom_suspicion_node->choices[1].actions[0].flag);
    world.dialogue = -1;

    /* The introductory gate dialogue changes Talan's persistent interaction
     * to his ordinary conversation before control is restored. */
    world.player_x = talan->x;
    world.player_y = talan->y + 1;
    world.player_facing = WORLD_NORTH;
    world.moving = 0;
    world_update(&world, &interact);
    assert(world.dialogue == GENERATED_DIALOGUE_TALAN_START);
    assert(strcmp(world_dialogue_text(&world),
                  "Listen, I really am sorry about that fuss at the gate. "
                  "I hope you will not hold it against me.") == 0);
    world_update(&world, &interact);
    world.dialogue = -1;
    world.dialogue_actor = -1;
    world_update(&world, &interact);
    assert(strcmp(world_dialogue_text(&world), "Oh, hullo again, Banec.") == 0);
    world.dialogue = -1;
    world.dialogue_actor = -1;
    world.player_x = 0;
    world.player_y = 0;

    for (frame = 0; frame < 2400; ++frame) {
        tick(&world, 1);
        if (tristan->scene == GENERATED_SCENE_REDWYNE_EXTERIOR &&
            tristan->x == 12 && tristan->y == 18 && !tristan->moving)
            break;
    }
    if (frame >= 2400)
        fprintf(stderr, "Tristan outward route stuck: scene=%d x=%d y=%d step=%d wait=%d moving=%d\n",
                tristan->scene, tristan->x, tristan->y, tristan->schedule_step,
                tristan->schedule_wait, tristan->moving);
    assert(frame < 2400);

    /* Put the player adjacent to the scheduled NPC. Talking turns Tristan
     * toward the player and freezes only Tristan's route until the dialogue
     * closes. */
    world.player_x = tristan->x;
    world.player_y = tristan->y + 1;
    world.player_facing = WORLD_NORTH;
    world.moving = 0;
    world_update(&world, &interact);
    assert(world.dialogue >= 0);
    assert(world.dialogue_actor == GENERATED_ENTITY_TRISTAN);
    assert(tristan->facing == WORLD_SOUTH);
    frozen_x = tristan->x;
    frozen_y = tristan->y;
    frozen_progress = tristan->move_progress;
    tick(&world, 30);
    assert(tristan->x == frozen_x && tristan->y == frozen_y);
    assert(tristan->move_progress == frozen_progress);

    /* Closing through the public input path releases the route. */
    for (frame = 0; frame < 64 && world.dialogue >= 0; ++frame)
        world_update(&world, &interact);
    assert(world.dialogue < 0);
    assert(world.dialogue_actor < 0);
    world.player_x = 0;
    world.player_y = 0;

    for (frame = 0; frame < 3000; ++frame) {
        tick(&world, 1);
        if (tristan->scene == GENERATED_SCENE_REDWYNE_INN &&
            tristan->x == 4 && tristan->y == 6 && !tristan->moving)
            break;
    }
    if (frame >= 3000)
        fprintf(stderr, "Tristan stuck: scene=%d x=%d y=%d step=%d wait=%d moving=%d\n",
                tristan->scene, tristan->x, tristan->y, tristan->schedule_step,
                tristan->schedule_wait, tristan->moving);
    assert(frame < 3000);

    /* Orloth follows the reference bar/clock/table route. The authored goals
     * are direct, as in set_goal(); A* resolves the bar's one-way edges. */
    talan->schedule = -1;
    tristan->schedule = -1;
    alek->schedule = -1;
    erek->schedule = -1;
    oliver->schedule = -1;
    frostbloom->schedule = -1;
    jelom->schedule = -1;

    /* As in Adonthell mapsquare::is_free(), only a character's anchor square
     * is occupied. Its upper sprite square must remain traversable. */
    orloth->schedule = -1;
    orloth->scene = GENERATED_SCENE_REDWYNE_INN;
    orloth->x = 4;
    orloth->y = 5;
    orloth->moving = 0;
    world.dialogue = -1;
    world.active_script = -1;
    world.control_enabled = 1;
    world.scene_index = GENERATED_SCENE_REDWYNE_INN;
    world.scene = &generated_runtime_scenes[world.scene_index];
    world.player_entity = 0;
    world.player_x = 4;
    world.player_y = 3;
    world.moving = 0;
    world_update(&world, &move_south);
    if (!world.moving)
        fprintf(stderr, "Player could not enter NPC upper tile: scene=%d player=(%d,%d) "
                        "dialogue=%d script=%d control=%d orloth=(%d,%d,%d)\n",
                world.scene_index, world.player_x, world.player_y, world.dialogue,
                world.active_script, world.control_enabled,
                orloth->scene, orloth->x, orloth->y);
    assert(world.moving);
    tick(&world, 10);
    assert(world.player_x == 4 && world.player_y == 4);
    world_update(&world, &move_south);
    assert(!world.moving);
    assert(world.player_x == 4 && world.player_y == 4);

    /* The common room's southern wall blocks the ground anchor at row 7.
     * Its translucent face is only a visual overlay; it must not require the
     * player to enter the out-of-bounds row before collision takes effect. */
    world.player_x = 6;
    world.player_y = 7;
    world.moving = 0;
    world_update(&world, &move_south);
    assert(!world.moving);
    assert(world.player_x == 6 && world.player_y == 7);

    world.player_x = 14;
    world.player_y = 8;
    orloth->scene = GENERATED_SCENE_REDWYNE_INN;
    orloth->x = 2;
    orloth->y = 2;
    orloth->moving = 0;
    orloth->move_progress = 0;
    orloth->schedule = GENERATED_SCHEDULE_ORLOTH_COMMON_ROOM;
    assert(generated_runtime_schedules[GENERATED_SCHEDULE_ORLOTH_COMMON_ROOM]
               .branch_count == 5);
    orloth->schedule_step = 1;
    orloth->schedule_wait = 0;
    world.schedule_random = 1;
    tick(&world, 1);
    first_random_step = orloth->schedule_step;
    orloth->schedule_step = 1;
    orloth->schedule_wait = 0;
    world.schedule_random = 2;
    tick(&world, 1);
    assert(orloth->schedule_step != first_random_step);
    orloth->schedule_step =
        generated_runtime_schedules[GENERATED_SCHEDULE_ORLOTH_COMMON_ROOM].branches[0];
    orloth->schedule_wait = 0;
    orloth->speech_line = -1;
    orloth->speech_frames = 0;
    orloth->speech_wait = 999999;
    for (frame = 0; frame < 2000; ++frame) {
        tick(&world, 1);
        if (orloth->x == 10 && orloth->y == 3 && !orloth->moving)
            break;
    }
    assert(frame < 2000);
    tick(&world, 2);
    assert(orloth->speech_line ==
           generated_runtime_schedules[GENERATED_SCHEDULE_ORLOTH_COMMON_ROOM]
               .ambient_speech_count);
    assert(orloth->speech_frames > 0);
    /* Orloth changes each table in two stages after one- and two-tick waits.
     * The mug slots begin hidden and retain their state even while their scene
     * is not the one currently displayed. */
    orloth->x = 2;
    orloth->y = 2;
    orloth->moving = 0;
    orloth->schedule_step =
        generated_runtime_schedules[GENERATED_SCHEDULE_ORLOTH_COMMON_ROOM].branches[1];
    orloth->schedule_wait = 0;
    for (frame = 0; frame < 2000; ++frame) {
        tick(&world, 1);
        if (orloth->x == 3 && orloth->y == 5 && !orloth->moving)
            break;
    }
    assert(frame < 2000);
    tick(&world, 1);  /* finish move */
    tick(&world, 1);  /* start first one-tick wait */
    tick(&world, 60);
    assert(count_object_state(&world, GENERATED_ENTITY_TABLE_MUG, 0) == 0);
    tick(&world, 1);
    assert(count_object_state(&world, GENERATED_ENTITY_TABLE_MUG, 0) == 1);
    tick(&world, 1);  /* start second one-tick wait */
    tick(&world, 60);
    tick(&world, 1);
    assert(count_object_state(&world, GENERATED_ENTITY_TABLE_MUG, 0) == 2);

    for (frame = 0; frame < 3000; ++frame) {
        orloth->schedule_wait = 0;
        tick(&world, 1);
        if (orloth->schedule_step == 0 && orloth->x == 2 && orloth->y == 2 &&
            !orloth->moving)
            break;
    }
    assert(frame < 3000);
    for (branch_index = 2; branch_index < 5; ++branch_index) {
        orloth->schedule_step =
            generated_runtime_schedules[GENERATED_SCHEDULE_ORLOTH_COMMON_ROOM]
                .branches[branch_index];
        for (frame = 0; frame < 3000; ++frame) {
            orloth->schedule_wait = 0;
            tick(&world, 1);
            if (orloth->schedule_step == 0 && orloth->x == 2 && orloth->y == 2 &&
                !orloth->moving)
                break;
        }
        assert(frame < 3000);
    }
    assert(count_object_state(&world, GENERATED_ENTITY_TABLE_MUG, 0) == 8);
    for (branch_index = 1; branch_index < 5; ++branch_index) {
        orloth->schedule_step =
            generated_runtime_schedules[GENERATED_SCHEDULE_ORLOTH_COMMON_ROOM]
                .branches[branch_index];
        for (frame = 0; frame < 3000; ++frame) {
            orloth->schedule_wait = 0;
            tick(&world, 1);
            if (orloth->schedule_step == 0 && orloth->x == 2 && orloth->y == 2 &&
                !orloth->moving)
                break;
        }
        assert(frame < 3000);
    }
    assert(count_object_state(&world, GENERATED_ENTITY_TABLE_MUG, 1) == 8);

    /* Alek traverses the original bar/table route in both directions. */
    orloth->scene = GENERATED_SCENE_REDWYNE_EXTERIOR;
    tristan->scene = GENERATED_SCENE_REDWYNE_EXTERIOR;
    erek->scene = GENERATED_SCENE_REDWYNE_EXTERIOR;
    alek->scene = GENERATED_SCENE_REDWYNE_INN;
    alek->x = 1;
    alek->y = 3;
    alek->moving = 0;
    alek->move_progress = 0;
    alek->schedule = GENERATED_SCHEDULE_ALEK_BAR_AND_TABLE;
    alek->schedule_step = 0;
    alek->schedule_wait = 0;
    for (frame = 0; frame < 1000; ++frame) {
        tick(&world, 1);
        if (alek->x == 12 && alek->y == 5 && !alek->moving)
            break;
    }
    if (frame >= 1000)
        fprintf(stderr, "Alek outward route stuck: x=%d y=%d step=%d moving=%d\n",
                alek->x, alek->y, alek->schedule_step, alek->moving);
    assert(frame < 1000);
    tick(&world, 1);
    alek->schedule_wait = 0;
    for (frame = 0; frame < 1000; ++frame) {
        tick(&world, 1);
        if (alek->x == 1 && alek->y == 3 && !alek->moving)
            break;
    }
    if (frame >= 1000) {
        fprintf(stderr, "Alek return route stuck: x=%d y=%d step=%d moving=%d\n",
                alek->x, alek->y, alek->schedule_step, alek->moving);
    }
    assert(frame < 1000);
    alek->schedule = -1;
    orloth->scene = GENERATED_SCENE_REDWYNE_INN;
    erek->scene = GENERATED_SCENE_REDWYNE_INN;

    /* Rhayne randomly chooses among collision-safe points inside the same yard
     * bounds as the reference random walk. */
    frostbloom->scene = GENERATED_SCENE_REDWYNE_EXTERIOR;
    frostbloom->x = 18;
    frostbloom->y = 22;
    frostbloom->moving = 0;
    frostbloom->move_progress = 0;
    frostbloom->schedule = GENERATED_SCHEDULE_FROSTBLOOM_YARD;
    frostbloom->schedule_step = 0;
    frostbloom->schedule_wait = 0;
    assert(generated_runtime_schedules[GENERATED_SCHEDULE_FROSTBLOOM_YARD]
               .branch_count == 4);
    for (frame = 0; frame < 5000; ++frame) {
        frostbloom->schedule_wait = 0;
        tick(&world, 1);
        if (frame > 0 && !frostbloom->moving && frostbloom->schedule_step == 0 &&
            ((frostbloom->x == 24 || frostbloom->x == 16) &&
             (frostbloom->y == 22 || frostbloom->y == 28)))
            break;
    }
    if (frame >= 5000)
        fprintf(stderr, "Frostbloom route stuck: x=%d y=%d step=%d moving=%d\n",
                frostbloom->x, frostbloom->y, frostbloom->schedule_step,
                frostbloom->moving);
    assert(frame < 5000);
    frostbloom->schedule = -1;

    /* Erek now follows the common-room/parlour half of his original route.
     * Explicit door placements stand in for the source engine's teleport
     * events while preserving his actor state and ambient speech. */
    talan->scene = GENERATED_SCENE_REDWYNE_EXTERIOR;
    tristan->scene = GENERATED_SCENE_REDWYNE_EXTERIOR;
    orloth->scene = GENERATED_SCENE_REDWYNE_EXTERIOR;
    alek->scene = GENERATED_SCENE_REDWYNE_EXTERIOR;
    oliver->scene = GENERATED_SCENE_REDWYNE_EXTERIOR;
    erek->scene = GENERATED_SCENE_REDWYNE_INN;
    erek->x = 5;
    erek->y = 5;
    erek->moving = 0;
    erek->move_progress = 0;
    erek->schedule = GENERATED_SCHEDULE_EREK_COMMON_ROOM;
    erek->schedule_step = 0;
    erek->schedule_wait = 0;
    for (frame = 0; frame < 5000; ++frame) {
        erek->schedule_wait = 0;
        tick(&world, 1);
        if (erek->scene == GENERATED_SCENE_REDWYNE_PARLOUR)
            break;
    }
    if (frame >= 5000)
        fprintf(stderr, "Erek failed to enter parlour: scene=%d x=%d y=%d step=%d moving=%d\n",
                erek->scene, erek->x, erek->y, erek->schedule_step, erek->moving);
    assert(frame < 5000);
    for (frame = 0; frame < 10000; ++frame) {
        erek->schedule_wait = 0;
        tick(&world, 1);
        if (erek->scene == GENERATED_SCENE_REDWYNE_INN &&
            ((erek->x == 5 && erek->y == 5) ||
             (erek->x == 10 && erek->y == 6)) && !erek->moving)
            break;
    }
    if (frame >= 10000)
        fprintf(stderr, "Erek route stuck: scene=%d x=%d y=%d step=%d moving=%d\n",
                erek->scene, erek->x, erek->y, erek->schedule_step, erek->moving);
    assert(frame < 10000);
    erek->schedule = -1;
    orloth->scene = GENERATED_SCENE_REDWYNE_INN;
    orloth->x = 2;
    orloth->y = 2;
    alek->scene = GENERATED_SCENE_REDWYNE_INN;
    alek->x = 1;
    alek->y = 3;
    erek->scene = GENERATED_SCENE_REDWYNE_INN;
    erek->x = 5;
    erek->y = 5;

    world.active_script = GENERATED_SCRIPT_ENTER_REDWYNE_PARLOUR;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_PARLOUR);
    assert(world.player_x == 1 && world.player_y == 4);
    assert(world.player_facing == WORLD_EAST);
    world.active_script = GENERATED_SCRIPT_LEAVE_REDWYNE_PARLOUR;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_INN);
    assert(world.player_x == 13 && world.player_y == 4);
    assert(world.player_facing == WORLD_WEST);

    /* First contact uses the introduction; setting its persistent flag makes
     * the next interaction begin with Orloth's ordinary inquiry. */
    orloth->schedule = -1;
    world.active_script = GENERATED_SCRIPT_ENTER_REDWYNE_INN;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_INN);
    world.player_x = orloth->x;
    world.player_y = orloth->y + 1;
    world.player_facing = WORLD_NORTH;
    world.moving = 0;
    world_update(&world, &interact);
    assert(world.dialogue == GENERATED_DIALOGUE_ORLOTH_START);
    assert(strcmp(world_dialogue_text(&world),
                  "This appears to be the Innkeeper and owner of the place. "
                  "He seems lost in thought and it looks as if he hasn't slept much lately. "
                  "His worried face brightens up as you approach though, and his voice is warm and friendly:") == 0);
    world_update(&world, &interact);
    world_update(&world, &interact);
    world_update(&world, &interact);
    assert(world_dialogue_choice_count(&world) == 1);
    world_update(&world, &interact);
    assert(strcmp(world_dialogue_text(&world), "'Tis a shame, really. You'd think you were "
                  "in danger on the road, with all those bandits about. But no, right here, "
                  "despite the guards, a mans belongings aren't safe from filthy thieves.") == 0);
    world.dialogue = -1;
    world.dialogue_actor = -1;
    world_update(&world, &interact);
    assert(strcmp(world_dialogue_text(&world), "Ah, any news about the thief, Banec?") == 0);

    /* The remaining common-room occupants use their decoded default anchors
     * and persistent first/repeat dialogue entries. */
    world.dialogue = -1;
    world.dialogue_actor = -1;
    world.player_x = alek->x;
    world.player_y = alek->y + 1;
    world.player_facing = WORLD_NORTH;
    world_update(&world, &interact);
    assert(world.dialogue == GENERATED_DIALOGUE_ALEK_START);
    assert(strcmp(world_dialogue_text(&world),
                  "The muscular fellow in front of you has the air of a troublemaker about him. "
                  "Scars all over his body are evidence of his readiness to use the sword he is carrying. "
                  "He eyes you with undisguised distaste as you approach.") == 0);
    world.dialogue = -1;
    world.dialogue_actor = -1;
    world.player_x = erek->x;
    world.player_y = erek->y + 1;
    world.player_facing = WORLD_NORTH;
    world_update(&world, &interact);
    assert(world.dialogue == GENERATED_DIALOGUE_EREK_START);
    assert(strcmp(world_dialogue_text(&world),
                  "Before you stands a young Dwarf, with an open face and no sign of the traditional beard. "
                  "His expression shows his intelligence and a trace of humour as he watches you expectantly.") == 0);
    assert(strcmp(generated_runtime_dialogues[GENERATED_DIALOGUE_OLIVER_START].nodes[0].text,
                  "As he notices you, the boy stops whatever he's been doing. With a cheerful grin on his face, "
                  "he turns towards you.") == 0);
    oliver_dialogue = &generated_runtime_dialogues[GENERATED_DIALOGUE_OLIVER_START];
    for (node_index = 0; node_index < oliver_dialogue->node_count; ++node_index)
        if (strcmp(oliver_dialogue->nodes[node_index].text,
                   "What would you like to know, sir?") == 0)
            break;
    assert(node_index < oliver_dialogue->node_count);
    world.dialogue = GENERATED_DIALOGUE_OLIVER_START;
    world.dialogue_node = node_index;
    world.dialogue_choice = 0;
    for (frame = 0; frame < GENERATED_RUNTIME_DIALOGUE_USED_COUNT; ++frame)
        world.dialogue_used[frame] = 0;
    assert(world_dialogue_choice_count(&world) == 3);
    world_update(&world, &interact);
    world.dialogue_node = node_index;
    assert(world_dialogue_choice_count(&world) == 2);
    assert(strcmp(world_dialogue_choice_at(&world, 0),
                  "Did something unusual happen here lately, Oliver?") == 0);
    world.dialogue = -1;
    world.dialogue_actor = -1;
    assert(strcmp(generated_runtime_dialogues[GENERATED_DIALOGUE_FROSTBLOOM_START].nodes[0].text,
                  "Hm? What do you want?") == 0);

    /* Later Silverhair questions are unlocked by the same authored evidence
     * flags that the source conversations establish. This guards the complete
     * content chain rather than merely checking that the lines compiled. */
    {
        const struct generated_runtime_dialogue *frostbloom_dialogue =
            &generated_runtime_dialogues[GENERATED_DIALOGUE_FROSTBLOOM_START];
        const struct generated_runtime_dialogue *erek_dialogue =
            &generated_runtime_dialogues[GENERATED_DIALOGUE_EREK_START];
        const struct generated_runtime_dialogue *silverhair_dialogue =
            &generated_runtime_dialogues[GENERATED_DIALOGUE_SILVERHAIR_START];
        const struct generated_runtime_dialogue_node *frostbloom_first =
            find_dialogue_node(frostbloom_dialogue, "Hm? What do you want?");
        const struct generated_runtime_dialogue_node *erek_menu =
            find_dialogue_node(erek_dialogue, "Ho there, Banec. What is it this time?");
        const struct generated_runtime_dialogue_node *erek_uncertain =
            find_dialogue_node(
                erek_dialogue,
                "Master Fingolson says her guilt is proven beyond doubt, and there are indeed facts that support his opinion. But I ... am not sure.");
        const struct generated_runtime_dialogue_node *silverhair_menu =
            find_dialogue_node(silverhair_dialogue,
                               "I have little choice. Do you have news? Or questions?");
        const struct generated_runtime_dialogue_choice *meet_frostbloom;
        const struct generated_runtime_dialogue_choice *erek_argument;
        const struct generated_runtime_dialogue_choice *erek_first_argument;
        const struct generated_runtime_dialogue_choice *erek_known_argument;
        const struct generated_runtime_dialogue_choice *silverhair_yeti;
        const struct generated_runtime_dialogue_choice *silverhair_argument;
        const struct generated_runtime_dialogue_choice *silverhair_noise;
        const struct generated_runtime_dialogue_node *frostbloom_second;
        const struct generated_runtime_dialogue_node *frostbloom_post_yeti;
        const struct generated_runtime_dialogue_node *frostbloom_flattery;
        const struct generated_runtime_dialogue_choice *frostbloom_yeti;
        const struct generated_runtime_dialogue_choice *honest_flattery;
        int know_frostbloom_flag;
        int know_argument_flag;
        int talked_about_yeti_flag;
        int frostbloom_talked_flag;
        int frostbloom_flattered_flag;

        assert(frostbloom_first != 0 && erek_menu != 0 && erek_uncertain != 0 &&
               silverhair_menu != 0);
        meet_frostbloom = find_dialogue_choice(
            frostbloom_first,
            "I am a servant of Lady Imoen Silverhair, and I'd like to ask ...");
        erek_argument = find_dialogue_choice(
            erek_menu,
            "Could you please repeat what you told me about the argument between your master and Lady Silverhair?");
        erek_first_argument = find_dialogue_choice(
            erek_uncertain,
            "I know of nothing that could possibly support such claims. I really would know what has happened!");
        erek_known_argument = find_dialogue_choice(
            erek_uncertain,
            "You refer to the argument she had with your master?");
        silverhair_yeti = find_dialogue_choice(
            silverhair_menu,
            "I am curious, Lady. What of that figurine there? I do not recall it.");
        silverhair_argument = find_dialogue_choice(
            silverhair_menu,
            "Lady, could you tell me more about the argument you had with the Dwarf? It may be helpful.");
        silverhair_noise = find_dialogue_choice(
            silverhair_menu,
            "I have spoken with the artist, Frostbloom. She mentioned that she had heard some strange sounds in the night, when the theft must have taken place. Did you hear anything?");
        assert(meet_frostbloom != 0 && meet_frostbloom->action_count == 2);
        assert(erek_argument != 0 && erek_argument->action_count == 2);
        assert(erek_first_argument != 0 && erek_known_argument != 0);
        assert(silverhair_yeti != 0 && silverhair_yeti->condition_count == 2);
        assert(silverhair_argument != 0 && silverhair_argument->condition_count == 1);
        assert(silverhair_noise != 0 && silverhair_noise->condition_count == 2);

        know_frostbloom_flag = meet_frostbloom->actions[1].flag;
        know_argument_flag = silverhair_argument->conditions[0].flag;
        assert(choice_has_condition(silverhair_yeti, know_frostbloom_flag, 1));
        assert(choice_has_action(erek_argument, know_argument_flag, 1));
        assert(single_condition_flag(erek_first_argument) == know_argument_flag);
        assert(single_condition_value(erek_first_argument) == 0);
        assert(choice_has_action(erek_first_argument, know_argument_flag, 1));
        assert(single_condition_flag(erek_known_argument) == know_argument_flag);
        assert(single_condition_value(erek_known_argument) == 1);
        assert(choice_has_action(erek_known_argument, know_argument_flag, 1));

        /* Erek's repeat wording is only reachable after his first account of
         * the argument. The original branches its first-time prompt depending
         * on whether Banec learned about the argument elsewhere. */
        {
            struct world_state erek_world;
            world_initialize(&erek_world);
            erek_world.dialogue = GENERATED_DIALOGUE_EREK_START;
            erek_world.dialogue_node = (int)(erek_uncertain - erek_dialogue->nodes);
            erek_world.dialogue_choice = 0;
            assert(world_dialogue_choice_count(&erek_world) == 1);
            assert(strcmp(world_dialogue_choice_at(&erek_world, 0),
                          "I know of nothing that could possibly support such claims. I really would know what has happened!") == 0);
            world_update(&erek_world, &interact);
            assert(erek_world.flags[know_argument_flag] == 1);
            assert(strcmp(world_dialogue_text(&erek_world),
                          "Very well then. It was yesterday, in the evening, when Lady Silverhair met my Master and me in the parlour next to the common room.") == 0);

            world_initialize(&erek_world);
            erek_world.dialogue = GENERATED_DIALOGUE_EREK_START;
            erek_world.dialogue_node = (int)(erek_uncertain - erek_dialogue->nodes);
            erek_world.dialogue_choice = 0;
            erek_world.flags[know_argument_flag] = 1;
            assert(world_dialogue_choice_count(&erek_world) == 1);
            assert(strcmp(world_dialogue_choice_at(&erek_world, 0),
                          "You refer to the argument she had with your master?") == 0);
        }

        /* Frostbloom cannot be praised for a figurine until Silverhair has
         * actually identified it. Her later investigation menu is likewise
         * behind the source dialogue's honesty test, not a visit counter. */
        frostbloom_second = find_dialogue_node(
            frostbloom_dialogue, "You again? I thought I told you to leave me alone.");
        frostbloom_post_yeti = find_dialogue_node(
            frostbloom_dialogue,
            "Back again, eh? You do hunger for punishment, don't you.");
        frostbloom_flattery = find_dialogue_node(
            frostbloom_dialogue,
            "False flattery, eh? I would wager my next commission you know nothing about the Yeti and their ways, yet would use this praise to loosen my tongue. Well, have I spoken truth?");
        assert(frostbloom_second != 0 && frostbloom_post_yeti != 0 &&
               frostbloom_flattery != 0);
        assert(find_dialogue_choice(
                   frostbloom_second,
                   "I have seen the lovely Yeti you carved for my mistress. It is quite impressive.") == 0);
        frostbloom_yeti = find_dialogue_choice(
            frostbloom_post_yeti,
            "I have seen the lovely Yeti you carved for my mistress. It is quite impressive.");
        honest_flattery = find_dialogue_choice(
            frostbloom_flattery,
            "My lady, you have a keen eye and a keener wit. That is precisely why I gave you praise, although you truly do deserve it.");
        assert(frostbloom_yeti != 0);
        assert(honest_flattery != 0 && honest_flattery->action_count == 1);

        talked_about_yeti_flag = silverhair_yeti->actions[0].flag;
        frostbloom_talked_flag = meet_frostbloom->actions[0].flag;
        frostbloom_flattered_flag = honest_flattery->actions[0].flag;
        assert(frostbloom_dialogue->start_count == 5);
        assert(frostbloom_dialogue->starts[0].flag == frostbloom_flattered_flag &&
               frostbloom_dialogue->starts[0].value == 2);
        assert(frostbloom_dialogue->starts[1].flag == frostbloom_flattered_flag &&
               frostbloom_dialogue->starts[1].value == 1);
        assert(frostbloom_dialogue->starts[2].flag == talked_about_yeti_flag &&
               frostbloom_dialogue->starts[2].value == 1);
        assert(frostbloom_dialogue->starts[3].flag == frostbloom_talked_flag &&
               frostbloom_dialogue->starts[3].value == 1);
        assert(frostbloom_dialogue->starts[4].flag == -1);
        assert(find_dialogue_node(silverhair_dialogue,
                                  "The lady looks anxiously at the door.") != 0);
        assert(find_dialogue_node(silverhair_dialogue,
                                  "Silverhair's eyes flash with remembered anger, but a scuffing sound by the door distracts her and her anger drains as suddenly as it rose.") != 0);
    }

    /* Sarin and Janesta were already scheduled room actors; their source
     * conversations must also be reachable from those same map entities. */
    {
        const struct generated_runtime_scene *room =
            &generated_runtime_scenes[GENERATED_SCENE_REDWYNE_SILVERHAIR_ROOM];
        const struct generated_runtime_dialogue *sarin_dialogue =
            &generated_runtime_dialogues[GENERATED_DIALOGUE_SARIN_START];
        const struct generated_runtime_dialogue *janesta_dialogue =
            &generated_runtime_dialogues[GENERATED_DIALOGUE_JANESTA_START];
        const struct generated_runtime_dialogue_node *sarin_truth =
            find_dialogue_node(
                sarin_dialogue,
                "The truth? The truth is that our Lady is innocent and that, that Dwarf ruffian, he's a liar.");
        const struct generated_runtime_dialogue_node *sarin_artist =
            find_dialogue_node(
                sarin_dialogue,
                "Nay, not a thing. An uncouth lot, save for the one. An artist, I think. Lady Silverhair had words with her when we arrived, but Janesta and I were too busy to learn her name.");
        const struct generated_runtime_dialogue_choice *true_thief;
        const struct generated_runtime_dialogue_choice *name_frostbloom;
        const struct generated_runtime_dialogue_choice *seek_artist;
        int sarin_interaction = -1;
        int janesta_interaction = -1;
        int entity_index;

        for (entity_index = 0; entity_index < room->entity_count; ++entity_index) {
            const struct generated_runtime_entity *entity = &room->entities[entity_index];
            if (entity->definition == GENERATED_ENTITY_SARIN)
                sarin_interaction = entity->interaction_dialogue;
            if (entity->definition == GENERATED_ENTITY_JANESTA)
                janesta_interaction = entity->interaction_dialogue;
        }
        assert(sarin_interaction == GENERATED_DIALOGUE_SARIN_START);
        assert(janesta_interaction == GENERATED_DIALOGUE_JANESTA_START);
        assert(strcmp(sarin_dialogue->nodes[0].text,
                      "What insolence! Banec, do you believe the depths of what they have done to our Lady? To accuse her of common theft, as if she was a human! And to lock her away in this tiny, dirty room! I tell you, I cannot bear it.") == 0);
        assert(sarin_truth != 0 && sarin_artist != 0);
        true_thief = find_dialogue_choice(
            sarin_truth,
            "He seems to be telling the truth about the theft. But I cannot believe that our Lady did the deed. There must be a true thief about.");
        name_frostbloom = find_dialogue_choice(
            sarin_artist,
            "Her name is Frostbloom. Not terribly friendly, but the Lady thinks highly of her talents. She went so far as to buy that figurine on the mantle.");
        seek_artist = find_dialogue_choice(
            sarin_artist,
            "I'll have to speak to her. Perhaps she may know something. Thank you, Sarin. I'm glad you are here to watch over the Lady during this trouble.");
        assert(true_thief != 0 && single_condition_value(true_thief) == 3);
        assert(name_frostbloom != 0 && seek_artist != 0);
        assert(single_condition_value(name_frostbloom) == 1);
        assert(single_condition_value(seek_artist) == 0);
        assert(single_condition_flag(name_frostbloom) ==
               single_condition_flag(seek_artist));
        assert(strcmp(janesta_dialogue->nodes[0].text,
                      "Oh, Banec, we've been so worried for the Mistress. Can't you do something to set her free?") == 0);
        assert(janesta_dialogue->nodes[0].choice_count == 2);
    }

    /* Only Orloth's room-offer endpoint launches the escort. The reference
     * switches Banec and Oliver directly into the room, then begins Oliver's
     * room-specific conversation. */
    world.dialogue = GENERATED_DIALOGUE_ORLOTH_START;
    world.dialogue_choice = 0;
    for (node_index = 0;
         node_index < generated_runtime_dialogues[GENERATED_DIALOGUE_ORLOTH_START].node_count;
         ++node_index) {
        if (strcmp(generated_runtime_dialogues[GENERATED_DIALOGUE_ORLOTH_START]
                       .nodes[node_index].text,
                   "The landlord calls for Oliver, the boy you met outside, and shortly "
                   "afterwards he enters the common room to show you to your chamber.") == 0)
            break;
    }
    assert(node_index < generated_runtime_dialogues[GENERATED_DIALOGUE_ORLOTH_START].node_count);
    world.dialogue_node = node_index;
    world_update(&world, &interact);
    assert(world.active_script == GENERATED_SCRIPT_ESCORT_TO_PLAYER_ROOM);
    tick(&world, 3);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_PLAYER_ROOM);
    assert(world.player_x == 5 && world.player_y == 3 && world.player_facing == WORLD_SOUTH);
    assert(oliver->scene == GENERATED_SCENE_REDWYNE_PLAYER_ROOM);
    assert(oliver->x == 5 && oliver->y == 4 && oliver->facing == WORLD_NORTH);
    assert(world.dialogue == GENERATED_DIALOGUE_OLIVER_ROOM);
    assert(strcmp(world_dialogue_text(&world),
                  "Oliver leads you to a room on the first floor.") == 0);

    /* Finishing Oliver's short room conversation restores his ordinary
     * dialogue and sends him through the bedroom, landing, common room and
     * yard. The one-shot route then chains back to his normal yard job. */
    for (node_index = 0;
         node_index < generated_runtime_dialogues[GENERATED_DIALOGUE_OLIVER_ROOM].node_count;
         ++node_index) {
        if (strcmp(generated_runtime_dialogues[GENERATED_DIALOGUE_OLIVER_ROOM]
                       .nodes[node_index].text,
                   "Don't worry Oliver. This one is fine.") == 0)
            break;
    }
    assert(node_index < generated_runtime_dialogues[GENERATED_DIALOGUE_OLIVER_ROOM].node_count);
    world.dialogue_node = node_index;
    world_update(&world, &interact);
    assert(world.active_script == GENERATED_SCRIPT_OLIVER_RETURN_TO_YARD);
    tick(&world, 2);
    assert(oliver->schedule == GENERATED_SCHEDULE_OLIVER_RETURN_TO_YARD);
    for (frame = 0; frame < 1000; ++frame) {
        tick(&world, 1);
        if (oliver->schedule == GENERATED_SCHEDULE_OLIVER_YARD)
            break;
    }
    if (frame >= 1000)
        fprintf(stderr, "Oliver return stuck: scene=%d x=%d y=%d step=%d moving=%d\n",
                oliver->scene, oliver->x, oliver->y,
                oliver->schedule_step, oliver->moving);
    assert(frame < 1000);
    assert(oliver->scene == GENERATED_SCENE_REDWYNE_EXTERIOR);
    assert(oliver->x == 25 && oliver->y == 15);
    assert(oliver->dialogue == GENERATED_DIALOGUE_OLIVER_START);

    /* The converted landing connects the common room and player bedroom in
     * both directions through named Tiled spawn points. */
    world.active_script = GENERATED_SCRIPT_LEAVE_PLAYER_ROOM;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_FIRST_FLOOR);
    assert(world.player_x == 7 && world.player_y == 3);
    assert(world.player_facing == WORLD_NORTH);

    world.active_script = GENERATED_SCRIPT_ENTER_PLAYER_ROOM;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_PLAYER_ROOM);
    assert(world.player_x == 5 && world.player_y == 2);
    assert(world.player_facing == WORLD_SOUTH);

    world.active_script = GENERATED_SCRIPT_ENTER_FIRST_FLOOR;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_FIRST_FLOOR);
    assert(world.player_x == 8 && world.player_y == 2);
    assert(world.player_facing == WORLD_SOUTH);

    world.active_script = GENERATED_SCRIPT_LEAVE_FIRST_FLOOR;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_INN);
    assert(world.player_x == 12 && world.player_y == 2);
    assert(world.player_facing == WORLD_SOUTH);

    /* The kitchen retains its original doors and exact spawn squares. */
    world.active_script = GENERATED_SCRIPT_ENTER_REDWYNE_KITCHEN;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_KITCHEN);
    assert(world.player_x == 1 && world.player_y == 2);
    assert(world.player_facing == WORLD_SOUTH);

    world.active_script = GENERATED_SCRIPT_LEAVE_REDWYNE_KITCHEN;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_INN);
    assert(world.player_x == 1 && world.player_y == 7);
    assert(world.player_facing == WORLD_NORTH);

    world.active_script = GENERATED_SCRIPT_ENTER_REDWYNE_KITCHEN_YARD;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_KITCHEN);
    assert(world.player_x == 6 && world.player_y == 3);
    assert(world.player_facing == WORLD_WEST);

    world.active_script = GENERATED_SCRIPT_LEAVE_REDWYNE_KITCHEN_YARD;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_EXTERIOR);
    assert(world.player_x == 12 && world.player_y == 14);
    assert(world.player_facing == WORLD_EAST);

    /* The kitchen stairs now follow the reference submap-3/submap-4 pair.
     * The cellar preserves all eight directional positional remarks, including
     * the two labels inspected from opposite sides of the same square. */
    world.active_script = GENERATED_SCRIPT_ENTER_REDWYNE_CELLAR;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_CELLAR);
    assert(world.player_x == 3 && world.player_y == 12);
    assert(world.player_facing == WORLD_NORTH);
    assert(world.scene->spawn_count == 10);
    assert(world.scene->trigger_count == 20);
    assert(world.scene->hotspots[3].x == 4 && world.scene->hotspots[3].y == 10);
    assert(world.scene->hotspots[3].facing == WORLD_SOUTH);
    assert(world.scene->hotspots[5].x == 4 && world.scene->hotspots[5].y == 10);
    assert(world.scene->hotspots[5].facing == WORLD_NORTH);
    assert(world.scene->hotspots[3].script == GENERATED_SCRIPT_EXAMINE_CELLAR_CIRDANTH);
    assert(world.scene->hotspots[5].script == GENERATED_SCRIPT_EXAMINE_CELLAR_UZDUN);

    /* The original common-room stair is a second reciprocal cellar route;
     * it is distinct from the kitchen stair at the south end. */
    world.active_script = -1;
    world.player_x = 6;
    world.player_y = 2;
    world.trigger_latch = -1;
    world_update(&world, &move_north);
    tick(&world, 12);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_INN);
    assert(world.player_x == 9 && world.player_y == 2);
    assert(world.player_facing == WORLD_SOUTH);
    world.trigger_latch = -1;
    world_update(&world, &move_north);
    tick(&world, 12);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_CELLAR);
    assert(world.player_x == 6 && world.player_y == 2);
    assert(world.player_facing == WORLD_SOUTH);

    world.player_x = 7;
    world.player_y = 3;
    world.player_facing = WORLD_SOUTH;
    world.actors[GENERATED_ENTITY_PLAYER].speech_line = -1;
    world.actors[GENERATED_ENTITY_PLAYER].speech_frames = 0;
    world.actors[GENERATED_ENTITY_PLAYER].speech_override = 0;
    world_update(&world, &interact);
    assert(world_entity_speech(&world, world.player_entity) ==
           &generated_runtime_scripts[GENERATED_SCRIPT_EXAMINE_CELLAR_WINE_TASTY]
                .speech[0]);

    /* Both pantry thresholds retain their locked/open conditions. Trying the
     * upper doorway records the failed attempt, speaks the original line and
     * steps Banec back into the cellar. */
    pantry_flag = world.scene->triggers[0].condition_flag;
    assert(pantry_flag >= 0);
    assert(world.scene->triggers[0].condition_value == 0);
    assert(world.scene->triggers[1].condition_flag == pantry_flag);
    assert(world.scene->triggers[1].condition_value == 1);
    assert(world.scene->triggers[2].condition_flag == pantry_flag);
    assert(world.scene->triggers[2].condition_value == 2);
    world.player_x = 0;
    world.player_y = 9;
    world.player_facing = WORLD_WEST;
    world.active_script = GENERATED_SCRIPT_PANTRY_LOCKED_UPPER;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 12);
    assert(world.flags[pantry_flag] == 1);
    assert(world.player_x == 1 && world.player_y == 9);
    assert(world.player_facing == WORLD_EAST);
    assert(world_entity_speech(&world, world.player_entity) ==
           &generated_runtime_scripts[GENERATED_SCRIPT_PANTRY_LOCKED_UPPER].speech[0]);

    /* The adjacent eastern doorway is Bjarn Fingolson's locked room. It opens
     * the original narrator/deep-voice exchange and turns Banec back west. */
    bjarn_door_flag = world.scene->triggers[6].condition_flag;
    world.player_x = 10;
    world.player_y = 6;
    world.player_facing = WORLD_EAST;
    world.active_script = GENERATED_SCRIPT_BJARN_DOOR_UPPER;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 1);
    assert(world.flags[bjarn_door_flag] == 1);
    assert(world.dialogue == GENERATED_DIALOGUE_BJARN_DOOR);
    assert(strcmp(world_dialogue_text(&world),
                  "You try the door, but find it locked. From within the room you hear a deep voice.") == 0);
    world_update(&world, &interact);
    assert(strcmp(world_dialogue_text(&world),
                  "I do not wish to be disturbed right now, so please go away! First I am burgled, and now I can get no peace with all the busybodies running around, making my tragedy their business!") == 0);
    world_update(&world, &interact);
    tick(&world, 12);
    assert(world.player_x == 9 && world.player_y == 6);
    assert(world.player_facing == WORLD_WEST);

    {
        const struct generated_runtime_dialogue_node *erek_obeys =
            find_dialogue_node(&generated_runtime_dialogues[GENERATED_DIALOGUE_EREK_START],
                               "You surely do. But Master Fingolson made it clear that he wants to be on his own and who am I to disobey his wishes?");
        const struct generated_runtime_dialogue_node *erek_beckons =
            find_dialogue_node(&generated_runtime_dialogues[GENERATED_DIALOGUE_EREK_START],
                               "Seemingly reluctant, Erek beckons you to follow him down into the cellar.");
        const struct generated_runtime_dialogue_choice *insist;
        assert(erek_obeys != 0 && erek_beckons != 0);
        insist = find_dialogue_choice(erek_obeys, "I am trying to help here, Erek!");
        assert(insist != 0);
        assert(choice_has_action(insist, bjarn_door_flag, 2));
        assert(erek_beckons->end_script == GENERATED_SCRIPT_ESCORT_TO_FINGOLSON_ROOM);
        assert(generated_runtime_dialogues[GENERATED_DIALOGUE_BJARN_START]
                   .starts[0].flag == bjarn_door_flag);
        assert(generated_runtime_dialogues[GENERATED_DIALOGUE_BJARN_START]
                   .starts[0].value == 3);
    }

    /* Once Erek has opened the door, both halves of the original two-cell
     * threshold lead to Fingolson's room and return to their matching cellar
     * rows. The room retains Bjarn, Erek's escort placement, and the original
     * chest examination square. */
    world.flags[bjarn_door_flag] = 3;
    world.trigger_latch = -1;
    world_update(&world, &move_east);
    tick(&world, 12);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_FINGOLSON_ROOM);
    assert(world.player_x == 1 && world.player_y == 6);
    assert(world.player_facing == WORLD_WEST);
    assert(world.scene->spawn_count == 3);
    assert(world.scene->trigger_count == 2);
    assert(world.scene->hotspot_count == 1);
    assert(world.scene->hotspots[0].x == 7 && world.scene->hotspots[0].y == 3);
    assert(world.scene->hotspots[0].facing == WORLD_NORTH);
    world.trigger_latch = -1;
    world_update(&world, &move_west);
    tick(&world, 12);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_CELLAR);
    assert(world.player_x == 9 && world.player_y == 6);
    assert(world.player_facing == WORLD_EAST);

    /* Receiving Lucia's key selects the open pantry transition. The room has
     * both original chest interactions and exact reciprocal doorway spawns. */
    get_item_flag = generated_runtime_dialogues[GENERATED_DIALOGUE_SEARCH_SILVERHAIR_CHEST]
                        .starts[0].flag;
    {
        const struct generated_runtime_dialogue_node *lucia_menu =
            find_dialogue_node(&generated_runtime_dialogues[GENERATED_DIALOGUE_LUCIA_START],
                               "Now don't keep me off my work. Ask what you want to know and then out with you!");
        const struct generated_runtime_dialogue_choice *key_request;
        assert(lucia_menu != 0);
        key_request = find_dialogue_choice(
            lucia_menu,
            "I need to fetch something for my mistress, but the pantry is locked.");
        assert(key_request != 0);
        assert(choice_has_condition(key_request, get_item_flag, 1));
        assert(choice_has_condition(key_request, pantry_flag, 1));
    }
    world.active_script = GENERATED_SCRIPT_GRANT_PANTRY_KEY;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 1);
    assert(world.flags[pantry_flag] == 2);
    world.active_script = GENERATED_SCRIPT_ENTER_REDWYNE_PANTRY_UPPER;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_PANTRY);
    assert(world.player_x == 6 && world.player_y == 3);
    assert(world.player_facing == WORLD_WEST);
    assert(world.scene->spawn_count == 2);
    assert(world.scene->trigger_count == 2);
    assert(world.scene->hotspot_count == 2);

    world.player_x = 3;
    world.player_y = 5;
    world.player_facing = WORLD_NORTH;
    world.actors[GENERATED_ENTITY_PLAYER].speech_frames = 0;
    world.actors[GENERATED_ENTITY_PLAYER].speech_override = 0;
    world_update(&world, &interact);
    assert(world_entity_speech(&world, world.player_entity) ==
           &generated_runtime_scripts[GENERATED_SCRIPT_EXAMINE_SILVERHAIR_LUGGAGE]
                .speech[0]);

    have_gem_flag = generated_runtime_scripts[GENERATED_SCRIPT_RECORD_FINGOLSON_GEM]
                        .actions[1].a;
    world.flags[get_item_flag] = 1;
    world.player_x = 4;
    world.player_y = 5;
    world.player_facing = WORLD_NORTH;
    world_update(&world, &interact);
    assert(world.dialogue == GENERATED_DIALOGUE_SEARCH_SILVERHAIR_CHEST);
    assert(strcmp(world_dialogue_text(&world),
                  "Upon opening the chest, a small green something catches your attention ...") == 0);
    world_update(&world, &interact);
    assert(strcmp(world_dialogue_text(&world),
                  "There, inmidst your mistress' luggage, lies one of Master Fingolson's gems.") == 0);
    world_update(&world, &interact);
    tick(&world, 1);
    assert(world.flags[get_item_flag] == 2);
    assert(world.flags[have_gem_flag] == 1);
    silverhair_gem_pending_flag =
        generated_runtime_scripts[GENERATED_SCRIPT_RECORD_FINGOLSON_GEM].actions[2].a;
    assert(world.flags[silverhair_gem_pending_flag] == 1);

    /* The recovered gem takes precedence over Silverhair's ordinary repeat
     * greeting and reproduces the complete original assessment. Completing
     * it records the evidence used by the later Fingolson conversations. */
    {
        static const char *const gem_lines[] = {
            "Returned already? But what is this?",
            "A stone I found in the cellar, among your chests. I hate to think such a thing, but is this one of the jewels that the Dwarf has lost?",
            "You show the jewel to Lady Silverhair, who frowns upon seeing it. She inspects it more closely, but carefully avoids touching it. She straightens then, a puzzled look on her features.",
            "You say this was found among my goods? But this is not a stone of mine, and neither is it the stone I was shown before. I have never seen this before.",
            "But is this not one of the Catseyes he claims to have lost?",
            "It cannot be. For this is no Catseye at all, and in fact is hardly worth a fraction of the other stones.",
            "I do not understand this at all.",
            "I admit he goaded me to rage earlier, to my shame. But I have done nothing more than voice my anger. You must believe that."
        };
        const struct generated_runtime_dialogue *silverhair_dialogue =
            &generated_runtime_dialogues[GENERATED_DIALOGUE_SILVERHAIR_START];
        const struct generated_runtime_dialogue_node *gem_start =
            find_dialogue_node(silverhair_dialogue, gem_lines[0]);

        assert(gem_start != 0);
        assert(silverhair_dialogue->starts[0].flag == silverhair_gem_pending_flag);
        assert(silverhair_dialogue->starts[0].value == 1);
        assert(&silverhair_dialogue->nodes[silverhair_dialogue->starts[0].node] == gem_start);
        world.dialogue = GENERATED_DIALOGUE_SILVERHAIR_START;
        world.dialogue_node = (int)(gem_start - silverhair_dialogue->nodes);
        world.dialogue_choice = 0;
        world.dialogue_actor = -1;
        for (frame = 0; frame < 8; ++frame) {
            assert(strcmp(world_dialogue_text(&world), gem_lines[frame]) == 0);
            world_update(&world, &interact);
        }
        assert(world.dialogue < 0);
        assert(world.active_script == GENERATED_SCRIPT_RECORD_SILVERHAIR_GEM_ASSESSMENT);
        gem_worthless_flag = generated_runtime_scripts[
                                 GENERATED_SCRIPT_RECORD_SILVERHAIR_GEM_ASSESSMENT]
                                 .actions[0].a;
        tick(&world, 1);
        assert(world.flags[gem_worthless_flag] == 1);
        assert(world.flags[have_gem_flag] == 1);
        assert(world.flags[silverhair_gem_pending_flag] == 0);
    }

    /* Erek's original post-discovery branch independently examines the gem.
     * Silverhair's assessment contributes one evidence bit; Erek contributes
     * two, preserving the reference values 2 (Erek only) and 3 (both). */
    {
        const struct generated_runtime_dialogue *erek_dialogue =
            &generated_runtime_dialogues[GENERATED_DIALOGUE_EREK_START];
        const struct generated_runtime_dialogue_node *erek_menu =
            find_dialogue_node(erek_dialogue, "Ho there, Banec. What is it this time?");
        const struct generated_runtime_dialogue_node *identifies_gem =
            find_dialogue_node(
                erek_dialogue,
                "This is a fine example of a Chrysoberyl. Perfectly suitable for a Lady's ring, were that your Mistress' intention. However, it is no Catseye and as such it is of no use in the workings of magic she had in mind, if I recall correctly.");
        const struct generated_runtime_dialogue_choice *erek_only;
        const struct generated_runtime_dialogue_choice *both_assessments = 0;
        int choice_index;

        assert(erek_menu != 0 && identifies_gem != 0);
        erek_only = find_dialogue_choice(
            identifies_gem, "Thank you Erek. That's all I need to know.");
        assert(erek_only != 0);
        for (choice_index = 0; choice_index < identifies_gem->choice_count; ++choice_index) {
            const struct generated_runtime_dialogue_choice *choice =
                &identifies_gem->choices[choice_index];
            if (strcmp(choice->text, "Thank you Erek. That's all I need to know.") == 0 &&
                choice_has_condition(choice, gem_worthless_flag, 1)) {
                both_assessments = choice;
                break;
            }
        }
        assert(choice_has_condition(erek_only, gem_worthless_flag, 0));
        assert(choice_has_action(erek_only, gem_worthless_flag, 2));
        assert(both_assessments != 0);
        assert(choice_has_action(both_assessments, gem_worthless_flag, 3));

        world.dialogue = GENERATED_DIALOGUE_EREK_START;
        world.dialogue_node = (int)(erek_menu - erek_dialogue->nodes);
        world.dialogue_choice = 0;
        world.dialogue_actor = GENERATED_ENTITY_EREK;
        choose_visible_dialogue_option(
            &world, "Erek, I'd like to hear your opinion about the gem I have here.");
        assert(strcmp(world_dialogue_text(&world),
                      "That ... - it's one of the Master's. It must be. Where did you get that, Banec?") == 0);
        choose_visible_dialogue_option(
            &world, "It was hidden in Lady Silverhair's luggage.");
        assert(strcmp(world_dialogue_text(&world),
                      "Then your mistress is not as innocent as you thought, no? Let me see the gem, please.") == 0);
        world_update(&world, &interact);
        assert(strstr(world_dialogue_text(&world), "Carefully, Erek takes the gem") != 0);
        world_update(&world, &interact);
        assert(strstr(world_dialogue_text(&world), "Once caught again") != 0);
        world_update(&world, &interact);
        assert(world.dialogue_node == (int)(identifies_gem - erek_dialogue->nodes));
        assert(world_dialogue_choice_count(&world) == 1);
        choose_visible_dialogue_option(&world, "Thank you Erek. That's all I need to know.");
        assert(world.dialogue < 0);
        assert(world.flags[gem_worthless_flag] == 3);
    }

    world.active_script = GENERATED_SCRIPT_LEAVE_REDWYNE_PANTRY_UPPER;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_CELLAR);
    assert(world.player_x == 1 && world.player_y == 9);
    assert(world.player_facing == WORLD_EAST);

    world.active_script = GENERATED_SCRIPT_LEAVE_REDWYNE_CELLAR;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_KITCHEN);
    assert(world.player_x == 6 && world.player_y == 5);
    assert(world.player_facing == WORLD_NORTH);

    /* The original pre-release route reaches Silverhair through the barn
     * ground floor, loft and exterior veranda. It is the only route that can
     * unlock Frostbloom's Yeti conversation before Jelom relents. */
    world_initialize(&world);
    world.dialogue = -1;
    world.dialogue_actor = -1;
    world.active_script = GENERATED_SCRIPT_ENTER_BARN_EXTERIOR_WEST;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_BARN);
    assert(world.player_x == 1 && world.player_y == 6);
    assert(world.scene->width == 6 && world.scene->height == 8);
    assert(world.scene->spawn_count == 6);
    assert(world.scene->trigger_count == 6);

    /* Traverse the real barn floor and stair instead of invoking the scene
     * transition directly. This protects the quest route from collision or
     * trigger regressions in the imported map. */
    walk_tile(&world, &move_north);
    walk_tile(&world, &move_north);
    walk_tile(&world, &move_north);
    walk_tile(&world, &move_east);
    walk_tile(&world, &move_east);
    walk_tile(&world, &move_north);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_BARN_LOFT);
    assert(world.player_x == 1 && world.player_y == 3);
    assert(world.scene->width == 4 && world.scene->height == 9);
    assert(world.scene->hotspot_count == 2);

    walk_tile(&world, &move_south);
    walk_tile(&world, &move_south);
    walk_tile(&world, &move_south);
    walk_tile(&world, &move_south);
    walk_tile(&world, &move_west);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_EXTERIOR);
    assert(world.player_x == 23 && world.player_y == 12);
    assert(world.player_facing == WORLD_WEST);
    assert(world.flags[generated_runtime_scripts[
               GENERATED_SCRIPT_LEAVE_BARN_LOFT_TO_VERANDA].actions[0].a] == 1);

    /* Holding west crosses the exterior veranda and activates its conditional
     * first-visit trigger; this exercises the real collision and trigger path. */
    world.trigger_latch = -1;
    for (frame = 0; frame < 180 &&
         world.scene_index == GENERATED_SCENE_REDWYNE_EXTERIOR; ++frame)
        world_update(&world, &move_west);
    assert(frame < 180);
    for (frame = 0; frame < 30 && world.dialogue < 0; ++frame)
        tick(&world, 1);
    assert(frame < 30);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_SILVERHAIR_ROOM);
    assert(world.player_x == 5 && world.player_y == 4);
    assert(world.dialogue == GENERATED_DIALOGUE_SILVERHAIR_START);
    assert(strcmp(world_dialogue_text(&world), "Oh!!") == 0);
    assert(world.flags[generated_runtime_scripts[
               GENERATED_SCRIPT_ENTER_SILVERHAIR_FROM_VERANDA_FIRST].actions[0].a] == 1);

    world.dialogue = -1;
    world.dialogue_actor = -1;
    world.active_script = GENERATED_SCRIPT_LEAVE_SILVERHAIR_HALLWAY_REFUSED;
    world.script_action = 0;
    world.script_wait = 0;
    world.player_x = 5;
    world.player_y = 1;
    tick(&world, 12);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_SILVERHAIR_ROOM);
    assert(world.player_x == 5 && world.player_y == 2);
    assert(world_entity_speech(&world, world.player_entity) != 0);

    world.active_script = GENERATED_SCRIPT_LEAVE_SILVERHAIR_TO_VERANDA;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_EXTERIOR);
    assert(world.player_x == 12 && world.player_y == 12);
    assert(world.player_facing == WORLD_EAST);
    assert(world.flags[generated_runtime_scripts[
               GENERATED_SCRIPT_ENTER_SILVERHAIR_FROM_VERANDA_FIRST].actions[0].a] == 0);

    world.active_script = GENERATED_SCRIPT_ENTER_BARN_LOFT_FROM_VERANDA;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_BARN_LOFT);
    assert(world.player_x == 1 && world.player_y == 7);
    world.active_script = GENERATED_SCRIPT_LEAVE_BARN_LOFT;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_BARN);
    assert(world.player_x == 3 && world.player_y == 3);
    world.active_script = GENERATED_SCRIPT_LEAVE_BARN_TO_CELLAR;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_CELLAR);
    assert(world.player_x == 12 && world.player_y == 2);
    world.active_script = GENERATED_SCRIPT_ENTER_BARN_FROM_CELLAR;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 2);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_BARN);
    assert(world.player_x == 1 && world.player_y == 3);

    /* The remaining original submaps form reciprocal, data-authored routes.
     * Check every newly added branch so a renamed spawn or omitted doorway is
     * caught before producing a ROM. */
    world_initialize(&world);
    run_script(&world, GENERATED_SCRIPT_ENTER_REDWYNE_BATH);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_BATH);
    assert(world.player_x == 4 && world.player_y == 6);
    assert(world.scene->hotspot_count == 3);
    run_script(&world, GENERATED_SCRIPT_LEAVE_REDWYNE_BATH);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_CELLAR);
    assert(world.player_x == 3 && world.player_y == 6);

    run_script(&world, GENERATED_SCRIPT_ENTER_ALEK_ROOM_UPPER);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_ALEK_ROOM);
    assert(world.player_x == 5 && world.player_y == 6);
    assert(world.scene->hotspot_count == 1);
    run_script(&world, GENERATED_SCRIPT_LEAVE_ALEK_ROOM_UPPER);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_CELLAR);
    assert(world.player_x == 1 && world.player_y == 6);

    run_script(&world, GENERATED_SCRIPT_ENTER_FELLNIR_ROOM);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_FELLNIR_ROOM);
    assert(world.player_x == 1 && world.player_y == 3);
    assert(world.scene->hotspot_count == 7);
    assert(find_scene_entity(world.scene, GENERATED_ENTITY_FELLNIR) >= 0);
    assert(generated_runtime_schedules[GENERATED_SCHEDULE_FELLNIR_ROOM].entity ==
           GENERATED_ENTITY_FELLNIR);
    run_script(&world, GENERATED_SCRIPT_LEAVE_FELLNIR_ROOM);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_FIRST_FLOOR);
    assert(world.player_x == 8 && world.player_y == 2);

    run_script(&world, GENERATED_SCRIPT_ENTER_GUEST_ROOM);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_GUEST_ROOM);
    assert(world.player_x == 4 && world.player_y == 3);
    run_script(&world, GENERATED_SCRIPT_LEAVE_GUEST_ROOM);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_FIRST_FLOOR);
    assert(world.player_x == 1 && world.player_y == 2);

    run_script(&world, GENERATED_SCRIPT_ENTER_UPPER_HALL);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_UPPER_HALL);
    assert(world.player_x == 4 && world.player_y == 2);
    assert(world.scene->trigger_count == 4);
    run_script(&world, GENERATED_SCRIPT_ENTER_UPPER_WEST_ROOM);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_UPPER_WEST_ROOM);
    run_script(&world, GENERATED_SCRIPT_LEAVE_UPPER_WEST_ROOM);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_UPPER_HALL);
    assert(world.player_x == 2 && world.player_y == 5);
    run_script(&world, GENERATED_SCRIPT_ENTER_UPPER_EAST_ROOM);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_UPPER_EAST_ROOM);
    run_script(&world, GENERATED_SCRIPT_LEAVE_UPPER_EAST_ROOM);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_UPPER_HALL);
    assert(world.player_x == 1 && world.player_y == 5);
    run_script(&world, GENERATED_SCRIPT_ENTER_TRISTAN_ROOM);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_TRISTAN_ROOM);
    assert(world.scene->hotspot_count == 1);
    run_script(&world, GENERATED_SCRIPT_LEAVE_TRISTAN_ROOM);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_UPPER_HALL);
    assert(world.player_x == 1 && world.player_y == 7);
    run_script(&world, GENERATED_SCRIPT_LEAVE_UPPER_HALL);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_FIRST_FLOOR);
    assert(world.player_x == 6 && world.player_y == 2);

    run_script(&world, GENERATED_SCRIPT_ENTER_GUARDHOUSE);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_GUARDHOUSE);
    assert(world.player_x == 7 && world.player_y == 3);
    run_script(&world, GENERATED_SCRIPT_ENTER_GUARDHOUSE_UPPER);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_GUARDHOUSE_UPPER);
    assert(world.player_x == 2 && world.player_y == 8);
    assert(world.scene->hotspot_count == 3);
    run_script(&world, GENERATED_SCRIPT_LEAVE_GUARDHOUSE_UPPER);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_GUARDHOUSE);
    assert(world.player_x == 2 && world.player_y == 8);
    run_script(&world, GENERATED_SCRIPT_LEAVE_GUARDHOUSE);
    assert(world.scene_index == GENERATED_SCENE_REDWYNE_EXTERIOR);
    assert(world.player_x == 13 && world.player_y == 23);

    /* Temporary escorts can return an actor to an authored checkpoint instead
     * of hard-coding a follow-up routine. Erek therefore returns to the exact
     * paused position beside Jelom after opening Fingolson's room. */
    {
        struct world_state return_world;
        struct world_actor_state *returning_erek;
        const struct generated_runtime_schedule *leave_route =
            &generated_runtime_schedules[GENERATED_SCHEDULE_EREK_LEAVE_FINGOLSON];
        world_initialize(&return_world);
        return_world.dialogue = -1;
        return_world.dialogue_actor = -1;
        returning_erek = &return_world.actors[GENERATED_ENTITY_EREK];
        returning_erek->scene = GENERATED_SCENE_REDWYNE_FIRST_FLOOR;
        returning_erek->x = 2;
        returning_erek->y = 4;
        returning_erek->facing = WORLD_NORTH;
        returning_erek->schedule = -1;
        return_world.active_script = GENERATED_SCRIPT_ESCORT_TO_FINGOLSON_ROOM;
        return_world.script_action = 0;
        tick(&return_world, 1);
        assert(return_world.actor_checkpoints[GENERATED_ENTITY_EREK].valid);
        assert(return_world.actor_checkpoints[GENERATED_ENTITY_EREK].scene ==
               GENERATED_SCENE_REDWYNE_FIRST_FLOOR);
        return_world.active_script = -1;
        return_world.transition_phase = WORLD_TRANSITION_NONE;
        returning_erek->scene = GENERATED_SCENE_REDWYNE_INN;
        returning_erek->x = 9;
        returning_erek->y = 2;
        returning_erek->schedule = GENERATED_SCHEDULE_EREK_LEAVE_FINGOLSON;
        returning_erek->schedule_step = leave_route->step_count - 1;
        returning_erek->schedule_wait = 0;
        tick(&return_world, 1);
        assert(!return_world.actor_checkpoints[GENERATED_ENTITY_EREK].valid);
        assert(returning_erek->scene == GENERATED_SCENE_REDWYNE_FIRST_FLOOR);
        assert(returning_erek->x == 2 && returning_erek->y == 4);
        assert(returning_erek->facing == WORLD_NORTH);
        assert(returning_erek->schedule == -1);
    }

    /* Save data is pointer-free and restores persistent state while forcing
     * transient route buffers and ROM speech pointers to be reconstructed. */
    {
        struct world_state loaded;
        int save_words[SAVEGAME_MAX_WORDS];
        int loaded_words[SAVEGAME_MAX_WORDS];
        int word_count;
        int loaded_word_count;
        world.flags[0] = 73;
        world.object_states[0] = 2;
        world.game_time = 8117;
        world.game_time_subticks = 9;
        world.actors[GENERATED_ENTITY_TALAN].path_schedule_step = 4;
        world.actors[GENERATED_ENTITY_TALAN].path_length = 7;
        world.actors[GENERATED_ENTITY_TALAN].speech_override =
            (const struct generated_runtime_text *)1;
        world.actor_checkpoints[GENERATED_ENTITY_EREK].valid = 1;
        world.actor_checkpoints[GENERATED_ENTITY_EREK].scene =
            GENERATED_SCENE_REDWYNE_FIRST_FLOOR;
        world.actor_checkpoints[GENERATED_ENTITY_EREK].x = 2;
        world.actor_checkpoints[GENERATED_ENTITY_EREK].y = 4;
        world.actor_checkpoints[GENERATED_ENTITY_EREK].facing = WORLD_NORTH;
        world.actor_checkpoints[GENERATED_ENTITY_EREK].visual =
            world.actors[GENERATED_ENTITY_EREK].visual;
        world.actor_checkpoints[GENERATED_ENTITY_EREK].schedule = -1;
        world.actor_checkpoints[GENERATED_ENTITY_EREK].dialogue =
            world.actors[GENERATED_ENTITY_EREK].dialogue;
        word_count = savegame_encode(&world, save_words, SAVEGAME_MAX_WORDS);
        assert(word_count > 0 && word_count < SAVEGAME_MAX_WORDS);
        assert(savegame_checksum(save_words, word_count) != 0);
        assert(savegame_decode(&loaded, save_words, word_count));
        assert(loaded.scene == &generated_runtime_scenes[world.scene_index]);
        assert(loaded.scene_index == world.scene_index);
        assert(loaded.player_x == world.player_x && loaded.player_y == world.player_y);
        assert(loaded.flags[0] == 73);
        assert(loaded.object_states[0] == 2);
        assert(loaded.game_time == 8117 && loaded.game_time_subticks == 9);
        assert(loaded.actors[GENERATED_ENTITY_TALAN].path_schedule_step == -1);
        assert(loaded.actors[GENERATED_ENTITY_TALAN].path_length == 0);
        assert(loaded.actors[GENERATED_ENTITY_TALAN].speech_override == 0);
        assert(loaded.actor_checkpoints[GENERATED_ENTITY_EREK].valid);
        assert(loaded.actor_checkpoints[GENERATED_ENTITY_EREK].scene ==
               GENERATED_SCENE_REDWYNE_FIRST_FLOOR);
        assert(loaded.actor_checkpoints[GENERATED_ENTITY_EREK].x == 2 &&
               loaded.actor_checkpoints[GENERATED_ENTITY_EREK].y == 4);
        loaded_word_count = savegame_encode(&loaded, loaded_words,
                                            SAVEGAME_MAX_WORDS);
        assert(loaded_word_count == word_count);
        assert(memcmp(loaded_words, save_words,
                      (size_t)word_count * sizeof(save_words[0])) == 0);
        /* Version 1 predates the saved game clock; it remains loadable and
         * resumes at the original noon start rather than becoming corrupt. */
        save_words[0] = 1;
        assert(savegame_decode(&loaded, save_words,
                               word_count - 2 - GENERATED_RUNTIME_ENTITY_COUNT * 12));
        assert(loaded.game_time == generated_runtime_initial_game_time);
        save_words[0] = SAVEGAME_FORMAT_VERSION;
        assert(!savegame_decode(&loaded, save_words, word_count - 1));
        save_words[1] ^= 1;
        assert(!savegame_decode(&loaded, save_words, word_count));
    }

    puts("world schedule checks passed");
    return 0;
}
