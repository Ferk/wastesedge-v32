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
}

int main(void)
{
    struct world_state world;
    struct world_actions actions;
    int simulation_frame;

    world_initialize(&world);
    platform_initialize();
    simulation_frame = platform_frame_counter();

    for (;;) {
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

        platform_poll_actions(&actions);
        for (update = 0; update < update_count; ++update) {
            world_update(&world, &actions);
            /* Held movement applies to catch-up updates, but button and choice
             * edges must only be consumed once. */
            consume_action_edges(&actions);
        }
        platform_update_audio(&world);
        platform_render_world(&world);
        platform_present();
    }
}
