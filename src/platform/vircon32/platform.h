#ifndef WASTESEDGE_PLATFORM_VIRCON32_PLATFORM_H
#define WASTESEDGE_PLATFORM_VIRCON32_PLATFORM_H

#include "engine/world.h"

#define PLATFORM_SAVE_SLOT_COUNT 4

enum platform_save_slot_state {
    PLATFORM_SAVE_SLOT_INVALID = -1,
    PLATFORM_SAVE_SLOT_EMPTY = 0,
    PLATFORM_SAVE_SLOT_USED = 1
};

enum platform_storage_result {
    PLATFORM_STORAGE_OK = 0,
    PLATFORM_STORAGE_NO_CARD = 1,
    PLATFORM_STORAGE_FOREIGN_CARD = 2,
    PLATFORM_STORAGE_EMPTY_SLOT = 3,
    PLATFORM_STORAGE_CORRUPT_SLOT = 4,
    PLATFORM_STORAGE_INCOMPATIBLE_SAVE = 5,
    PLATFORM_STORAGE_WRITE_FAILED = 6,
    PLATFORM_STORAGE_SAVED = 7
};

void platform_initialize(void);
int platform_random_seed(void);
void platform_poll_actions(struct world_actions *actions);
int platform_frame_counter(void);
void platform_play_music_track(int track_index);
void platform_update_audio(const struct world_state *world);
void platform_stop_audio(void);
void platform_begin_cutscene(int dialogue, int start_node);
int platform_update_cutscene(const struct world_actions *actions);
int platform_cutscene_node(void);
void platform_render_cutscene(void);
void platform_render_world(const struct world_state *world);
void platform_begin_menu(void);
void platform_invalidate_world(void);
void platform_render_title_menu(int selected, int storage_result);
void platform_render_pause_menu(int selected, int storage_result);
void platform_render_slot_menu(int from_title, int saving, int selected,
                               const int slot_states[PLATFORM_SAVE_SLOT_COUNT],
                               int storage_result);
int platform_query_save_slots(int slot_states[PLATFORM_SAVE_SLOT_COUNT]);
int platform_save_slot(const struct world_state *world, int slot);
int platform_load_slot(struct world_state *world, int slot);
void platform_present(void);

#endif
