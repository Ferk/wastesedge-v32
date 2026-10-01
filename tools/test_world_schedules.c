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
    struct world_actions interact = { 0, 0, 1, 0 };
    int frozen_x;
    int frozen_y;
    int frozen_progress;
    int frame;

    world_initialize(&world);
    world.dialogue = -1;
    world.active_script = GENERATED_SCRIPT_ENTER_INN;
    world.script_action = 0;
    world.script_wait = 0;
    tick(&world, 900);

    talan = &world.actors[GENERATED_ENTITY_TALAN];
    tristan = &world.actors[GENERATED_ENTITY_TRISTAN];
    assert(talan->schedule == GENERATED_SCHEDULE_TALAN_GATE_PATROL);
    assert(tristan->schedule == GENERATED_SCHEDULE_TRISTAN_INN_AND_GATE);
    assert(talan->dialogue == GENERATED_DIALOGUE_TALAN_START);
    assert(talan->x == 11);
    assert(talan->y == 17 || talan->y == 18 || talan->y == 19);

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

    puts("world schedule checks passed");
    return 0;
}
