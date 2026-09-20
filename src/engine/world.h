#ifndef WASTESEDGE_ENGINE_WORLD_H
#define WASTESEDGE_ENGINE_WORLD_H

#include "content_scene_data.h"

enum world_facing {
    WORLD_NORTH = 0,
    WORLD_SOUTH = 1,
    WORLD_WEST = 2,
    WORLD_EAST = 3
};

struct world_actions {
    int move_x;
    int move_y;
    int interact_pressed;
    int choice_move;
};

struct world_state {
    const struct generated_runtime_scene *scene;
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
    int flags[GENERATED_RUNTIME_FLAG_STORAGE_COUNT];
};

void world_initialize(struct world_state *world);
void world_update(struct world_state *world, const struct world_actions *actions);
int world_entity_x(const struct world_state *world, int entity_index);
int world_entity_y(const struct world_state *world, int entity_index);
int world_entity_facing(const struct world_state *world, int entity_index);
int world_entity_is_moving(const struct world_state *world, int entity_index);
int world_dialogue_is_open(const struct world_state *world);
const char *world_dialogue_text(const struct world_state *world);
const char *world_dialogue_choice_text(const struct world_state *world);
const char *world_dialogue_choice_at(const struct world_state *world, int index);
int world_dialogue_choice_count(const struct world_state *world);
int world_dialogue_choice_selected(const struct world_state *world);
const struct generated_runtime_dialogue_speaker *world_dialogue_speaker(const struct world_state *world);
const struct generated_runtime_text *world_dialogue_display(const struct world_state *world);
const struct generated_runtime_text *world_dialogue_choice_display(const struct world_state *world, int index);

#endif
