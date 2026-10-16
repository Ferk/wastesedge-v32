#include "engine/world.h"

#define WALK_NORTH 2
#define WALK_SOUTH 1
#define WALK_EAST 4
#define WALK_WEST 8
#define WALK_PIXELS_PER_FRAME 2
#define PATH_MAX_WIDTH 64
#define PATH_MAX_HEIGHT 64
#define PATH_MAX_TILES (PATH_MAX_WIDTH * PATH_MAX_HEIGHT)
#define PATH_PLAYER_SLOT GENERATED_RUNTIME_ENTITY_STORAGE_COUNT
#define PATH_ROUTE_SLOT_COUNT (GENERATED_RUNTIME_ENTITY_STORAGE_COUNT + 1)
#define PATH_UNSEEN 0
#define PATH_OPEN 1
#define PATH_CLOSED 2

/* Adonthell 0.3 used A* over its 20x20 mapsquares and stored the resulting
 * cardinal moves on each mapcharacter. Fixed buffers preserve those semantics
 * without heap allocation; the largest current scene is 42x40. */
static int path_open_heap[PATH_MAX_TILES];
static int path_heap_position[PATH_MAX_TILES];
static int path_move_cost[PATH_MAX_TILES];
static signed char path_parent_direction[PATH_MAX_TILES];
static unsigned char path_node_state[PATH_MAX_TILES];
static unsigned char path_reverse[PATH_MAX_TILES];
static unsigned char path_routes[PATH_ROUTE_SLOT_COUNT][PATH_MAX_TILES];
static int path_heap_count;
static int path_heap_width;
static int path_heap_target_x;
static int path_heap_target_y;

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

static int collision_mask_at(const struct world_state *world, int scene_index,
                             int tile_x, int tile_y)
{
    const struct generated_runtime_scene *scene;
    int walkability = 15;
    int index;
    if (scene_index < 0 || scene_index >= generated_runtime_scene_count)
        return 0;
    scene = &generated_runtime_scenes[scene_index];
    if (tile_x < 0 || tile_y < 0 || tile_x >= scene->width || tile_y >= scene->height)
        return 0;
    for (index = world->collision_override_count - 1; index >= 0; --index) {
        const struct world_collision_override *override = &world->collision_overrides[index];
        if (override->scene == scene_index && override->x == tile_x && override->y == tile_y)
            return override->walkability;
    }
    for (index = 0; index < scene->collision_count; ++index) {
        const struct generated_runtime_collision *collision = &scene->collisions[index];
        int left = collision->x / scene->tile_size;
        int top = collision->y / scene->tile_size;
        int right = (collision->x + collision->width - 1) / scene->tile_size;
        int bottom = (collision->y + collision->height - 1) / scene->tile_size;
        if (tile_x >= left && tile_x <= right && tile_y >= top && tile_y <= bottom)
            /* Legacy mapobjects contributed their directional masks to a
             * mapsquare with bitwise AND. Keeping that composition here also
             * lets Tiled add a small guard over an original transition cell
             * without rewriting the imported collision rectangles. */
            walkability &= collision->walkability;
    }
    return walkability;
}

static int path_edge_is_open(const struct world_state *world, int scene_index,
                             int x, int y, int move_x, int move_y);

static void set_collision_mask(struct world_state *world, int scene_index,
                               int tile_x, int tile_y, int walkability)
{
    int index;
    for (index = 0; index < world->collision_override_count; ++index) {
        struct world_collision_override *override = &world->collision_overrides[index];
        if (override->scene == scene_index && override->x == tile_x && override->y == tile_y) {
            override->walkability = walkability;
            return;
        }
    }
    if (world->collision_override_count >= WORLD_COLLISION_OVERRIDE_CAPACITY)
        return;
    world->collision_overrides[world->collision_override_count].scene = scene_index;
    world->collision_overrides[world->collision_override_count].x = tile_x;
    world->collision_overrides[world->collision_override_count].y = tile_y;
    world->collision_overrides[world->collision_override_count].walkability = walkability;
    ++world->collision_override_count;
}

static int player_definition(const struct world_state *world)
{
    if (world->player_entity < 0 || world->player_entity >= world->scene->entity_count)
        return -1;
    return world->scene->entities[world->player_entity].definition;
}

static int actor_blocks_tile(const struct world_state *world, int definition, int tile_x, int tile_y)
{
    const struct world_actor_state *actor;
    if (definition < 0 || definition >= generated_runtime_actor_count ||
        definition == player_definition(world) ||
        !generated_runtime_actors[definition].blocks_movement ||
        !world_actor_is_present(world, definition))
        return 0;
    actor = &world->actors[definition];
    /* Adonthell 0.3 registered the full sprite footprint for drawing, but
     * mapsquare::is_free() only considered its single base (anchor) square.
     * The upper body therefore never occupied an extra movement tile. */
    return tile_x == actor->x && tile_y == actor->y;
}

static int can_move(const struct world_state *world, int move_x, int move_y)
{
    int entity_index;

    /* Directional map collision and actor occupancy are both defined at the
     * ground anchor. A character's upper sprite cells participate in drawing
     * and sorting, but Adonthell's mapsquare::is_free() did not block them. */
    if (!path_edge_is_open(world, world->scene_index,
                           world->player_x, world->player_y, move_x, move_y))
        return 0;

    for (entity_index = 0; entity_index < generated_runtime_actor_count; ++entity_index)
        if (actor_blocks_tile(world, entity_index,
                               world->player_x + move_x, world->player_y + move_y))
            return 0;
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
    for (index = 0; index < generated_runtime_actor_count; ++index) {
        const struct generated_runtime_actor *entity = &generated_runtime_actors[index];
        const struct world_actor_state *actor = &world->actors[index];
        int dialogue = actor->dialogue >= 0 ?
                       actor->dialogue : entity->interaction_dialogue;
        if (dialogue >= 0 &&
            actor_blocks_tile(world, index, world->player_x + move_x, world->player_y + move_y))
            return index;
    }
    return -1;
}

static int hotspot_script(const struct world_state *world)
{
    int selected = -1;
    int selected_priority = 0;
    int index;
    for (index = 0; index < world->scene->hotspot_count; ++index) {
        const struct generated_runtime_hotspot *hotspot = &world->scene->hotspots[index];
        if (world->player_x < hotspot->x ||
            world->player_x >= hotspot->x + hotspot->width ||
            world->player_y < hotspot->y ||
            world->player_y >= hotspot->y + hotspot->height ||
            (hotspot->facing >= 0 && hotspot->facing != world->player_facing))
            continue;
        if (selected < 0 || hotspot->priority > selected_priority) {
            selected = hotspot->script;
            selected_priority = hotspot->priority;
        }
    }
    return selected;
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
    if (!choice->repeatable && choice->next_node >= 0 &&
        choice->next_node < GENERATED_RUNTIME_DIALOGUE_USED_COUNT)
        world->dialogue_used[choice->next_node] = 1;
    world->dialogue_node = choice->next_node;
}

static int dialogue_choice_is_visible(
    const struct world_state *world,
    const struct generated_runtime_dialogue_choice *choice)
{
    int index;
    if (!choice->repeatable && choice->next_node >= 0 &&
        choice->next_node < GENERATED_RUNTIME_DIALOGUE_USED_COUNT &&
        world->dialogue_used[choice->next_node])
        return 0;
    for (index = 0; index < choice->condition_count; ++index) {
        const struct generated_runtime_dialogue_condition *condition =
            &choice->conditions[index];
        if (condition->flag < 0 ||
            condition->flag >= GENERATED_RUNTIME_FLAG_COUNT ||
            world->flags[condition->flag] != condition->value)
            return 0;
    }
    return 1;
}

static const struct generated_runtime_dialogue_choice *dialogue_visible_choice(
    const struct world_state *world,
    const struct generated_runtime_dialogue_node *node,
    int visible_index)
{
    int index;
    for (index = 0; index < node->choice_count; ++index) {
        if (!dialogue_choice_is_visible(world, &node->choices[index]))
            continue;
        if (visible_index == 0)
            return &node->choices[index];
        --visible_index;
    }
    return 0;
}

