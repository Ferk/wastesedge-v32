#include "engine/world.h"

#define WALK_NORTH 2
#define WALK_SOUTH 1
#define WALK_EAST 4
#define WALK_WEST 8
#define WALK_PIXELS_PER_FRAME 2

/* wasm2vircon accepts the canonical stack-pointer ABI, so world_state may
 * live in main's automatic storage. Clang still combines adjacent assignments
 * here into a dynamic-address i64.store; retain this narrowly scoped i32
 * store helper until that lowering pattern is accepted as well. */
#if defined(__clang__)
#define WORLD_NOINLINE __attribute__((noinline))
#else
#define WORLD_NOINLINE
#endif

static WORLD_NOINLINE void store_int(int *slot, int value)
{
    *(volatile int *)slot = value;
}

static WORLD_NOINLINE void store_scene(const struct generated_runtime_scene **slot,
                                       const struct generated_runtime_scene *scene)
{
    *(const struct generated_runtime_scene * volatile *)slot = scene;
}

static int opposite_mask(int move_x, int move_y)
{
    if (move_y < 0) return WALK_SOUTH;
    if (move_y > 0) return WALK_NORTH;
    if (move_x < 0) return WALK_EAST;
    return WALK_WEST;
}

static int direction_mask(int move_x, int move_y)
{
    if (move_y < 0) return WALK_NORTH;
    if (move_y > 0) return WALK_SOUTH;
    if (move_x < 0) return WALK_WEST;
    return WALK_EAST;
}

static int collision_mask_at(const struct generated_runtime_scene *scene, int tile_x, int tile_y)
{
    int index;
    if (tile_x < 0 || tile_y < 0 || tile_x >= scene->width || tile_y >= scene->height)
        return 0;
    for (index = 0; index < scene->collision_count; ++index) {
        const struct generated_runtime_collision *collision = &scene->collisions[index];
        int left = collision->x / scene->tile_size;
        int top = collision->y / scene->tile_size;
        int right = (collision->x + collision->width - 1) / scene->tile_size;
        int bottom = (collision->y + collision->height - 1) / scene->tile_size;
        if (tile_x >= left && tile_x <= right && tile_y >= top && tile_y <= bottom)
            return collision->walkability;
    }
    return 15;
}

static int entity_blocks_tile(const struct world_state *world, int entity_index, int tile_x, int tile_y)
{
    const struct generated_runtime_entity *entity = &world->scene->entities[entity_index];
    const struct generated_runtime_actor_visual *visual;
    int x;
    int y;
    if (entity_index == world->player_entity || !entity->blocks_movement)
        return 0;
    visual = &generated_runtime_actor_visuals[entity->visual];
    x = entity->tile_x;
    y = entity->tile_y;
    return tile_x == x && tile_y >= y - visual->base_y && tile_y <= y;
}

static int can_move(const struct world_state *world, int move_x, int move_y)
{
    const struct generated_runtime_actor_visual *visual = &generated_runtime_actor_visuals[
        world->scene->entities[world->player_entity].visual];
    int local_x;
    int local_y;
    int source_mask = direction_mask(move_x, move_y);
    int target_mask = opposite_mask(move_x, move_y);
    int left = world->player_x - visual->base_x;
    int top = world->player_y - visual->base_y;
    for (local_y = 0; local_y <= visual->base_y; ++local_y) {
        for (local_x = 0; local_x <= visual->base_x; ++local_x) {
            int x = left + local_x;
            int y = top + local_y;
            int entity_index;
            if (!(collision_mask_at(world->scene, x, y) & source_mask) ||
                !(collision_mask_at(world->scene, x + move_x, y + move_y) & target_mask))
                return 0;
            for (entity_index = 0; entity_index < world->scene->entity_count; ++entity_index) {
                if (entity_blocks_tile(world, entity_index, x + move_x, y + move_y))
                    return 0;
            }
        }
    }
    return 1;
}

static int facing_for_direction(int move_x, int move_y)
{
    if (move_y < 0) return WORLD_NORTH;
    if (move_y > 0) return WORLD_SOUTH;
    if (move_x < 0) return WORLD_WEST;
    return WORLD_EAST;
}

