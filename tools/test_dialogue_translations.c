/* Host regression checks for port dialogue keys, player names and Spanish credits. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "content_ids.h"
#include "engine/world.h"

/* Copy a localized CP-1252 glyph string, preserving authored line breaks. */
static void display_string(const struct generated_runtime_text *source, char *output, int capacity)
{
    const struct generated_runtime_text *text = world_localized_text(source);
    int index;
    assert(text->glyph_count < capacity);
    for (index = 0; index < text->glyph_count; ++index) {
        int glyph = text->glyphs[index];
        output[index] = glyph == -1 ? ' ' : glyph == -2 ? '\n' : (char)glyph;
    }
    output[index] = 0;
}

/* Locate a compiled NPC/player line or choice by its English authoring text. */
static const struct generated_runtime_text *find_text(int dialogue_id, const char *english)
{
    const struct generated_runtime_dialogue *dialogue = &generated_runtime_dialogues[dialogue_id];
    int node_index;
    int choice_index;
    for (node_index = 0; node_index < dialogue->node_count; ++node_index) {
        const struct generated_runtime_dialogue_node *node = &dialogue->nodes[node_index];
        if (strcmp(node->text, english) == 0)
            return &node->display;
        for (choice_index = 0; choice_index < node->choice_count; ++choice_index)
            if (strcmp(node->choices[choice_index].text, english) == 0)
                return &node->choices[choice_index].display;
    }
    return 0;
}

/* Verify real catalog lookups, reused choices, dynamic names and edited credits. */
int main(void)
{
    struct world_state world;
    const struct generated_runtime_text *text;
    const struct generated_runtime_dialogue *credits = &generated_runtime_dialogues[GENERATED_DIALOGUE_EXTRO_CREDITS];
    char displayed[4096];
    int language;
    world_initialize(&world);
    world_set_player_name(&world, "Alba");
    text = find_text(GENERATED_DIALOGUE_EREK_START, "Well, in that case I will leave him alone.");
    assert(text != 0);
    for (language = 1; language < generated_runtime_language_count; ++language) {
        world_set_language(language);
        display_string(text, displayed, sizeof(displayed));
        assert(strcmp(displayed, "Well, in that case I will leave him alone.") != 0);
    }
    world_set_language(2); /* Spanish in the shipped manifest. */
    text = find_text(GENERATED_DIALOGUE_EREK_START, "Obviously not. Now would you like to see it?");
    assert(text != 0);
    display_string(text, displayed, sizeof(displayed));
    assert(strstr(displayed, "Obviamente no") != 0);
    text = find_text(GENERATED_DIALOGUE_EREK_START,
                     "You don't trust me, Banec? In that case I'm sorry. "
                     "Unless I know the truth about the gem, I cannot help you.");
    assert(text != 0);
    display_string(text, displayed, sizeof(displayed));
    assert(strstr(displayed, "Alba") != 0 && strstr(displayed, "Banec") == 0);
    assert(strstr(displayed, "verdad") != 0);
    display_string(&credits->nodes[credits->starts[0].node].display, displayed, sizeof(displayed));
    assert(strstr(displayed, "Fernando Carmona Varo") != 0);
    assert(strstr(displayed, "Vircon32") != 0);
    assert(strstr(displayed, "software libre") != 0);
    assert(strstr(displayed, "Alba - ") != 0);
    puts("dialogue translation checks passed");
    return 0;
}
