/* Exact-mask achievement rules, persistent unlocks and a public RAM observation record. */
#ifndef WASTESEDGE_ENGINE_ACHIEVEMENTS_H
#define WASTESEDGE_ENGINE_ACHIEVEMENTS_H

#include "content_scene_data.h"

#define ACHIEVEMENT_RAM_MAGIC 0x57454143u
#define ACHIEVEMENT_RAM_VERSION 1u
/* Located through the link map: magic, schema version, unlocked bits, active gameplay. */
extern volatile unsigned int achievement_ram[4];

void achievements_initialize(void);
void achievements_begin_run(unsigned int *progress);
int achievements_event(unsigned int *progress, int kind, int source, int node, int choice);
int achievements_update(unsigned int *progress, int achievement, int bit);
unsigned int achievements_unlocked(void);
void achievements_merge(unsigned int unlocked);
int achievements_take_notification(void);
void achievements_set_active(int active);

#endif