static int dialogue_visible_choice_count(
    const struct world_state *world,
    const struct generated_runtime_dialogue_node *node)
{
    int count = 0;
    int index;
    for (index = 0; index < node->choice_count; ++index)
        if (dialogue_choice_is_visible(world, &node->choices[index]))
            ++count;
    return count;
}

static void start_script(struct world_state *world, int script)
{
    if (script < 0 || script >= generated_runtime_script_count)
        return;
    world->active_script = script;
    world->script_action = 0;
    world->script_wait = 0;
    world->script_path_action = -1;
    world->script_path_length = 0;
    world->script_path_index = 0;
}

static int select_player_entity(struct world_state *world)
{
    int index;
    world->player_entity = -1;
    for (index = 0; index < world->scene->entity_count; ++index) {
        if (world->scene->entities[index].player_controlled) {
            world->player_entity = index;
            return 1;
        }
    }
    return 0;
}

static void apply_scene_change(struct world_state *world, int scene_index, int spawn_index)
{
    const struct generated_runtime_scene *scene;
    const struct generated_runtime_spawn *spawn;
    if (scene_index < 0 || scene_index >= generated_runtime_scene_count)
        return;
    scene = &generated_runtime_scenes[scene_index];
    if (spawn_index < 0 || spawn_index >= scene->spawn_count)
        return;
    spawn = &scene->spawns[spawn_index];
    world->scene = scene;
    world->scene_index = scene_index;
    if (!select_player_entity(world))
        return;
    world->player_x = spawn->tile_x;
    world->player_y = spawn->tile_y;
    world->player_facing = spawn->facing;
    world->moving = 0;
    world->move_x = 0;
    world->move_y = 0;
    world->move_progress = 0;
    world->animation_frame = 0;
    world->trigger_latch = -1;
    world->script_path_action = -1;
    world->script_path_length = 0;
    world->script_path_index = 0;
    ++world->visual_revision;
}

static void begin_scene_change(struct world_state *world, int scene_index, int spawn_index)
{
    if (scene_index < 0 || scene_index >= generated_runtime_scene_count)
        return;
    if (spawn_index < 0 || spawn_index >= generated_runtime_scenes[scene_index].spawn_count)
        return;
    world->transition_phase = WORLD_TRANSITION_FADE_OUT;
    world->transition_frame = 0;
    world->transition_scene = scene_index;
    world->transition_spawn = spawn_index;
    ++world->visual_revision;
}

static void update_scene_transition(struct world_state *world)
{
    if (world->transition_phase == WORLD_TRANSITION_FADE_OUT) {
        ++world->transition_frame;
        if (world->transition_frame >= WORLD_TRANSITION_FRAMES) {
            world->transition_frame = WORLD_TRANSITION_FRAMES;
            apply_scene_change(world, world->transition_scene, world->transition_spawn);
            world->transition_phase = WORLD_TRANSITION_FADE_IN;
        }
        ++world->visual_revision;
        return;
    }
    if (world->transition_phase == WORLD_TRANSITION_FADE_IN) {
        --world->transition_frame;
        if (world->transition_frame <= 0) {
            world->transition_frame = 0;
            world->transition_phase = WORLD_TRANSITION_NONE;
        }
        ++world->visual_revision;
    }
}

static int start_position_trigger(struct world_state *world, int when)
{
    int index;
    for (index = 0; index < world->scene->trigger_count; ++index) {
        const struct generated_runtime_trigger *trigger = &world->scene->triggers[index];
        if (trigger->when != when || world->trigger_latch == index + 1)
            continue;
        if (trigger->facing >= 0 && trigger->facing != world->player_facing)
            continue;
        if (trigger->condition_flag >= 0 &&
            (trigger->condition_flag >= GENERATED_RUNTIME_FLAG_COUNT ||
             world->flags[trigger->condition_flag] != trigger->condition_value))
            continue;
        if (world->player_x < trigger->x || world->player_y < trigger->y ||
            world->player_x >= trigger->x + trigger->width ||
            world->player_y >= trigger->y + trigger->height)
            continue;
        world->trigger_latch = index + 1;
        start_script(world, trigger->script);
        return 1;
    }
    return 0;
}

static void close_dialogue(struct world_state *world)
{
    int end_script = -1;
    if (world->dialogue >= 0) {
        const struct generated_runtime_dialogue *dialogue =
            &generated_runtime_dialogues[world->dialogue];
        if (world->dialogue_node >= 0 && world->dialogue_node < dialogue->node_count)
            end_script = dialogue->nodes[world->dialogue_node].end_script;
        if (end_script < 0)
            end_script = dialogue->end_script;
    }
    world->dialogue = -1;
    world->dialogue_node = 0;
    world->dialogue_choice = 0;
    world->dialogue_actor = -1;
    if (end_script >= 0)
        start_script(world, end_script);
}

static void start_dialogue(struct world_state *world, int dialogue)
{
    const struct generated_runtime_dialogue *runtime_dialogue;
    int index;
    if (dialogue < 0 || dialogue >= generated_runtime_dialogue_count)
        return;
    runtime_dialogue = &generated_runtime_dialogues[dialogue];
    for (index = 0; index < GENERATED_RUNTIME_DIALOGUE_USED_COUNT; ++index)
        world->dialogue_used[index] = 0;
    world->dialogue = dialogue;
    world->dialogue_node = 0;
    for (index = 0; index < runtime_dialogue->start_count; ++index) {
        const struct generated_runtime_dialogue_start *start = &runtime_dialogue->starts[index];
        if (start->flag < 0 ||
            (start->flag < GENERATED_RUNTIME_FLAG_COUNT &&
             world->flags[start->flag] == start->value)) {
            world->dialogue_node = start->node;
            break;
        }
    }
    world->dialogue_choice = 0;
}

static int advance_player_motion(struct world_state *world)
{
    if (!world->moving)
        return 1;
    world->move_progress += WALK_PIXELS_PER_FRAME;
    if ((world->move_progress & 3) == 0)
        ++world->animation_frame;
    if (world->move_progress < world->scene->tile_size)
        return 0;
    world->player_x += world->move_x;
    world->player_y += world->move_y;
    world->moving = 0;
    world->move_progress = 0;
    return 1;
}

static int schedule_random_delay(struct world_state *world, int minimum, int maximum)
{
    int range;
    world->schedule_random = (world->schedule_random * 25173 + 13849) % 65536;
    range = maximum - minimum + 1;
    return range > 1 ? minimum + world->schedule_random % range : minimum;
}

static int speech_random_value(struct world_state *world, int minimum, int maximum)
{
    int range;
    world->speech_random = (world->speech_random * 25173 + 13849) % 65536;
    range = maximum - minimum + 1;
    return range > 1 ? minimum + world->speech_random % range : minimum;
}

static int actor_anchor_blocked(const struct world_state *world, int definition,
                                int scene_index, int x, int y)
{
    int other;
    if (definition != player_definition(world) && scene_index == world->scene_index &&
        x == world->player_x && y == world->player_y)
        return 1;
    for (other = 0; other < GENERATED_RUNTIME_ENTITY_COUNT; ++other) {
        const struct world_actor_state *actor = &world->actors[other];
        if (other != definition && actor->scene == scene_index && actor->x == x && actor->y == y)
            return 1;
    }
    return 0;
}

static int scheduled_actor_can_move(const struct world_state *world, int definition,
                                    int move_x, int move_y)
{
    const struct world_actor_state *actor = &world->actors[definition];
    if (actor->scene < 0 || actor->scene >= generated_runtime_scene_count)
        return 0;
    if (!path_edge_is_open(world, actor->scene, actor->x, actor->y,
                           move_x, move_y))
        return 0;
    return !actor_anchor_blocked(world, definition, actor->scene,
                                 actor->x + move_x, actor->y + move_y);
}

