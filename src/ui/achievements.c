/* Queue earned achievement badges without losing simultaneous unlocks. */
#include "ui/achievements.h"
#include "engine/achievements.h"

#define ACHIEVEMENT_NOTICE_FRAMES 250
static int current = -1;
static int remaining;
static int previous_frame;

/* Advance notifications only while gameplay is visible; report changes for redraw invalidation. */
int achievement_notice_update(int frame, int active)
{
    int before = current;
    int elapsed = frame - previous_frame;
    previous_frame = frame;
    if (!active)
        return 0;
    if (elapsed < 0 || elapsed > 4)
        elapsed = 1;
    if (current >= 0) {
        remaining -= elapsed;
        if (remaining <= 0)
            current = -1;
    }
    if (current < 0) {
        current = achievements_take_notification();
        remaining = ACHIEVEMENT_NOTICE_FRAMES;
    }
    return current != before;
}

/* Return the displayed badge, or -1 when no notification is visible. */
int achievement_notice_current(void)
{
    return current;
}
