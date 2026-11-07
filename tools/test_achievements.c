/* Host checks for original exact-mask rules, dialogue hooks, notices and saved progress. */
#include <assert.h>
#include <stdio.h>
#include "engine/achievements.h"
#include "engine/savegame.h"
#include "engine/world.h"
#include "ui/achievements.h"
#include "ui/text_layout.h"
#include "content_ids.h"

/* Cover every original rule, including initial bits, poison bits and bit 31. */
static void test_rules(void)
{
    unsigned int progress[GENERATED_RUNTIME_ACHIEVEMENT_STORAGE_COUNT];
    int index;
    int bit;
    for (index = 0; index < GENERATED_RUNTIME_ACHIEVEMENT_COUNT; ++index) {
        const struct generated_runtime_achievement *rule = &generated_runtime_achievements[index];
        achievements_initialize();
        achievements_begin_run(progress);
        assert(progress[index] == rule->initial);
        for (bit = 0; bit < 32; ++bit)
            if (rule->expected & (1u << bit))
                achievements_update(progress, index, bit);
        assert(achievements_unlocked() == (1u << index));
        assert(achievements_take_notification() == index);
        assert(achievements_take_notification() == -1);
        achievements_begin_run(progress);
        assert(achievements_unlocked() == (1u << index));
        for (bit = 0; bit < 32; ++bit)
            achievements_update(progress, index, bit);
        assert(achievements_take_notification() == -1);
        assert(progress[index] == rule->initial);
        /* A bit outside the expected mask disqualifies this entire run. */
        achievements_initialize();
        achievements_begin_run(progress);
        for (bit = 0; bit < 32; ++bit)
            if (!(rule->expected & (1u << bit))) {
                achievements_update(progress, index, bit);
                break;
            }
        for (bit = 0; bit < 32; ++bit)
            if (rule->expected & (1u << bit))
                achievements_update(progress, index, bit);
        assert(achievements_unlocked() == 0);
    }
}

/* Saved unfinished progress must survive without rolling back global achievements. */
static void test_save(void)
{
    struct world_state world;
    struct world_state restored;
    int words[SAVEGAME_MAX_WORDS];
    int count;
    achievements_initialize();
    world_initialize(&world);
    achievements_update(world.achievement_progress, 0, 31);
    count = savegame_encode(&world, words, SAVEGAME_MAX_WORDS);
    assert(count > 0 && count <= SAVEGAME_MAX_WORDS);
    achievements_update(world.achievement_progress, 0, 11);
    achievements_update(world.achievement_progress, 0, 21);
    assert(achievements_unlocked() == 1);
    assert(savegame_decode(&restored, words, count));
    assert(restored.achievement_progress[0] == (1u << 31));
    assert(achievements_unlocked() == 1);
    printf("Achievement save payload: %d/%d words\n", count, SAVEGAME_MAX_WORDS);
}

/* Exercise a real script-start hook, then a real player choice in world_update. */
static void test_hooks(void)
{
    struct world_state world;
    struct world_actions actions = { 0 };
    int index;
    int found = 0;
    achievements_initialize();
    world_initialize(&world);
    achievements_event(world.achievement_progress, 2, GENERATED_SCRIPT_EXAMINE_CELLAR_WINE_TASTY, -1, -1);
    achievements_event(world.achievement_progress, 2, GENERATED_SCRIPT_EXAMINE_CELLAR_WINE_STRONG, -1, -1);
    achievements_event(world.achievement_progress, 2, GENERATED_SCRIPT_EXAMINE_CELLAR_WINE_SIP, -1, -1);
    assert(achievements_unlocked() == 1);
    for (index = 0; index < generated_runtime_achievement_event_count; ++index) {
        const struct generated_runtime_achievement_event *event = &generated_runtime_achievement_events[index];
        if (event->kind == 1 && event->achievement == 2) {
            world.dialogue = event->source;
            world.dialogue_node = event->node;
            world.dialogue_choice = event->choice;
            actions.interact_pressed = 1;
            world_update(&world, &actions);
            assert(achievements_unlocked() & (1u << 2));
            found = 1;
            break;
        }
    }
    assert(found);
}

/* Profile merging is silent, and simultaneous unlock notices survive pausing. */
static void test_notifications(void)
{
    unsigned int progress[GENERATED_RUNTIME_ACHIEVEMENT_STORAGE_COUNT];
    achievements_initialize();
    achievements_begin_run(progress);
    achievements_merge(1u << 1);
    assert(achievements_take_notification() == -1);
    achievements_update(progress, 2, 11);
    achievements_update(progress, 3, 22);
    assert(achievement_notice_update(1, 1));
    assert(achievement_notice_current() == 2);
    achievement_notice_update(2, 0);
    achievement_notice_update(1000, 0);
    assert(achievement_notice_current() == 2);
    for (int frame = 1001; frame < 1251; ++frame)
        achievement_notice_update(frame, 1);
    assert(achievement_notice_current() == 3);
    for (int frame = 1251; frame < 1501; ++frame)
        achievement_notice_update(frame, 1);
    assert(achievement_notice_current() == -1);
}

/* Report localized row heights so long translated descriptions are not silently clipped. */
static void test_layout(void)
{
    for (int language = 0; language < generated_runtime_language_count; ++language) {
        world_set_language(language);
        for (int index = 0; index < GENERATED_RUNTIME_ACHIEVEMENT_COUNT; ++index) {
            int width;
            int height;
            const struct generated_runtime_achievement *entry = &generated_runtime_achievements[index];
            ui_measure_text(world_localized_text(&entry->title), 486, &width, &height);
            assert(height <= 28);
            ui_measure_text(world_localized_text(&entry->description), 486, &width, &height);
            assert(height <= 48);
        }
    }
}

/* Run independently from platform/GPU code with the generated game rules. */
int main(void)
{
    test_rules();
    test_save();
    test_hooks();
    test_notifications();
    test_layout();
    puts("Achievement checks passed");
    return 0;
}