static int interaction_target(const struct world_state *world)
{
    int move_x = 0;
    int move_y = 0;
    int index;
    if (world->player_facing == WORLD_NORTH) move_y = -1;
    if (world->player_facing == WORLD_SOUTH) move_y = 1;
    if (world->player_facing == WORLD_WEST) move_x = -1;
    if (world->player_facing == WORLD_EAST) move_x = 1;
    for (index = 0; index < world->scene->entity_count; ++index) {
        const struct generated_runtime_entity *entity = &world->scene->entities[index];
        if (entity->interaction_dialogue >= 0 &&
            entity_blocks_tile(world, index, world->player_x + move_x, world->player_y + move_y))
            return entity->interaction_dialogue;
    }
    return -1;
}

static void apply_dialogue_choice(struct world_state *world,
                                  const struct generated_runtime_dialogue_choice *choice)
{
    int index;
    for (index = 0; index < choice->action_count; ++index) {
        const struct generated_runtime_dialogue_action *action = &choice->actions[index];
        if (action->flag >= 0 && action->flag < GENERATED_RUNTIME_FLAG_COUNT)
            world->flags[action->flag] = action->value;
    }
    world->dialogue_node = choice->next_node;
}

static void close_dialogue(struct world_state *world)
{
    store_int(&world->dialogue, -1);
    store_int(&world->dialogue_node, 0);
    store_int(&world->dialogue_choice, 0);
}

/* Player lines are the selectable answers themselves. The original dialogue
 * runner recorded the selected answer, then advanced directly to the next
 * NPC or narrator line instead of displaying the answer a second time. */
static void advance_past_player_lines(struct world_state *world)
{
    int remaining;
    if (world->dialogue < 0)
        return;
    remaining = generated_runtime_dialogues[world->dialogue].node_count;
    while (remaining-- > 0) {
        const struct generated_runtime_dialogue *dialogue = &generated_runtime_dialogues[world->dialogue];
        const struct generated_runtime_dialogue_node *node = &dialogue->nodes[world->dialogue_node];
        const struct generated_runtime_dialogue_speaker *speaker = &dialogue->speakers[node->speaker];
        if (!speaker->is_player)
            return;
        if (node->next_node < 0) {
            close_dialogue(world);
            return;
        }
        world->dialogue_node = node->next_node;
    }
    close_dialogue(world);
}

void world_initialize(struct world_state *world)
{
    int index;
    store_scene(&world->scene, &generated_runtime_scenes[generated_runtime_initial_scene]);
    store_int(&world->player_entity, -1);
    store_int(&world->player_x, 0);
    store_int(&world->player_y, 0);
    store_int(&world->player_facing, WORLD_SOUTH);
    store_int(&world->moving, 0);
    store_int(&world->move_x, 0);
    store_int(&world->move_y, 0);
    store_int(&world->move_progress, 0);
    store_int(&world->animation_frame, 0);
    store_int(&world->dialogue, -1);
    store_int(&world->dialogue_node, 0);
    store_int(&world->dialogue_choice, 0);
    for (index = 0; index < GENERATED_RUNTIME_FLAG_STORAGE_COUNT; ++index)
        store_int(&world->flags[index], 0);
    for (index = 0; index < world->scene->entity_count; ++index) {
        const struct generated_runtime_entity *entity = &world->scene->entities[index];
        if (entity->player_controlled) {
            world->player_entity = index;
            world->player_x = entity->tile_x;
            world->player_y = entity->tile_y;
            world->player_facing = entity->facing;
            if (world->scene->entry_dialogue >= 0) {
                world->dialogue = world->scene->entry_dialogue;
                world->dialogue_node = generated_runtime_dialogues[world->dialogue].start_node;
            }
            return;
        }
    }
}

