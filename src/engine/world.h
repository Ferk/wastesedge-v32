#ifndef WASTESEDGE_ENGINE_WORLD_H
#define WASTESEDGE_ENGINE_WORLD_H

#include "content_scene_data.h"

enum world_facing {
    WORLD_NORTH = 0,
    WORLD_SOUTH = 1,
    WORLD_WEST = 2,
    WORLD_EAST = 3
};

#define WORLD_COLLISION_OVERRIDE_CAPACITY 16
#define WORLD_TRANSITION_FRAMES 8

enum world_transition_phase {
    WORLD_TRANSITION_NONE = 0,
    WORLD_TRANSITION_FADE_OUT = 1,
    WORLD_TRANSITION_FADE_IN = 2
};

struct world_collision_override {
    int scene;
    int x;
    int y;
    int walkability;
};

struct world_actions {
    int move_x;
    int move_y;
    int interact_pressed;
    int choice_move;
};

struct world_actor_state {
    int scene;
    int x;
    int y;
    int facing;
    int moving;
    int move_x;
    int move_y;
    int move_progress;
    int animation_frame;
    int visual;
    int visual_tick;
    int schedule;
    int schedule_step;
    int schedule_wait;
    int path_schedule_step;
    int path_scene;
    int path_target_x;
    int path_target_y;
    int path_length;
    int path_index;
    int speech_line;
    int speech_wait;
    int speech_frames;
    const struct generated_runtime_text *speech_override;
    int dialogue;
};

struct world_state {
    const struct generated_runtime_scene *scene;
    int scene_index;
    int player_entity;
    int player_x;
    int player_y;
    int player_facing;
    int moving;
    int move_x;
    int move_y;
    int move_progress;
    int animation_frame;
    int dialogue;
    int dialogue_node;
    int dialogue_choice;
    int dialogue_actor;
    unsigned char dialogue_used[GENERATED_RUNTIME_DIALOGUE_USED_STORAGE_COUNT];
    struct world_actor_state actors[GENERATED_RUNTIME_ENTITY_STORAGE_COUNT];
    int flags[GENERATED_RUNTIME_FLAG_STORAGE_COUNT];
    int object_states[GENERATED_RUNTIME_STATEFUL_STORAGE_COUNT];
    int active_script;
    int script_action;
    int script_wait;
    int script_path_action;
    int script_path_length;
    int script_path_index;
    struct world_collision_override collision_overrides[WORLD_COLLISION_OVERRIDE_CAPACITY];
    int collision_override_count;
    int trigger_latch;
    int control_enabled;
    int music_track;
    int music_revision;
    int visual_revision;
    int transition_phase;
    int transition_frame;
    int transition_scene;
    int transition_spawn;
    int schedule_random;
    int speech_random;
};

void world_initialize(struct world_state *world);
void world_seed_random(struct world_state *world, int seed);
void world_update(struct world_state *world, const struct world_actions *actions);
int world_transition_alpha(const struct world_state *world);
int world_entity_x(const struct world_state *world, int entity_index);
int world_entity_y(const struct world_state *world, int entity_index);
int world_entity_facing(const struct world_state *world, int entity_index);
int world_entity_is_moving(const struct world_state *world, int entity_index);
int world_entity_is_present(const struct world_state *world, int entity_index);
int world_entity_pixel_x(const struct world_state *world, int entity_index);
int world_entity_pixel_y(const struct world_state *world, int entity_index);
int world_entity_animation_frame(const struct world_state *world, int entity_index);
int world_actor_x(const struct world_state *world, int definition);
int world_actor_y(const struct world_state *world, int definition);
int world_actor_facing(const struct world_state *world, int definition);
int world_actor_is_moving(const struct world_state *world, int definition);
int world_actor_is_present(const struct world_state *world, int definition);
int world_actor_pixel_x(const struct world_state *world, int definition);
int world_actor_pixel_y(const struct world_state *world, int definition);
int world_actor_animation_frame(const struct world_state *world, int definition);
int world_actor_visual(const struct world_state *world, int definition);
int world_actor_visual_tick(const struct world_state *world, int definition);
const struct generated_runtime_frame *world_scenery_frame(
    const struct world_state *world,
    const struct generated_runtime_scenery *scenery);
int world_dialogue_is_open(const struct world_state *world);
const char *world_dialogue_text(const struct world_state *world);
const char *world_dialogue_choice_text(const struct world_state *world);
const char *world_dialogue_choice_at(const struct world_state *world, int index);
int world_dialogue_choice_count(const struct world_state *world);
int world_dialogue_choice_selected(const struct world_state *world);
const struct generated_runtime_dialogue_speaker *world_dialogue_speaker(const struct world_state *world);
const struct generated_runtime_text *world_dialogue_display(const struct world_state *world);
const struct generated_runtime_text *world_dialogue_choice_display(const struct world_state *world, int index);
const struct generated_runtime_text *world_entity_speech(
    const struct world_state *world, int entity_index);
const struct generated_runtime_text *world_actor_speech(
    const struct world_state *world, int definition);

#endif