static int path_tile_is_blocked(const struct world_state *world, int definition,
                                int scene_index, int tile_x, int tile_y)
{
    int entity_index;
    if (definition >= 0)
        return actor_anchor_blocked(world, definition, scene_index, tile_x, tile_y);
    if (scene_index != world->scene_index)
        return 0;
    for (entity_index = 0; entity_index < generated_runtime_actor_count; ++entity_index)
        if (actor_blocks_tile(world, entity_index, tile_x, tile_y))
            return 1;
    return 0;
}

static int path_tile_can_be_used(const struct generated_runtime_scene *scene,
                                 int tile_x, int tile_y)
{
    int index;
    for (index = 0; index < scene->navigation_blocker_count; ++index) {
        const struct generated_runtime_navigation_blocker *blocker =
            &scene->navigation_blockers[index];
        if (tile_x >= blocker->x && tile_y >= blocker->y &&
            tile_x < blocker->x + blocker->width &&
            tile_y < blocker->y + blocker->height)
            return 0;
    }
    return 1;
}

static int path_heuristic(int node)
{
    int x = node % path_heap_width;
    int y = node / path_heap_width;
    int dx = x - path_heap_target_x;
    int dy = y - path_heap_target_y;
    int diagonal;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    diagonal = dx < dy ? dx : dy;
    /* Adonthell prices a diagonal grid step at 28 versus 20 for a cardinal
     * step. The equivalent 14/10 octile heuristic remains admissible. */
    return diagonal * 14 + (dx + dy - diagonal * 2) * 10;
}

static int path_total_cost(int node)
{
    return path_move_cost[node] + path_heuristic(node);
}

static void path_heap_swap(int first, int second)
{
    int node = path_open_heap[first];
    path_open_heap[first] = path_open_heap[second];
    path_open_heap[second] = node;
    path_heap_position[path_open_heap[first]] = first;
    path_heap_position[path_open_heap[second]] = second;
}

static void path_heap_up(int position)
{
    while (position > 0) {
        int parent = (position - 1) / 2;
        if (path_total_cost(path_open_heap[parent]) <=
            path_total_cost(path_open_heap[position]))
            break;
        path_heap_swap(parent, position);
        position = parent;
    }
}

static void path_heap_down(int position)
{
    for (;;) {
        int left = position * 2 + 1;
        int right = left + 1;
        int smallest = position;
        if (left < path_heap_count &&
            path_total_cost(path_open_heap[left]) <
                path_total_cost(path_open_heap[smallest]))
            smallest = left;
        if (right < path_heap_count &&
            path_total_cost(path_open_heap[right]) <
                path_total_cost(path_open_heap[smallest]))
            smallest = right;
        if (smallest == position)
            return;
        path_heap_swap(position, smallest);
        position = smallest;
    }
}

static void path_heap_push(int node)
{
    int position = path_heap_count++;
    path_open_heap[position] = node;
    path_heap_position[node] = position;
    path_heap_up(position);
}

static int path_heap_pop(void)
{
    int result = path_open_heap[0];
    --path_heap_count;
    path_heap_position[result] = -1;
    if (path_heap_count > 0) {
        path_open_heap[0] = path_open_heap[path_heap_count];
        path_heap_position[path_open_heap[0]] = 0;
        path_heap_down(0);
    }
    return result;
}

static int path_edge_is_open(const struct world_state *world, int scene_index,
                             int x, int y, int move_x, int move_y)
{
    int source = collision_mask_at(world, scene_index, x, y);
    int target = collision_mask_at(world, scene_index, x + move_x, y + move_y);
    if (move_x != 0 &&
        (!(source & direction_mask(move_x, 0)) ||
         !(target & opposite_mask(move_x, 0))))
        return 0;
    if (move_y != 0 &&
        (!(source & direction_mask(0, move_y)) ||
         !(target & opposite_mask(0, move_y))))
        return 0;
    return 1;
}

/* Adonthell's pathfinder expands all eight neighbouring squares and prices a
 * diagonal step at roughly sqrt(2) times a cardinal one. The original vector
 * of moves is materialized in a fixed route slot. */
static int calculate_path(const struct world_state *world, int definition,
                          int scene_index, int start_x, int start_y,
                          int target_x, int target_y, int route_slot)
{
    const struct generated_runtime_scene *scene;
    int width;
    int height;
    int start;
    int target;
    int current = -1;
    int index;
    int reverse_count = 0;
    static const int offsets_x[8] = { 1, 1, 1, 0, -1, -1, -1, 0 };
    static const int offsets_y[8] = { 1, 0, -1, -1, 1, 0, -1, 1 };

    if (scene_index < 0 || scene_index >= generated_runtime_scene_count)
        return 0;
    scene = &generated_runtime_scenes[scene_index];
    width = scene->width;
    height = scene->height;
    if (width <= 0 || width > PATH_MAX_WIDTH || height <= 0 || height > PATH_MAX_HEIGHT ||
        start_x < 0 || start_y < 0 || start_x >= width || start_y >= height ||
        target_x < 0 || target_y < 0 || target_x >= width || target_y >= height)
        return 0;
    if (route_slot < 0 || route_slot >= PATH_ROUTE_SLOT_COUNT)
        return 0;
    if (start_x == target_x && start_y == target_y)
        return 0;
    if (path_tile_is_blocked(world, definition, scene_index, target_x, target_y))
        return 0;

    for (index = 0; index < width * height; ++index) {
        path_node_state[index] = PATH_UNSEEN;
        path_heap_position[index] = -1;
        path_parent_direction[index] = -1;
    }
    start = start_y * width + start_x;
    target = target_y * width + target_x;
    path_heap_count = 0;
    path_heap_width = width;
    path_heap_target_x = target_x;
    path_heap_target_y = target_y;
    path_move_cost[start] = 0;
    path_node_state[start] = PATH_OPEN;
    path_heap_push(start);

    while (path_heap_count > 0) {
        int direction;
        current = path_heap_pop();
        if (current == target)
            break;
        path_node_state[current] = PATH_CLOSED;
        for (direction = 0; direction < 8; ++direction) {
            int x = current % width;
            int y = current / width;
            int next_x = x + offsets_x[direction];
            int next_y = y + offsets_y[direction];
            int next;
            int new_cost;
            if (next_x < 0 || next_y < 0 || next_x >= width || next_y >= height)
                continue;
            if (!path_edge_is_open(world, scene_index, x, y,
                                   offsets_x[direction], offsets_y[direction]))
                continue;
            next = next_y * width + next_x;
            if (next != target && !path_tile_can_be_used(scene, next_x, next_y))
                continue;
            if (path_tile_is_blocked(world, definition, scene_index, next_x, next_y))
                continue;
            new_cost = path_move_cost[current] +
                       (offsets_x[direction] != 0 && offsets_y[direction] != 0 ? 14 : 10);
            if (path_node_state[next] != PATH_UNSEEN && path_move_cost[next] <= new_cost)
                continue;
            path_move_cost[next] = new_cost;
            path_parent_direction[next] = (signed char)direction;
            if (path_node_state[next] == PATH_OPEN)
                path_heap_up(path_heap_position[next]);
            else {
                path_node_state[next] = PATH_OPEN;
                path_heap_push(next);
            }
        }
    }
    if (current != target)
        return 0;

    current = target;
    while (current != start) {
        int parent_direction = path_parent_direction[current];
        int x = current % width;
        int y = current / width;
        if (parent_direction < 0 || reverse_count >= PATH_MAX_TILES)
            return 0;
        path_reverse[reverse_count++] = (unsigned char)parent_direction;
        current = (y - offsets_y[parent_direction]) * width +
                  (x - offsets_x[parent_direction]);
    }
    for (index = 0; index < reverse_count; ++index)
        path_routes[route_slot][index] = path_reverse[reverse_count - index - 1];
    return reverse_count;
}

