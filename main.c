#include "engine/world.h"
#include "platform/vircon32/platform.h"

#if defined(__clang__)
#define MAIN_NOINLINE __attribute__((noinline))
#else
#define MAIN_NOINLINE
#endif

static MAIN_NOINLINE void consume_action_edges(struct world_actions *actions)
{
    *(volatile int *)&actions->interact_pressed = 0;
    *(volatile int *)&actions->choice_move = 0;
    *(volatile int *)&actions->cancel_pressed = 0;
    *(volatile int *)&actions->menu_pressed = 0;
}

enum application_mode {
    APPLICATION_TITLE = 0,
    APPLICATION_PLAYING = 1,
    APPLICATION_PAUSED = 2,
    APPLICATION_LOAD_FROM_TITLE = 3,
    APPLICATION_LOAD_FROM_PAUSE = 4,
    APPLICATION_SAVE_FROM_PAUSE = 5,
    APPLICATION_CUTSCENE = 6
};

static int moved_choice(int current, int movement, int count)
{
    if (movement == 0)
        return current;
    current += movement;
    if (current < 0)
        current = count - 1;
    if (current >= count)
        current = 0;
    return current;
}

/* Start a compiled cutscene while keeping the current world paused in memory. */
static void begin_world_cutscene(struct world_state *world)
{
    platform_update_audio(world);
    platform_begin_cutscene(world->dialogue, world->dialogue_node);
}

