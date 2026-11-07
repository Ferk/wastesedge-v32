/* Host checks for compact notice layout, translated text and chest flow. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "content_ids.h"
#include "engine/world.h"
#include "ui/notice.h"
#include "ui/text_layout.h"

/* Whole-word wrapping requires every individual word to fit its column. */
static void check_words_fit(const struct generated_runtime_text *text, int available)
{
    int word_width = 0;
    int index;
    for (index = 0; index < text->glyph_count; ++index) {
        int glyph = text->glyphs[index];
        if (glyph < 0)
            word_width = 0;
        else {
            word_width += generated_runtime_glyphs[glyph].advance;
            assert(word_width <= available);
        }
    }
}

/* Verify both gem pages fit in every shipped language and retain state timing. */
int main(void)
{
    struct world_state world;
    struct world_actions actions = { 0 };
    struct ui_notice_layout layout;
    const struct generated_runtime_dialogue *dialogue =
        &generated_runtime_dialogues[GENERATED_DIALOGUE_SEARCH_SILVERHAIR_CHEST];
    int gem_node = dialogue->starts[0].node;
    int flag = dialogue->starts[0].flag;
    int next = dialogue->nodes[gem_node].next_node;
    int language;
    int node;
    world_initialize(&world);
    world.dialogue = GENERATED_DIALOGUE_SEARCH_SILVERHAIR_CHEST;
    world.dialogue_node = gem_node;
    world.flags[flag] = 1;
    assert(world_dialogue_presentation(&world) == GENERATED_DIALOGUE_PRESENTATION_NOTICE);
    assert(dialogue->speakers[dialogue->nodes[gem_node].speaker].portrait.texture >= 0);
    ui_notice_layout(&layout, 640, 360, 64, 64, 19);
    assert(layout.width == 330 && layout.height == 105 && layout.x == 155);
    assert(layout.y + layout.height / 2 == 165);
    for (language = 0; language < generated_runtime_language_count; ++language) {
        world_set_language(language);
        for (node = gem_node; ; node = next) {
            int width;
            int height;
            assert(dialogue->nodes[node].presentation_mode == GENERATED_DIALOGUE_PRESENTATION_NOTICE);
            ui_notice_layout(&layout, 640, 360, 64, 64, 0);
            ui_measure_text(world_localized_text(&dialogue->nodes[node].display), layout.text_width, &width, &height);
            ui_notice_layout(&layout, 640, 360, 64, 64, height);
            /* Measurement may include trailing whitespace beyond the edge,
             * but all drawn glyphs fit when each wrapped word fits. */
            check_words_fit(world_localized_text(&dialogue->nodes[node].display), layout.text_width);
            assert(layout.text_y >= layout.y + 8);
            assert(layout.text_y + height <= layout.y + layout.height - 8);
            if (node == next) break;
        }
    }
    actions.cancel_pressed = 1;
    world_update(&world, &actions);
    assert(world.dialogue_node == next && world.flags[flag] == 1);
    actions.cancel_pressed = 0;
    actions.interact_pressed = 1;
    world_update(&world, &actions);
    assert(world.dialogue < 0 && world.flags[flag] == 1);
    actions.interact_pressed = 0;
    world_update(&world, &actions);
    assert(world.flags[flag] == 2);

    /* The familiar-chest remark is timed actor speech, not a blocking modal. */
    world_initialize(&world);
    world.dialogue = -1;
    world.active_script = GENERATED_SCRIPT_SEARCH_SILVERHAIR_CHEST;
    world.script_action = 0;
    world_update(&world, &actions);
    assert(world.dialogue < 0);
    assert(world.actors[GENERATED_ENTITY_PLAYER].speech_frames == 180);
    assert(world.actors[GENERATED_ENTITY_PLAYER].speech_override != 0);
    world_update(&world, &actions);
    assert(world.active_script < 0 && world.control_enabled);
    assert(world.actors[GENERATED_ENTITY_PLAYER].speech_frames > 0);
    puts("notice checks passed");
    return 0;
}