static void path_direction(int direction, int *move_x, int *move_y)
{
    static const int offsets_x[8] = { 1, 1, 1, 0, -1, -1, -1, 0 };
    static const int offsets_y[8] = { 1, 0, -1, -1, 1, 0, -1, 1 };
    if (direction < 0 || direction >= 8) {
        *move_x = 0;
        *move_y = 0;
        return;
    }
    *move_x = offsets_x[direction];
    *move_y = offsets_y[direction];
}

static void start_actor_schedule(struct world_state *world, int definition,
                                 int schedule, int delay);

static void save_actor_state(struct world_state *world, int definition)
{
    const struct world_actor_state *actor;
    struct world_actor_checkpoint *checkpoint;
    if (definition < 0 || definition >= GENERATED_RUNTIME_ENTITY_COUNT)
        return;
    actor = &world->actors[definition];
    checkpoint = &world->actor_checkpoints[definition];
    checkpoint->valid = 1;
    checkpoint->scene = actor->scene;
    checkpoint->x = actor->x;
    checkpoint->y = actor->y;
    checkpoint->facing = actor->facing;
    checkpoint->animation_frame = actor->animation_frame;
    checkpoint->visual = actor->visual;
    checkpoint->visual_tick = actor->visual_tick;
    checkpoint->schedule = actor->schedule;
    checkpoint->schedule_step = actor->schedule_step;
    checkpoint->schedule_wait = actor->schedule_wait;
    checkpoint->dialogue = actor->dialogue;
}

static int restore_actor_state(struct world_state *world, int definition)
{
    struct world_actor_state *actor;
    struct world_actor_checkpoint *checkpoint;
    if (definition < 0 || definition >= GENERATED_RUNTIME_ENTITY_COUNT)
        return 0;
    checkpoint = &world->actor_checkpoints[definition];
    if (!checkpoint->valid)
        return 0;
    actor = &world->actors[definition];
    actor->scene = checkpoint->scene;
    actor->x = checkpoint->x;
    actor->y = checkpoint->y;
    actor->facing = checkpoint->facing;
    actor->moving = 0;
    actor->move_x = 0;
    actor->move_y = 0;
    actor->move_progress = 0;
    actor->animation_frame = checkpoint->animation_frame;
    actor->visual = checkpoint->visual;
    actor->visual_tick = checkpoint->visual_tick;
    actor->schedule = checkpoint->schedule;
    actor->schedule_step = checkpoint->schedule_step;
    actor->schedule_wait = checkpoint->schedule_wait;
    actor->path_schedule_step = -1;
    actor->path_scene = -1;
    actor->path_length = 0;
    actor->path_index = 0;
    actor->speech_line = -1;
    actor->speech_wait = 0;
    actor->speech_frames = 0;
    actor->speech_override = 0;
    actor->dialogue = checkpoint->dialogue;
    checkpoint->valid = 0;
    ++world->visual_revision;
    return 1;
}

static void finish_schedule_step(struct world_state *world, int definition,
                                 const struct generated_runtime_schedule_step *step)
{
    struct world_actor_state *actor = &world->actors[definition];
    const struct generated_runtime_schedule *schedule =
        &generated_runtime_schedules[actor->schedule];
    actor->schedule_wait = schedule_random_delay(world, step->delay_min, step->delay_max);
    actor->path_schedule_step = -1;
    actor->path_scene = -1;
    actor->path_length = 0;
    actor->path_index = 0;
    ++actor->schedule_step;
    if (actor->schedule_step >= schedule->step_count) {
        if (schedule->loop)
            actor->schedule_step = 0;
        else if (schedule->next_schedule >= 0)
            start_actor_schedule(world, definition, schedule->next_schedule, 0);
        else
            actor->schedule = -1;
    }
}

static void update_actor_schedule(struct world_state *world, int definition)
{
    struct world_actor_state *actor = &world->actors[definition];
    const struct generated_runtime_schedule *schedule;
    const struct generated_runtime_schedule_step *step;
    int move_x = 0;
    int move_y = 0;
    if (actor->schedule < 0 || actor->schedule >= generated_runtime_schedule_count ||
        definition == world->dialogue_actor)
        return;
    if (actor->schedule_wait > 0) {
        --actor->schedule_wait;
        return;
    }
    schedule = &generated_runtime_schedules[actor->schedule];
    if (actor->schedule_step < 0 || actor->schedule_step >= schedule->step_count) {
        actor->schedule = -1;
        return;
    }
    step = &schedule->steps[actor->schedule_step];
    if (actor->moving) {
        actor->move_progress += WALK_PIXELS_PER_FRAME;
        if ((actor->move_progress & 3) == 0)
            ++actor->animation_frame;
        if (actor->move_progress >= generated_runtime_scenes[actor->scene].tile_size) {
            actor->x += actor->move_x;
            actor->y += actor->move_y;
            actor->moving = 0;
            actor->move_progress = 0;
        }
        ++world->visual_revision;
        return;
    }
    if (step->op == GENERATED_SCHEDULE_WAIT) {
        finish_schedule_step(world, definition, step);
        return;
    }
    if (step->op == GENERATED_SCHEDULE_SET_OBJECT_STATE ||
        step->op == GENERATED_SCHEDULE_TOGGLE_OBJECT_STATE) {
        if (step->target >= 0 &&
            step->target < GENERATED_RUNTIME_STATEFUL_STORAGE_COUNT) {
            int state = step->value;
            if (step->op == GENERATED_SCHEDULE_TOGGLE_OBJECT_STATE)
                state = world->object_states[step->target] == 0 ? 1 : 0;
            if (world->object_states[step->target] != state) {
                world->object_states[step->target] = state;
                if (step->scene == world->scene_index)
                    ++world->visual_revision;
            }
        }
        finish_schedule_step(world, definition, step);
        return;
    }
    if (step->op == GENERATED_SCHEDULE_SPEAK) {
        if (step->target >= 0 && step->target < schedule->speech_count) {
            actor->speech_override = 0;
            actor->speech_line = step->target;
            actor->speech_frames = step->value;
            ++world->visual_revision;
        }
        finish_schedule_step(world, definition, step);
        return;
    }
    if (step->op == GENERATED_SCHEDULE_JUMP) {
        actor->schedule_step = step->target;
        actor->path_schedule_step = -1;
        actor->schedule_wait = schedule_random_delay(
            world, step->delay_min, step->delay_max);
        return;
    }
    if (step->op == GENERATED_SCHEDULE_RANDOM_JUMP) {
        int branch;
        if (step->target < 0 || step->value <= 0 ||
            step->target + step->value > schedule->branch_count) {
            actor->schedule = -1;
            return;
        }
        branch = schedule_random_delay(world, 0, step->value - 1);
        actor->schedule_step = schedule->branches[step->target + branch];
        actor->path_schedule_step = -1;
        actor->schedule_wait = schedule_random_delay(
            world, step->delay_min, step->delay_max);
        return;
    }
    if (step->op == GENERATED_SCHEDULE_RESTORE_ACTOR_STATE) {
        if (!restore_actor_state(world, definition))
            finish_schedule_step(world, definition, step);
        return;
    }
    if (step->op == GENERATED_SCHEDULE_FACE) {
        if (definition == player_definition(world))
            world->player_facing = step->facing;
        else
            actor->facing = step->facing;
        ++world->visual_revision;
        finish_schedule_step(world, definition, step);
        return;
    }
    if (step->op == GENERATED_SCHEDULE_PLACE_RANDOM) {
        int index;
        int free_count = 0;
        int selected;
        const struct generated_runtime_schedule_position *position = 0;
        if (step->target < 0 || step->value <= 0 ||
            step->target + step->value > schedule->position_count) {
            actor->schedule = -1;
            return;
        }
        for (index = 0; index < step->value; ++index) {
            const struct generated_runtime_schedule_position *candidate =
                &schedule->positions[step->target + index];
            if (candidate->scene >= 0 && candidate->scene < generated_runtime_scene_count &&
                path_tile_can_be_used(&generated_runtime_scenes[candidate->scene],
                                      candidate->x, candidate->y) &&
                !actor_anchor_blocked(world, definition, candidate->scene,
                                      candidate->x, candidate->y))
                ++free_count;
        }
        if (free_count == 0)
            return;
        selected = schedule_random_delay(world, 0, free_count - 1);
        for (index = 0; index < step->value; ++index) {
            const struct generated_runtime_schedule_position *candidate =
                &schedule->positions[step->target + index];
            if (candidate->scene < 0 || candidate->scene >= generated_runtime_scene_count ||
                !path_tile_can_be_used(&generated_runtime_scenes[candidate->scene],
                                       candidate->x, candidate->y) ||
                actor_anchor_blocked(world, definition, candidate->scene,
                                     candidate->x, candidate->y))
                continue;
            if (selected-- == 0) {
                position = candidate;
                break;
            }
        }
        if (position == 0)
            return;
        if (definition == player_definition(world)) {
            if (position->scene != world->scene_index)
                return;
            world->player_x = position->x;
            world->player_y = position->y;
            world->player_facing = position->facing;
            world->moving = 0;
            world->move_progress = 0;
        }
        else {
            actor->scene = position->scene;
            actor->x = position->x;
            actor->y = position->y;
            actor->facing = position->facing;
            actor->move_progress = 0;
        }
        ++world->visual_revision;
        finish_schedule_step(world, definition, step);
        return;
    }
    if (step->op == GENERATED_SCHEDULE_PLACE) {
        actor->scene = step->scene;
        actor->x = step->x;
        actor->y = step->y;
        actor->facing = step->facing;
        actor->move_progress = 0;
        ++world->visual_revision;
        finish_schedule_step(world, definition, step);
        return;
    }
    if (actor->scene != step->scene)
        return;
    if (actor->x == step->x && actor->y == step->y) {
        actor->facing = step->facing;
        ++world->visual_revision;
        finish_schedule_step(world, definition, step);
        return;
    }
    if (actor->path_schedule_step != actor->schedule_step ||
        actor->path_scene != actor->scene || actor->path_target_x != step->x ||
        actor->path_target_y != step->y ||
        actor->path_index >= actor->path_length) {
        actor->path_length = calculate_path(
            world, definition, actor->scene, actor->x, actor->y,
            step->x, step->y, definition);
        actor->path_index = 0;
        actor->path_schedule_step = actor->schedule_step;
        actor->path_scene = actor->scene;
        actor->path_target_x = step->x;
        actor->path_target_y = step->y;
        if (actor->path_length <= 0)
            return;
    }
    path_direction(path_routes[definition][actor->path_index], &move_x, &move_y);
    actor->facing = facing_for_direction(move_x, move_y);
    if (!scheduled_actor_can_move(world, definition, move_x, move_y)) {
        /* mapcharacter::follow_path recalculated immediately when another
         * character occupied its next mapsquare. Preserve that behavior. */
        actor->path_length = calculate_path(
            world, definition, actor->scene, actor->x, actor->y,
            step->x, step->y, definition);
        actor->path_index = 0;
        if (actor->path_length <= 0)
            return;
        path_direction(path_routes[definition][0], &move_x, &move_y);
        actor->facing = facing_for_direction(move_x, move_y);
        if (!scheduled_actor_can_move(world, definition, move_x, move_y))
            return;
    }
    actor->move_x = move_x;
    actor->move_y = move_y;
    actor->moving = 1;
    actor->move_progress = 0;
    ++actor->path_index;
    ++world->visual_revision;
}

