#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "content_ids.h"
#include "engine/world.h"

static void tick(struct world_state *world, int frames)
{
    struct world_actions actions = { 0, 0, 0, 0 };
    while (frames-- > 0)
        world_update(world, &actions);
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
    const struct generated_runtime_dialogue *talan_dialogue;
    const struct generated_runtime_dialogue_node *jelom_node = 0;
    struct world_actions interact = { 0, 0, 1, 0 };
    int frozen_x;
    int frozen_y;
    int frozen_progress;
    int frame;
    int node_index;
    int visual_index;
    int direction;
    int animation;

    world_initialize(&world);

    /* Authored character frames remain separate PNGs, but every runtime actor
     * visual must occupy one generated texture and address distinct regions. */
    for (visual_index = 0; visual_index < generated_runtime_actor_visual_count; ++visual_index) {
        const struct generated_runtime_actor_visual *visual =
            &generated_runtime_actor_visuals[visual_index];
        int texture = visual->standing[0].texture;
        assert(texture >= 0);
        for (direction = 0; direction < 4; ++direction) {
            assert(visual->standing[direction].texture == texture);
            for (animation = 0; animation < visual->walking_count[direction]; ++animation)
                assert(visual->walking[direction][animation].texture == texture);
        }
        assert(visual->standing[0].source_x != visual->standing[1].source_x ||
               visual->standing[0].source_y != visual->standing[1].source_y);
        assert(visual->standing[0].width > 0 && visual->standing[0].height > 0);
    }

    world.dialogue = -1;
    world.active_script = GENERATED_SCRIPT_ENTER_INN;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 900);

    talan = &world.actors[GENERATED_ENTITY_TALAN];
    tristan = &world.actors[GENERATED_ENTITY_TRISTAN];
    orloth = &world.actors[GENERATED_ENTITY_ORLOTH];
    alek = &world.actors[GENERATED_ENTITY_ALEK];
    erek = &world.actors[GENERATED_ENTITY_EREK];
    oliver = &world.actors[GENERATED_ENTITY_OLIVER];
    frostbloom = &world.actors[GENERATED_ENTITY_FROSTBLOOM];
    assert(talan->schedule == GENERATED_SCHEDULE_TALAN_GATE_PATROL);
    assert(tristan->schedule == GENERATED_SCHEDULE_TRISTAN_INN_AND_GATE);
    assert(orloth->schedule == GENERATED_SCHEDULE_ORLOTH_COMMON_ROOM);
    assert(alek->schedule == GENERATED_SCHEDULE_ALEK_BAR_AND_TABLE);
    assert(frostbloom->schedule == GENERATED_SCHEDULE_FROSTBLOOM_YARD);
    assert(erek->schedule == GENERATED_SCHEDULE_EREK_COMMON_ROOM);
    assert(oliver->schedule == GENERATED_SCHEDULE_OLIVER_YARD);
    assert(orloth->scene == GENERATED_SCENE_REDWYNE_INN);
    assert(orloth->x == 2 && orloth->y == 2);
    assert(alek->scene == GENERATED_SCENE_REDWYNE_INN);
    assert(alek->x == 1 && alek->y == 3);
    assert(erek->scene == GENERATED_SCENE_REDWYNE_INN);
    assert(erek->x == 5 && erek->y == 5);
    assert(oliver->scene == GENERATED_SCENE_REDWYNE_EXTERIOR);
    assert(oliver->x == 25 && oliver->y == 15);
    assert(frostbloom->scene == GENERATED_SCENE_REDWYNE_EXTERIOR);
    assert(talan->dialogue == GENERATED_DIALOGUE_TALAN_START);
    assert(talan->x == 11);
    assert(talan->y == 17 || talan->y == 18 || talan->y == 19);

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
    assert(jelom_node->choices[0].condition_flag >= 0);
    world.flags[jelom_node->choices[0].condition_flag] = 1;
    assert(world_dialogue_choice_count(&world) == 1);
    assert(strcmp(world_dialogue_choice_at(&world, 0),
                  "Ah, Jelom said that, did he? What else did he say?") == 0);
    world.flags[jelom_node->choices[0].condition_flag] = 0;
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

    /* Orloth follows the reference bar/clock/table route. Explicit return
     * waypoints route him around the bar's one-way collision edge. */
    talan->schedule = -1;
    tristan->schedule = -1;
    alek->schedule = -1;
    world.player_x = 14;
    world.player_y = 8;
    orloth->scene = GENERATED_SCENE_REDWYNE_INN;
    orloth->x = 2;
    orloth->y = 2;
    orloth->moving = 0;
    orloth->move_progress = 0;
    orloth->schedule_step = 0;
    orloth->schedule_wait = 0;
    for (frame = 0; frame < 2000; ++frame) {
        tick(&world, 1);
        if (orloth->x == 10 && orloth->y == 3 && !orloth->moving)
            break;
    }
    assert(frame < 2000);
    orloth->schedule_wait = 0;
    for (frame = 0; frame < 2000; ++frame) {
        tick(&world, 1);
        if (orloth->x == 2 && orloth->y == 2 && !orloth->moving)
            break;
    }
    assert(frame < 2000);
    for (frame = 0; frame < 10000; ++frame) {
        orloth->schedule_wait = 0;
        tick(&world, 1);
        if (orloth->schedule_step == 0 && orloth->x == 2 && orloth->y == 2 &&
            !orloth->moving)
            break;
    }
    assert(frame < 10000);

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

    /* Rhayne's deterministic authored circuit stays within the same yard
     * bounds as the reference random walk and clears the central tree. */
    frostbloom->scene = GENERATED_SCENE_REDWYNE_EXTERIOR;
    frostbloom->x = 18;
    frostbloom->y = 22;
    frostbloom->moving = 0;
    frostbloom->move_progress = 0;
    frostbloom->schedule = GENERATED_SCHEDULE_FROSTBLOOM_YARD;
    frostbloom->schedule_step = 0;
    frostbloom->schedule_wait = 0;
    for (frame = 0; frame < 5000; ++frame) {
        frostbloom->schedule_wait = 0;
        tick(&world, 1);
        if (frostbloom->x == 16 && frostbloom->y == 22 && !frostbloom->moving &&
            frostbloom->schedule_step == 3)
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
            erek->x == 5 && erek->y == 5 && !erek->moving &&
            erek->schedule_step == 0)
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
    assert(strcmp(generated_runtime_dialogues[GENERATED_DIALOGUE_FROSTBLOOM_START].nodes[0].text,
                  "Hm? What do you want?") == 0);

    puts("world schedule checks passed");
    return 0;
}
