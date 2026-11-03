/* Host regression checks for localized name expansion and save persistence.
 * Build with src/ui/virtual_keyboard.c, src/engine/{world,savegame}.c,
 * and generated/content*.c. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "engine/savegame.h"
#include "content_ids.h"
#include "ui/virtual_keyboard.h"

/* Check page edges, incomplete rows, default replacement and accented editing. */
static void check_keyboard(void)
{
    struct virtual_keyboard keyboard;
    struct virtual_keyboard_input input = { 0 };
    char text[21] = "Banec";
    int page;
    int key;
    int direction;
    virtual_keyboard_begin(&keyboard);
    input.accept = 1;
    assert(virtual_keyboard_update(&keyboard, text, 21, &input) & VIRTUAL_KEYBOARD_ACCEPTED);
    assert(strcmp(text, "Banec") == 0);
    input.accept = 0;
    input.page_right = 1;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(keyboard.page == 1 && keyboard.text_selected);
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(keyboard.page == 1); /* Holding a shoulder does not cycle every frame. */
    input.page_right = 0;
    input.page_left = 1;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(keyboard.page == 0);
    input.page_left = 0;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    input.page_left = 1;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(keyboard.page == 1); /* Both directions wrap around the page list. */
    input.page_left = 0;
    input.type = 1;
    for (key = 0; key < VIRTUAL_KEYBOARD_CHARACTER_KEYS; ++key)
        if (virtual_keyboard_character(1, key) == 0xC9) break;
    assert(key < VIRTUAL_KEYBOARD_CHARACTER_KEYS);
    keyboard.key = key;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert((unsigned char)text[0] == 0xC9 && text[1] == 0);
    assert(!keyboard.text_selected);
    keyboard.key = VIRTUAL_KEYBOARD_SPACE;
    input.type = 0;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    input.type = 1;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(text[1] == ' ' && text[2] == 0);
    input.type = 0;
    input.erase = 1;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(text[1] == 0);
    input.erase = 0;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    strcpy(text, "12345678901234567890");
    keyboard.key = 0;
    input.type = 1;
    assert(!(virtual_keyboard_update(&keyboard, text, 21, &input) & VIRTUAL_KEYBOARD_EDITED));
    assert(strlen(text) == 20);
    input.type = 0;
    input.erase = 1;
    input.frame = 100;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(strlen(text) == 19);
    input.frame = 123;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(strlen(text) == 19);
    input.frame = 124;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(strlen(text) == 18);
    for (input.frame = 125; input.frame < 220; ++input.frame)
        virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(text[0] == 0);
    assert(!(virtual_keyboard_update(&keyboard, text, 21, &input) & VIRTUAL_KEYBOARD_EDITED));
    input.erase = 0;
    input.space = 1;
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(strcmp(text, " ") == 0);
    virtual_keyboard_update(&keyboard, text, 21, &input);
    assert(strcmp(text, " ") == 0); /* X cannot also type the focused letter. */
    input.space = 0;
    /* Every directional transition stays on an actual key, even in the
     * shorter last basic row and the three-key shared action row. */
    for (page = 0; page < VIRTUAL_KEYBOARD_PAGE_COUNT; ++page) {
        for (key = 0; key <= VIRTUAL_KEYBOARD_ACCEPT; ++key) {
            if (key < VIRTUAL_KEYBOARD_CHARACTER_KEYS && !virtual_keyboard_character(page, key))
                continue;
            assert(virtual_keyboard_character(page, key) != '_');
            /* The font's florin cell is a question-mark fallback, not a
             * usable name character. Use a curly apostrophe instead. */
            assert(virtual_keyboard_character(page, key) != 0x83);
            if (key < VIRTUAL_KEYBOARD_CHARACTER_KEYS)
                assert(generated_runtime_glyphs[virtual_keyboard_character(page, key)].advance > 0);
            for (direction = 0; direction < 4; ++direction) {
                virtual_keyboard_begin(&keyboard);
                keyboard.page = page;
                keyboard.key = key;
                input.x = direction == 0 ? -1 : direction == 1 ? 1 : 0;
                input.y = direction == 2 ? -1 : direction == 3 ? 1 : 0;
                virtual_keyboard_update(&keyboard, text, 21, &input);
                assert(keyboard.key >= 0 && keyboard.key <= VIRTUAL_KEYBOARD_ACCEPT);
                assert(keyboard.key >= VIRTUAL_KEYBOARD_CHARACTER_KEYS ||
                       virtual_keyboard_character(page, keyboard.key) != 0);
            }
        }
    }
    assert(virtual_keyboard_character(1, VIRTUAL_KEYBOARD_CHARACTER_KEYS - 1) == 0x92);
}

/* Decode compiled CP-1252 glyphs for assertions about displayed text. */
static void display_string(const struct generated_runtime_text *text, char *output)
{
    int index;
    assert(text->glyph_count < 512);
    for (index = 0; index < text->glyph_count; ++index)
        output[index] = text->glyphs[index] == -1 ? ' ' : (char)text->glyphs[index];
    output[index] = 0;
}

/* Verify actual intro translations, repeated tokens, bounds and save slots. */
int main(void)
{
    struct world_state world;
    struct world_state preview;
    int words[SAVEGAME_MAX_WORDS];
    int count;
    int node;
    int language;
    char displayed[512];
    static const int tokens[] = { -3, -1, -3 };
    const struct generated_runtime_text source = { tokens, 3, -1 };
    const struct generated_runtime_dialogue *intro =
        &generated_runtime_dialogues[GENERATED_DIALOGUE_INTRO_CUTSCENE];
    check_keyboard();
    world_initialize(&world);
    assert(strcmp(world.player_name, "Banec") == 0);
    world_set_player_name(&world, "Alba");
    display_string(world_localized_text(&source), displayed);
    assert(strcmp(displayed, "Alba Alba") == 0);
    for (node = 0; node < intro->node_count; ++node)
        if (strstr(intro->nodes[node].text, "I am $name") != 0)
            break;
    assert(node < intro->node_count);
    for (language = 0; language < generated_runtime_language_count; ++language) {
        world_set_language(language);
        display_string(world_localized_text(&intro->nodes[node].display), displayed);
        assert(strstr(displayed, "Alba") != 0);
        assert(strstr(displayed, "Banec") == 0);
        if (strcmp(generated_runtime_languages[language].id, "es") == 0)
            assert(strncmp(displayed, "Soy Alba,", 9) == 0);
    }
    count = savegame_encode(&world, words, SAVEGAME_MAX_WORDS);
    assert(count > 0);
    world_set_player_name(&world, "Other");
    assert(savegame_decode(&preview, words, count));
    assert(strcmp(preview.player_name, "Alba") == 0);
    display_string(world_localized_text(&source), displayed);
    assert(strcmp(displayed, "Other Other") == 0);
    world_set_player_name(&preview, preview.player_name);
    display_string(world_localized_text(&source), displayed);
    assert(strcmp(displayed, "Alba Alba") == 0);
    world_set_player_name(&world, "1234567890123456789012345");
    assert(strlen(world.player_name) == 20);
    world_set_player_name(&world, "");
    assert(strcmp(world.player_name, "Banec") == 0);
    /* Format 5 has no name; keep the authored default when loading it. */
    words[0] = 5;
    assert(savegame_decode(&preview, words, count - 21));
    assert(strcmp(preview.player_name, "Banec") == 0);
    puts("player name checks passed");
    return 0;
}