void world_update(struct world_state *world, const struct world_actions *actions)
{
    int move_x = actions->move_x;
    int move_y = actions->move_y;
    if (world->player_entity < 0)
        return;
    if (world->dialogue >= 0) {
        const struct generated_runtime_dialogue *dialogue = &generated_runtime_dialogues[world->dialogue];
        const struct generated_runtime_dialogue_node *node = &dialogue->nodes[world->dialogue_node];
        if (node->choice_count > 0 && actions->choice_move != 0) {
            world->dialogue_choice += actions->choice_move;
            if (world->dialogue_choice < 0)
                world->dialogue_choice = node->choice_count - 1;
            if (world->dialogue_choice >= node->choice_count)
                world->dialogue_choice = 0;
        }
        if (actions->interact_pressed) {
            if (node->choice_count > 0) {
                apply_dialogue_choice(world, &node->choices[world->dialogue_choice]);
                world->dialogue_choice = 0;
                advance_past_player_lines(world);
            } else if (node->next_node >= 0) {
                world->dialogue_node = node->next_node;
                world->dialogue_choice = 0;
            } else {
                close_dialogue(world);
            }
        }
        return;
    }
    if (world->moving) {
        world->move_progress += WALK_PIXELS_PER_FRAME;
        if ((world->move_progress & 3) == 0)
            ++world->animation_frame;
        if (world->move_progress >= world->scene->tile_size) {
            world->player_x += world->move_x;
            world->player_y += world->move_y;
            world->moving = 0;
            world->move_progress = 0;
        }
        return;
    }
    if (actions->interact_pressed) {
        int dialogue = interaction_target(world);
        if (dialogue >= 0) {
            store_int(&world->dialogue, dialogue);
            store_int(&world->dialogue_node, generated_runtime_dialogues[dialogue].start_node);
            store_int(&world->dialogue_choice, 0);
        }
        return;
    }
    if (move_x != 0)
        move_y = 0;
    if (move_x == 0 && move_y == 0)
        return;
    world->player_facing = facing_for_direction(move_x, move_y);
    if (!can_move(world, move_x, move_y))
        return;
    world->move_x = move_x;
    world->move_y = move_y;
    world->moving = 1;
    world->move_progress = 0;
}

int world_entity_x(const struct world_state *world, int entity_index)
{
    if (entity_index == world->player_entity)
        return world->player_x;
    return world->scene->entities[entity_index].tile_x;
}

int world_entity_y(const struct world_state *world, int entity_index)
{
    if (entity_index == world->player_entity)
        return world->player_y;
    return world->scene->entities[entity_index].tile_y;
}

int world_entity_facing(const struct world_state *world, int entity_index)
{
    if (entity_index == world->player_entity)
        return world->player_facing;
    return world->scene->entities[entity_index].facing;
}

int world_entity_is_moving(const struct world_state *world, int entity_index)
{
    return entity_index == world->player_entity && world->moving;
}

int world_dialogue_is_open(const struct world_state *world)
{
    return world->dialogue >= 0;
}

const char *world_dialogue_text(const struct world_state *world)
{
    if (world->dialogue < 0)
        return "";
    return generated_runtime_dialogues[world->dialogue].nodes[world->dialogue_node].text;
}

const char *world_dialogue_choice_text(const struct world_state *world)
{
    return world_dialogue_choice_at(world, world_dialogue_choice_selected(world));
}

const char *world_dialogue_choice_at(const struct world_state *world, int index)
{
    const struct generated_runtime_dialogue_node *node;
    if (world->dialogue < 0)
        return "";
    node = &generated_runtime_dialogues[world->dialogue].nodes[world->dialogue_node];
    return index >= 0 && index < node->choice_count ? node->choices[index].text : "";
}

int world_dialogue_choice_count(const struct world_state *world)
{
    if (world->dialogue < 0)
        return 0;
    return generated_runtime_dialogues[world->dialogue].nodes[world->dialogue_node].choice_count;
}

int world_dialogue_choice_selected(const struct world_state *world)
{
    return world->dialogue_choice;
}

const struct generated_runtime_dialogue_speaker *world_dialogue_speaker(const struct world_state *world)
{
    const struct generated_runtime_dialogue *dialogue;
    const struct generated_runtime_dialogue_node *node;
    if (world->dialogue < 0)
        return 0;
    dialogue = &generated_runtime_dialogues[world->dialogue];
    node = &dialogue->nodes[world->dialogue_node];
    return &dialogue->speakers[node->speaker];
}

const struct generated_runtime_text *world_dialogue_display(const struct world_state *world)
{
    if (world->dialogue < 0)
        return 0;
    return &generated_runtime_dialogues[world->dialogue].nodes[world->dialogue_node].display;
}

const struct generated_runtime_text *world_dialogue_choice_display(const struct world_state *world, int index)
{
    const struct generated_runtime_dialogue_node *node;
    if (world->dialogue < 0)
        return 0;
    node = &generated_runtime_dialogues[world->dialogue].nodes[world->dialogue_node];
    if (index < 0 || index >= node->choice_count)
        return 0;
    return &node->choices[index].display;
}