int main(void)
{
    struct world_state world;
    struct world_actions actions;
    int slot_states[PLATFORM_SAVE_SLOT_COUNT];
    int mode = APPLICATION_TITLE;
    int menu_choice = 0;
    int slot_choice = 0;
    int storage_result;
    int simulation_frame;
    int cutscene_from_world = 0;

    world_initialize(&world);
    platform_initialize();
    platform_play_music_track(generated_runtime_title_music);
    world_seed_random(&world, platform_random_seed());
    storage_result = platform_query_save_slots(slot_states);
    simulation_frame = platform_frame_counter();

    for (;;) {
        platform_poll_actions(&actions);

        if (mode == APPLICATION_PLAYING) {
            if (world_dialogue_is_cutscene(&world)) {
                begin_world_cutscene(&world);
                cutscene_from_world = 1;
                mode = APPLICATION_CUTSCENE;
            } else if (actions.menu_pressed) {
                mode = APPLICATION_PAUSED;
                menu_choice = 0;
                storage_result = PLATFORM_STORAGE_OK;
                platform_begin_menu();
            } else {
                int current_frame = platform_frame_counter();
                int update_count = current_frame - simulation_frame;
                int update;
                if (update_count < 1)
                    update_count = 1;
                if (update_count > 4) {
                    update_count = 4;
                    simulation_frame = current_frame;
                } else {
                    simulation_frame += update_count;
                }
                for (update = 0; update < update_count; ++update) {
                    world_update(&world, &actions);
                    if (world_dialogue_is_cutscene(&world)) {
                        begin_world_cutscene(&world);
                        cutscene_from_world = 1;
                        mode = APPLICATION_CUTSCENE;
                        break;
                    }
                    /* Held movement applies to catch-up updates, but button
                     * and menu edges must only be consumed once. */
                    consume_action_edges(&actions);
                }
            }
        } else if (mode == APPLICATION_TITLE) {
            menu_choice = moved_choice(menu_choice, actions.choice_move, 2);
            if (actions.interact_pressed) {
                if (menu_choice == 0) {
                    platform_stop_audio();
                    world_initialize(&world);
                    world_seed_random(&world, platform_random_seed());
                    platform_invalidate_world();
                    simulation_frame = platform_frame_counter();
                    if (generated_runtime_new_game_cutscene >= 0) {
                        int dialogue = generated_runtime_new_game_cutscene;
                        int start_node =
                            generated_runtime_dialogues[dialogue].starts[0].node;
                        platform_begin_cutscene(dialogue, start_node);
                        cutscene_from_world = 0;
                        mode = APPLICATION_CUTSCENE;
                    } else {
                        mode = APPLICATION_PLAYING;
                    }
                } else {
                    storage_result = platform_query_save_slots(slot_states);
                    slot_choice = 0;
                    mode = APPLICATION_LOAD_FROM_TITLE;
                }
            }
        } else if (mode == APPLICATION_CUTSCENE) {
            if (platform_update_cutscene(&actions)) {
                if (cutscene_from_world) {
                    world_finish_cutscene(&world, platform_cutscene_node());
                    if (world_dialogue_is_cutscene(&world)) {
                        begin_world_cutscene(&world);
                    } else {
                        platform_invalidate_world();
                        simulation_frame = platform_frame_counter();
                        mode = APPLICATION_PLAYING;
                    }
                } else {
                    platform_stop_audio();
                    platform_invalidate_world();
                    simulation_frame = platform_frame_counter();
                    mode = APPLICATION_PLAYING;
                }
            }
        } else if (mode == APPLICATION_PAUSED) {
            menu_choice = moved_choice(menu_choice, actions.choice_move, 4);
            if (actions.menu_pressed || actions.cancel_pressed ||
                (actions.interact_pressed && menu_choice == 0)) {
                platform_invalidate_world();
                simulation_frame = platform_frame_counter();
                mode = APPLICATION_PLAYING;
            } else if (actions.interact_pressed && menu_choice == 1) {
                storage_result = platform_query_save_slots(slot_states);
                slot_choice = 0;
                mode = APPLICATION_SAVE_FROM_PAUSE;
            } else if (actions.interact_pressed && menu_choice == 2) {
                storage_result = platform_query_save_slots(slot_states);
                slot_choice = 0;
                mode = APPLICATION_LOAD_FROM_PAUSE;
            } else if (actions.interact_pressed && menu_choice == 3) {
                platform_stop_audio();
                storage_result = platform_query_save_slots(slot_states);
                menu_choice = 0;
                mode = APPLICATION_TITLE;
                platform_play_music_track(generated_runtime_title_music);
            }
        } else {
            int from_title = mode == APPLICATION_LOAD_FROM_TITLE;
            int saving = mode == APPLICATION_SAVE_FROM_PAUSE;
            slot_choice = moved_choice(slot_choice, actions.choice_move,
                                       PLATFORM_SAVE_SLOT_COUNT);
            if (actions.cancel_pressed || actions.menu_pressed) {
                storage_result = PLATFORM_STORAGE_OK;
                if (from_title) {
                    menu_choice = 1;
                    mode = APPLICATION_TITLE;
                } else {
                    menu_choice = saving ? 1 : 2;
                    mode = APPLICATION_PAUSED;
                }
            } else if (actions.interact_pressed) {
                if (saving) {
                    int query_result;
                    storage_result = platform_save_slot(&world, slot_choice);
                    query_result = platform_query_save_slots(slot_states);
                    if (storage_result == PLATFORM_STORAGE_SAVED &&
                        query_result != PLATFORM_STORAGE_OK)
                        storage_result = query_result;
                } else {
                    storage_result = platform_load_slot(&world, slot_choice);
                    if (storage_result == PLATFORM_STORAGE_OK) {
                        platform_invalidate_world();
                        simulation_frame = platform_frame_counter();
                        if (world_dialogue_is_cutscene(&world)) {
                            begin_world_cutscene(&world);
                            cutscene_from_world = 1;
                            mode = APPLICATION_CUTSCENE;
                        } else {
                            mode = APPLICATION_PLAYING;
                        }
                    }
                }
            }
        }

        if (mode == APPLICATION_PLAYING) {
            platform_update_audio(&world);
            platform_render_world(&world);
        } else if (mode == APPLICATION_CUTSCENE) {
            platform_render_cutscene();
        } else if (mode == APPLICATION_TITLE) {
            platform_render_title_menu(menu_choice, storage_result);
        } else if (mode == APPLICATION_PAUSED) {
            platform_render_pause_menu(menu_choice, storage_result);
        } else {
            platform_render_slot_menu(mode == APPLICATION_LOAD_FROM_TITLE,
                                      mode == APPLICATION_SAVE_FROM_PAUSE,
                                      slot_choice, slot_states, storage_result);
        }
        platform_present();
    }
}