static void update_actor_speech(struct world_state *world, int definition)
{
    struct world_actor_state *actor = &world->actors[definition];
    const struct generated_runtime_schedule *schedule;
    if (world->dialogue >= 0)
        return;
    if (actor->speech_override != 0) {
        if (actor->speech_frames > 0)
            --actor->speech_frames;
        if (actor->speech_frames == 0) {
            actor->speech_override = 0;
            actor->speech_line = -1;
            if (actor->schedule >= 0 && actor->schedule < generated_runtime_schedule_count) {
                schedule = &generated_runtime_schedules[actor->schedule];
                if (schedule->ambient_speech_count > 0)
                    actor->speech_wait = speech_random_value(
                        world, schedule->speech_delay_min, schedule->speech_delay_max);
            }
            ++world->visual_revision;
        }
        return;
    }
    if (actor->schedule < 0 || actor->schedule >= generated_runtime_schedule_count)
        return;
    schedule = &generated_runtime_schedules[actor->schedule];
    if (actor->speech_frames > 0) {
        --actor->speech_frames;
        if (actor->speech_frames == 0) {
            actor->speech_line = -1;
            if (schedule->ambient_speech_count > 0)
                actor->speech_wait = speech_random_value(
                    world, schedule->speech_delay_min, schedule->speech_delay_max);
            ++world->visual_revision;
        }
        return;
    }
    if (schedule->ambient_speech_count <= 0)
        return;
    if (actor->speech_wait > 0) {
        --actor->speech_wait;
        return;
    }
    actor->speech_line = speech_random_value(
        world, 0, schedule->ambient_speech_count - 1);
    actor->speech_frames = schedule->speech_duration;
    ++world->visual_revision;
}

static void update_schedules(struct world_state *world)
{
    int definition;
    for (definition = 0; definition < GENERATED_RUNTIME_ENTITY_COUNT; ++definition) {
        update_actor_schedule(world, definition);
        update_actor_speech(world, definition);
    }
}

static void start_actor_schedule(struct world_state *world, int definition,
                                 int schedule, int delay)
{
    struct world_actor_state *actor;
    if (definition < 0 || definition >= GENERATED_RUNTIME_ENTITY_COUNT ||
        schedule < 0 || schedule >= generated_runtime_schedule_count ||
        generated_runtime_schedules[schedule].entity != definition)
        return;
    actor = &world->actors[definition];
    if (actor->speech_line >= 0)
        ++world->visual_revision;
    actor->schedule = schedule;
    actor->schedule_step = 0;
    actor->schedule_wait = delay;
    actor->path_schedule_step = -1;
    actor->path_scene = -1;
    actor->path_length = 0;
    actor->path_index = 0;
    actor->speech_line = -1;
    actor->speech_frames = 0;
    actor->speech_override = 0;
    if (generated_runtime_schedules[schedule].ambient_speech_count > 0)
        actor->speech_wait = speech_random_value(
            world, generated_runtime_schedules[schedule].speech_delay_min,
            generated_runtime_schedules[schedule].speech_delay_max);
    else
        actor->speech_wait = 0;
}

static void set_entity_state(struct world_state *world, int definition, int state)
{
    int index;
    for (index = 0; index < world->scene->scenery_count; ++index) {
        const struct generated_runtime_scenery *scenery = &world->scene->scenery[index];
        if (scenery->definition != definition || scenery->state_slot < 0 ||
            scenery->state_slot >= GENERATED_RUNTIME_STATEFUL_STORAGE_COUNT)
            continue;
        if (world->object_states[scenery->state_slot] != state) {
            world->object_states[scenery->state_slot] = state;
            ++world->visual_revision;
        }
    }
}

static int begin_script_step(struct world_state *world, int target_x, int target_y, int facing)
{
    int move_x = 0;
    int move_y = 0;
    if (world->player_x == target_x && world->player_y == target_y) {
        world->player_facing = facing;
        world->script_path_action = -1;
        world->script_path_length = 0;
        world->script_path_index = 0;
        return 1;
    }
    if (world->script_path_action != world->script_action ||
        world->script_path_index >= world->script_path_length) {
        world->script_path_length = calculate_path(
            world, -1, world->scene_index, world->player_x, world->player_y,
            target_x, target_y, PATH_PLAYER_SLOT);
        world->script_path_index = 0;
        world->script_path_action = world->script_action;
        if (world->script_path_length <= 0)
            return 0;
    }
    path_direction(path_routes[PATH_PLAYER_SLOT][world->script_path_index],
                   &move_x, &move_y);
    world->player_facing = facing_for_direction(move_x, move_y);
    if (!can_move(world, move_x, move_y)) {
        world->script_path_length = calculate_path(
            world, -1, world->scene_index, world->player_x, world->player_y,
            target_x, target_y, PATH_PLAYER_SLOT);
        world->script_path_index = 0;
        if (world->script_path_length <= 0)
            return 0;
        path_direction(path_routes[PATH_PLAYER_SLOT][0], &move_x, &move_y);
        world->player_facing = facing_for_direction(move_x, move_y);
        if (!can_move(world, move_x, move_y))
            return 0;
    }
    world->move_x = move_x;
    world->move_y = move_y;
    world->moving = 1;
    world->move_progress = 0;
    ++world->script_path_index;
    return 0;
}

