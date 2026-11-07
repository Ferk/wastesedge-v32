/* Data-driven achievement progress and independently persistent unlock state. */
#include "engine/achievements.h"

volatile unsigned int achievement_ram[4];
static unsigned int pending_notifications;

/* Start the application with a recognizable, versioned RAM record. */
void achievements_initialize(void)
{
    achievement_ram[0] = ACHIEVEMENT_RAM_MAGIC;
    achievement_ram[1] = ACHIEVEMENT_RAM_VERSION;
    achievement_ram[2] = 0;
    achievement_ram[3] = 0;
    pending_notifications = 0;
}

/* Reset unfinished progress for a new playthrough without erasing earned badges. */
void achievements_begin_run(unsigned int *progress)
{
    int index;
    for (index = 0; index < GENERATED_RUNTIME_ACHIEVEMENT_COUNT; ++index)
        progress[index] = generated_runtime_achievements[index].initial;
}

/* OR one event bit and unlock only on an exact match, as in Adonthell. */
int achievements_update(unsigned int *progress, int achievement, int bit)
{
    unsigned int mask;
    if (achievement < 0 || achievement >= GENERATED_RUNTIME_ACHIEVEMENT_COUNT || bit < 0 || bit > 31)
        return 0;
    mask = 1u << achievement;
    if (achievement_ram[2] & mask)
        return 0;
    progress[achievement] |= 1u << bit;
    if (progress[achievement] != generated_runtime_achievements[achievement].expected)
        return 0;
    achievement_ram[2] |= mask;
    pending_notifications |= mask;
    return 1;
}

/* Apply all hooks belonging to a node (0), a choice (1), or a script start (2). */
int achievements_event(unsigned int *progress, int kind, int source, int node, int choice)
{
    int index;
    int unlocked = 0;
    for (index = 0; index < generated_runtime_achievement_event_count; ++index) {
        const struct generated_runtime_achievement_event *event = &generated_runtime_achievement_events[index];
        if (event->kind == kind && event->source == source && event->node == node && event->choice == choice)
            unlocked |= achievements_update(progress, event->achievement, event->bit);
    }
    return unlocked;
}

/* Return profile unlocks independently of the currently loaded save slot. */
unsigned int achievements_unlocked(void)
{
    return achievement_ram[2];
}

/* Merge validated card data silently; loading a profile never produces a toast. */
void achievements_merge(unsigned int unlocked)
{
    int index;
    for (index = 0; index < GENERATED_RUNTIME_ACHIEVEMENT_COUNT; ++index)
        if (unlocked & (1u << index))
            achievement_ram[2] |= 1u << index;
}

/* Consume one queued unlock, preserving simultaneous achievements for later display. */
int achievements_take_notification(void)
{
    int index;
    for (index = 0; index < GENERATED_RUNTIME_ACHIEVEMENT_COUNT; ++index) {
        if (pending_notifications & (1u << index)) {
            pending_notifications &= ~(1u << index);
            return index;
        }
    }
    return -1;
}

/* Advertise genuine gameplay rather than title/menu/save-preview state to external observers. */
void achievements_set_active(int active)
{
    achievement_ram[3] = active != 0;
}
