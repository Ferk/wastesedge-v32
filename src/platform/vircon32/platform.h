#ifndef WASTESEDGE_PLATFORM_VIRCON32_PLATFORM_H
#define WASTESEDGE_PLATFORM_VIRCON32_PLATFORM_H

#include "engine/world.h"

void platform_initialize(void);
int platform_random_seed(void);
void platform_poll_actions(struct world_actions *actions);
int platform_frame_counter(void);
void platform_update_audio(const struct world_state *world);
void platform_render_world(const struct world_state *world);
void platform_present(void);

#endif