static void update_script(struct world_state *world)
{
    const struct generated_runtime_script *script;
    if (world->active_script < 0 || world->active_script >= generated_runtime_script_count)
        return;
    if (world->script_wait > 0) {
        --world->script_wait;
        return;
    }
    script = &generated_runtime_scripts[world->active_script];
    while (world->script_action < script->action_count) {
        const struct generated_runtime_script_action *action = &script->actions[world->script_action];
        if (action->op == GENERATED_SCRIPT_SET_CONTROL) {
            world->control_enabled = action->a;
            ++world->script_action;
            continue;
        }
        if (action->op == GENERATED_SCRIPT_SET_ENTITY_STATE) {
            set_entity_state(world, action->a, action->b);
            ++world->script_action;
            continue;
        }
        if (action->op == GENERATED_SCRIPT_MOVE_ENTITY) {
            int player_definition = world->scene->entities[world->player_entity].definition;
            if (action->a != player_definition) {
                ++world->script_action;
                continue;
            }
            if (world->moving) {
                if (advance_player_motion(world))
                    continue;
                return;
            }
            if (begin_script_step(world, action->b, action->c, action->d)) {
                ++world->script_action;
                continue;
            }
            return;
        }
        if (action->op == GENERATED_SCRIPT_WAIT) {
            world->script_wait = action->a;
            ++world->script_action;
            return;
        }
        if (action->op == GENERATED_SCRIPT_SHOW_DIALOGUE) {
            start_dialogue(world, action->a);
            ++world->script_action;
            return;
        }
        if (action->op == GENERATED_SCRIPT_CHANGE_SCENE) {
            begin_scene_change(world, action->a, action->b);
            ++world->script_action;
            return;
        }
        if (action->op == GENERATED_SCRIPT_PLAY_MUSIC) {
            if (world->music_track != action->a) {
                world->music_track = action->a;
                ++world->music_revision;
            }
            ++world->script_action;
            continue;
        }
        if (action->op == GENERATED_SCRIPT_START_SCHEDULE) {
            start_actor_schedule(world, action->a, action->b, action->c);
            ++world->script_action;
            continue;
        }
        if (action->op == GENERATED_SCRIPT_STOP_SCHEDULE) {
            if (action->a >= 0 && action->a < GENERATED_RUNTIME_ENTITY_COUNT) {
                if (world->actors[action->a].moving ||
                    world->actors[action->a].speech_line >= 0)
                    ++world->visual_revision;
                world->actors[action->a].schedule = -1;
                world->actors[action->a].moving = 0;
                world->actors[action->a].move_progress = 0;
                world->actors[action->a].path_schedule_step = -1;
                world->actors[action->a].path_scene = -1;
                world->actors[action->a].path_length = 0;
                world->actors[action->a].path_index = 0;
                world->actors[action->a].speech_line = -1;
                world->actors[action->a].speech_wait = 0;
                world->actors[action->a].speech_frames = 0;
                world->actors[action->a].speech_override = 0;
            }
            ++world->script_action;
            continue;
        }
        if (action->op == GENERATED_SCRIPT_SET_ENTITY_DIALOGUE) {
            if (action->a >= 0 && action->a < GENERATED_RUNTIME_ENTITY_COUNT)
                world->actors[action->a].dialogue = action->b;
            ++world->script_action;
            continue;
        }
        if (action->op == GENERATED_SCRIPT_SET_WALKABILITY) {
            if (action->a >= 0 && action->a < generated_runtime_scene_count &&
                action->b >= 0 && action->c >= 0 &&
                action->b < generated_runtime_scenes[action->a].width &&
                action->c < generated_runtime_scenes[action->a].height &&
                action->d >= 0 && action->d <= 15)
                set_collision_mask(world, action->a, action->b, action->c, action->d);
            ++world->script_action;
            continue;
        }
        if (action->op == GENERATED_SCRIPT_SPEAK) {
            if (action->a >= 0 && action->a < GENERATED_RUNTIME_ENTITY_COUNT &&
                action->b >= 0 && action->b < script->speech_count && action->c > 0) {
                struct world_actor_state *actor = &world->actors[action->a];
                actor->speech_override = &script->speech[action->b];
                actor->speech_line = -1;
                actor->speech_frames = action->c;
                ++world->visual_revision;
            }
            ++world->script_action;
            continue;
        }
        if (action->op == GENERATED_SCRIPT_SET_FLAG) {
            if (action->a >= 0 && action->a < GENERATED_RUNTIME_FLAG_COUNT)
                world->flags[action->a] = action->b;
            ++world->script_action;
            continue;
        }
        if (action->op == GENERATED_SCRIPT_SET_ACTOR_VISUAL) {
            if (action->a >= 0 && action->a < GENERATED_RUNTIME_ENTITY_COUNT &&
                action->b >= 0 && action->b < generated_runtime_actor_visual_count &&
                world->actors[action->a].visual != action->b) {
                world->actors[action->a].visual = action->b;
                world->actors[action->a].visual_tick = 0;
                world->actors[action->a].animation_frame = 0;
                ++world->visual_revision;
            }
            ++world->script_action;
            continue;
        }
        if (action->op == GENERATED_SCRIPT_SAVE_ACTOR_STATE) {
            save_actor_state(world, action->a);
            ++world->script_action;
            continue;
        }
        if (action->op == GENERATED_SCRIPT_WAIT_SCHEDULE) {
            if (action->a >= 0 && action->a < GENERATED_RUNTIME_ENTITY_COUNT &&
                world->actors[action->a].schedule >= 0)
                return;
            ++world->script_action;
            continue;
        }
        ++world->script_action;
    }
    world->active_script = -1;
    world->script_path_action = -1;
    world->script_path_length = 0;
    world->script_path_index = 0;
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
    int scene_index;
    int entity_index;
    world->scene = &generated_runtime_scenes[generated_runtime_initial_scene];
    world->scene_index = generated_runtime_initial_scene;
    world->player_entity = -1;
    world->player_x = 0;
    world->player_y = 0;
    world->player_facing = WORLD_SOUTH;
    world->moving = 0;
    world->move_x = 0;
    world->move_y = 0;
    world->move_progress = 0;
    world->animation_frame = 0;
    world->dialogue = -1;
    world->dialogue_node = 0;
    world->dialogue_choice = 0;
    world->dialogue_actor = -1;
    for (index = 0; index < GENERATED_RUNTIME_ENTITY_STORAGE_COUNT; ++index) {
        world->actors[index].scene = -1;
        world->actors[index].x = 0;
        world->actors[index].y = 0;
        world->actors[index].facing = WORLD_SOUTH;
        world->actors[index].moving = 0;
        world->actors[index].move_x = 0;
        world->actors[index].move_y = 0;
        world->actors[index].move_progress = 0;
        world->actors[index].animation_frame = 0;
        world->actors[index].visual = -1;
        world->actors[index].visual_tick = 0;
        world->actors[index].schedule = -1;
        world->actors[index].schedule_step = 0;
        world->actors[index].schedule_wait = 0;
        world->actors[index].path_schedule_step = -1;
        world->actors[index].path_scene = -1;
        world->actors[index].path_target_x = 0;
        world->actors[index].path_target_y = 0;
        world->actors[index].path_length = 0;
        world->actors[index].path_index = 0;
        world->actors[index].speech_line = -1;
        world->actors[index].speech_wait = 0;
        world->actors[index].speech_frames = 0;
        world->actors[index].speech_override = 0;
        world->actors[index].dialogue = -1;
        world->actor_checkpoints[index].valid = 0;
        world->actor_checkpoints[index].scene = -1;
        world->actor_checkpoints[index].x = 0;
        world->actor_checkpoints[index].y = 0;
        world->actor_checkpoints[index].facing = WORLD_SOUTH;
        world->actor_checkpoints[index].animation_frame = 0;
        world->actor_checkpoints[index].visual = -1;
        world->actor_checkpoints[index].visual_tick = 0;
        world->actor_checkpoints[index].schedule = -1;
        world->actor_checkpoints[index].schedule_step = 0;
        world->actor_checkpoints[index].schedule_wait = 0;
        world->actor_checkpoints[index].dialogue = -1;
    }
    for (entity_index = 0; entity_index < generated_runtime_actor_count; ++entity_index) {
        const struct generated_runtime_actor *entity = &generated_runtime_actors[entity_index];
        struct world_actor_state *actor = &world->actors[entity_index];
        actor->visual = entity->visual;
        if (entity->player_controlled || entity->initial_scene < 0)
            continue;
        actor->scene = entity->initial_scene;
        actor->x = entity->initial_x;
        actor->y = entity->initial_y;
        actor->facing = entity->initial_facing;
    }
    for (index = 0; index < GENERATED_RUNTIME_FLAG_STORAGE_COUNT; ++index)
        world->flags[index] = 0;
    for (index = 0; index < GENERATED_RUNTIME_STATEFUL_STORAGE_COUNT; ++index)
        world->object_states[index] = 0;
    for (scene_index = 0; scene_index < generated_runtime_scene_count; ++scene_index) {
        const struct generated_runtime_scene *scene = &generated_runtime_scenes[scene_index];
        for (entity_index = 0; entity_index < scene->scenery_count; ++entity_index) {
            const struct generated_runtime_scenery *scenery = &scene->scenery[entity_index];
            if (scenery->state_slot >= 0 &&
                scenery->state_slot < GENERATED_RUNTIME_STATEFUL_STORAGE_COUNT)
                world->object_states[scenery->state_slot] = scenery->initial_state;
        }
    }
    world->active_script = -1;
    world->script_action = 0;
    world->script_wait = 0;
    world->script_path_action = -1;
    world->script_path_length = 0;
    world->script_path_index = 0;
    world->collision_override_count = 0;
    world->trigger_latch = -1;
    world->control_enabled = 1;
    world->music_track = -1;
    world->music_revision = 0;
    world->visual_revision = 0;
    world->transition_phase = WORLD_TRANSITION_NONE;
    world->transition_frame = 0;
    world->transition_scene = -1;
    world->transition_spawn = -1;
    world->schedule_random = 1;
    world->speech_random = 17;
    world->game_time = generated_runtime_initial_game_time;
    world->game_time_subticks = 0;
    if (select_player_entity(world)) {
        const struct generated_runtime_entity *entity = &world->scene->entities[world->player_entity];
        world->player_x = entity->tile_x;
        world->player_y = entity->tile_y;
        world->player_facing = entity->facing;
        if (world->scene->entry_dialogue >= 0)
            start_dialogue(world, world->scene->entry_dialogue);
    }
}

void world_seed_random(struct world_state *world, int seed)
{
    unsigned int value = (unsigned int)seed;
    if (value == 0)
        value = 1;
    world->schedule_random = (int)(value & 0xFFFFu);
    world->speech_random = (int)(((value >> 16) ^ value ^ 0x5A17u) & 0xFFFFu);
}

static void update_actor_visual_animations(struct world_state *world)
{
    int definition;
    for (definition = 0; definition < GENERATED_RUNTIME_ENTITY_COUNT; ++definition) {
        struct world_actor_state *actor = &world->actors[definition];
        const struct generated_runtime_actor_visual *visual;
        int count;
        int delay;
        if (!world_actor_is_present(world, definition) || actor->moving ||
            actor->visual < 0 || actor->visual >= generated_runtime_actor_visual_count)
            continue;
        visual = &generated_runtime_actor_visuals[actor->visual];
        count = visual->standing_count[actor->facing];
        delay = visual->standing_delay[actor->facing];
        if (count <= 1 || delay <= 0)
            continue;
        ++actor->visual_tick;
        if (actor->visual_tick % delay == 0)
            ++world->visual_revision;
    }
}

void world_update(struct world_state *world, const struct world_actions *actions)
{
    int move_x = actions->move_x;
    int move_y = actions->move_y;
    if (world->player_entity < 0)
        return;
    /* The original clock advanced eight tenths-of-a-minute units per real
     * second. Preserve that rate at this engine's 60 Hz simulation frequency
     * with an integer accumulator. */
    world->game_time_subticks += WORLD_GAME_TIME_SUBTICKS_PER_FRAME;
    if (world->game_time_subticks >= WORLD_GAME_TIME_SUBTICKS_PER_UNIT) {
        world->game_time_subticks -= WORLD_GAME_TIME_SUBTICKS_PER_UNIT;
        ++world->game_time;
        /* Ambient map-object animations use the same stable simulation clock
         * as the original engine. Only scenes containing animated scenery need
         * a redraw; ordinary rooms retain the inexpensive dirty replay path. */
        if (world->scene != 0) {
            int scenery_index;
            for (scenery_index = 0; scenery_index < world->scene->scenery_count;
                 ++scenery_index) {
                const struct generated_runtime_scenery *scenery =
                    &world->scene->scenery[scenery_index];
                if (scenery->visual >= 0 &&
                    scenery->visual < generated_runtime_object_visual_count &&
                    generated_runtime_object_visuals[scenery->visual].frame_delay > 0) {
                    ++world->visual_revision;
                    break;
                }
            }
        }
    }
    update_schedules(world);
    update_actor_visual_animations(world);
    if (world->transition_phase != WORLD_TRANSITION_NONE) {
        update_scene_transition(world);
        return;
    }
    if (world->dialogue >= 0) {
        const struct generated_runtime_dialogue *dialogue = &generated_runtime_dialogues[world->dialogue];
        const struct generated_runtime_dialogue_node *node = &dialogue->nodes[world->dialogue_node];
        int choice_count = dialogue_visible_choice_count(world, node);
        if (choice_count > 0 && actions->choice_move != 0) {
            world->dialogue_choice += actions->choice_move;
            if (world->dialogue_choice < 0)
                world->dialogue_choice = choice_count - 1;
            if (world->dialogue_choice >= choice_count)
                world->dialogue_choice = 0;
        }
        if (actions->interact_pressed) {
            if (choice_count > 0) {
                const struct generated_runtime_dialogue_choice *choice =
                    dialogue_visible_choice(world, node, world->dialogue_choice);
                if (choice == 0)
                    return;
                apply_dialogue_choice(world, choice);
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
    if (world->active_script >= 0) {
        update_script(world);
        return;
    }
    if (!world->control_enabled)
        return;
    if (world->moving) {
        if (advance_player_motion(world)) {
            world->trigger_latch = -1;
            start_position_trigger(world, GENERATED_TRIGGER_ENTER);
        }
        return;
    }
    if (actions->interact_pressed) {
        int definition = interaction_target(world);
        if (definition >= 0) {
            const struct generated_runtime_actor *entity = &generated_runtime_actors[definition];
            struct world_actor_state *actor = &world->actors[definition];
            int dialogue = actor->dialogue >= 0 ?
                           actor->dialogue : entity->interaction_dialogue;
            actor->facing = (world->player_facing == WORLD_NORTH) ? WORLD_SOUTH :
                            (world->player_facing == WORLD_SOUTH) ? WORLD_NORTH :
                            (world->player_facing == WORLD_WEST) ? WORLD_EAST : WORLD_WEST;
            world->dialogue_actor = definition;
            ++world->visual_revision;
            start_dialogue(world, dialogue);
        }
        else {
            int script = hotspot_script(world);
            if (script >= 0) {
                start_script(world, script);
                update_script(world);
            }
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
    if (start_position_trigger(world, GENERATED_TRIGGER_LEAVE)) {
        update_script(world);
        if (world->active_script >= 0)
            return;
    }
    world->move_x = move_x;
    world->move_y = move_y;
    world->moving = 1;
    world->move_progress = 0;
}

int world_transition_alpha(const struct world_state *world)
{
    if (world->transition_phase == WORLD_TRANSITION_NONE)
        return 0;
    return (world->transition_frame * 255) / WORLD_TRANSITION_FRAMES;
}

int world_actor_x(const struct world_state *world, int definition)
{
    if (definition == player_definition(world))
        return world->player_x;
    if (definition < 0 || definition >= GENERATED_RUNTIME_ENTITY_COUNT)
        return 0;
    return world->actors[definition].x;
}

int world_actor_y(const struct world_state *world, int definition)
{
    if (definition == player_definition(world))
        return world->player_y;
    if (definition < 0 || definition >= GENERATED_RUNTIME_ENTITY_COUNT)
        return 0;
    return world->actors[definition].y;
}

int world_actor_facing(const struct world_state *world, int definition)
{
    if (definition == player_definition(world))
        return world->player_facing;
    if (definition < 0 || definition >= GENERATED_RUNTIME_ENTITY_COUNT)
        return WORLD_SOUTH;
    return world->actors[definition].facing;
}

int world_actor_is_moving(const struct world_state *world, int definition)
{
    if (definition == player_definition(world))
        return world->moving;
    return definition >= 0 && definition < GENERATED_RUNTIME_ENTITY_COUNT &&
           world->actors[definition].moving;
}

int world_actor_is_present(const struct world_state *world, int definition)
{
    if (definition == player_definition(world))
        return 1;
    return definition >= 0 && definition < GENERATED_RUNTIME_ENTITY_COUNT &&
           world->actors[definition].scene == world->scene_index;
}

int world_actor_pixel_x(const struct world_state *world, int definition)
{
    int x = world_actor_x(world, definition) * world->scene->tile_size;
    if (definition == player_definition(world)) {
        if (world->moving)
            x += world->move_x * world->move_progress;
        return x;
    }
    if (definition >= 0 && definition < GENERATED_RUNTIME_ENTITY_COUNT &&
        world->actors[definition].moving)
        x += world->actors[definition].move_x * world->actors[definition].move_progress;
    return x;
}

int world_actor_pixel_y(const struct world_state *world, int definition)
{
    int y = world_actor_y(world, definition) * world->scene->tile_size;
    if (definition == player_definition(world)) {
        if (world->moving)
            y += world->move_y * world->move_progress;
        return y;
    }
    if (definition >= 0 && definition < GENERATED_RUNTIME_ENTITY_COUNT &&
        world->actors[definition].moving)
        y += world->actors[definition].move_y * world->actors[definition].move_progress;
    return y;
}

int world_actor_animation_frame(const struct world_state *world, int definition)
{
    if (definition == player_definition(world))
        return world->animation_frame;
    if (definition < 0 || definition >= GENERATED_RUNTIME_ENTITY_COUNT)
        return 0;
    return world->actors[definition].animation_frame;
}

int world_actor_visual(const struct world_state *world, int definition)
{
    if (definition < 0 || definition >= GENERATED_RUNTIME_ENTITY_COUNT)
        return -1;
    return world->actors[definition].visual;
}

int world_actor_visual_tick(const struct world_state *world, int definition)
{
    if (definition < 0 || definition >= GENERATED_RUNTIME_ENTITY_COUNT)
        return 0;
    return world->actors[definition].visual_tick;
}

static int entity_definition(const struct world_state *world, int entity_index)
{
    if (entity_index < 0 || entity_index >= world->scene->entity_count)
        return -1;
    return world->scene->entities[entity_index].definition;
}

int world_entity_x(const struct world_state *world, int entity_index)
{
    return world_actor_x(world, entity_definition(world, entity_index));
}

int world_entity_y(const struct world_state *world, int entity_index)
{
    return world_actor_y(world, entity_definition(world, entity_index));
}

int world_entity_facing(const struct world_state *world, int entity_index)
{
    return world_actor_facing(world, entity_definition(world, entity_index));
}

int world_entity_is_moving(const struct world_state *world, int entity_index)
{
    return world_actor_is_moving(world, entity_definition(world, entity_index));
}

int world_entity_is_present(const struct world_state *world, int entity_index)
{
    return world_actor_is_present(world, entity_definition(world, entity_index));
}

int world_entity_pixel_x(const struct world_state *world, int entity_index)
{
    return world_actor_pixel_x(world, entity_definition(world, entity_index));
}

int world_entity_pixel_y(const struct world_state *world, int entity_index)
{
    return world_actor_pixel_y(world, entity_definition(world, entity_index));
}

int world_entity_animation_frame(const struct world_state *world, int entity_index)
{
    return world_actor_animation_frame(world, entity_definition(world, entity_index));
}

const struct generated_runtime_frame *world_scenery_frame(
    const struct world_state *world,
    const struct generated_runtime_scenery *scenery)
{
    const struct generated_runtime_object_visual *visual;
    int state;
    if (scenery->visual < 0 || scenery->visual >= generated_runtime_object_visual_count ||
        (scenery->state_slot >= GENERATED_RUNTIME_STATEFUL_STORAGE_COUNT))
        return &scenery->frame;
    visual = &generated_runtime_object_visuals[scenery->visual];
    if (visual->frame_count <= 0)
        return &scenery->frame;
    if (scenery->state_slot >= 0)
        state = world->object_states[scenery->state_slot];
    else if (visual->frame_delay > 0)
        state = (world->game_time / visual->frame_delay) % visual->frame_count;
    else
        state = 0;
    if (state < 0) state = 0;
    if (state >= visual->frame_count) state = visual->frame_count - 1;
    return &visual->frames[state];
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
    const struct generated_runtime_dialogue_choice *choice;
    if (world->dialogue < 0)
        return "";
    node = &generated_runtime_dialogues[world->dialogue].nodes[world->dialogue_node];
    if (index < 0)
        return "";
    choice = dialogue_visible_choice(world, node, index);
    return choice != 0 ? choice->text : "";
}

int world_dialogue_choice_count(const struct world_state *world)
{
    if (world->dialogue < 0)
        return 0;
    return dialogue_visible_choice_count(
        world, &generated_runtime_dialogues[world->dialogue].nodes[world->dialogue_node]);
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
    const struct generated_runtime_dialogue_choice *choice;
    if (world->dialogue < 0)
        return 0;
    node = &generated_runtime_dialogues[world->dialogue].nodes[world->dialogue_node];
    if (index < 0)
        return 0;
    choice = dialogue_visible_choice(world, node, index);
    return choice != 0 ? &choice->display : 0;
}

const struct generated_runtime_text *world_entity_speech(
    const struct world_state *world, int entity_index)
{
    return world_actor_speech(world, entity_definition(world, entity_index));
}

const struct generated_runtime_text *world_actor_speech(
    const struct world_state *world, int definition)
{
    const struct world_actor_state *actor;
    const struct generated_runtime_schedule *schedule;
    if (definition < 0 || definition >= GENERATED_RUNTIME_ENTITY_COUNT)
        return 0;
    actor = &world->actors[definition];
    if (actor->speech_override != 0)
        return actor->speech_override;
    if (actor->schedule < 0 ||
        actor->schedule >= generated_runtime_schedule_count || actor->speech_line < 0)
        return 0;
    schedule = &generated_runtime_schedules[actor->schedule];
    if (actor->speech_line >= schedule->speech_count)
        return 0;
    return &schedule->speech[actor->speech_line];
}
