/* Shared virtual keyboard editing and navigation, with basic/international pages.
 * Text buffers use CP-1252 bytes, matching the compiled game's glyph atlas. */
#include "ui/virtual_keyboard.h"

static const unsigned char basic_characters[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789'.-";
static const unsigned char accented_characters[] =
    /* Latin-1 letters (excluding multiplication/division), then CP-1252 extras. */
    "\xC0\xC1\xC2\xC3\xC4\xC5\xC6\xC7\xC8\xC9\xCA\xCB\xCC\xCD\xCE\xCF"
    "\xD0\xD1\xD2\xD3\xD4\xD5\xD6\xD8\xD9\xDA\xDB\xDC\xDD\xDE\xDF"
    "\xE0\xE1\xE2\xE3\xE4\xE5\xE6\xE7\xE8\xE9\xEA\xEB\xEC\xED\xEE\xEF"
    "\xF0\xF1\xF2\xF3\xF4\xF5\xF6\xF8\xF9\xFA\xFB\xFC\xFD\xFE\xFF"
    "\x8A\x8C\x8E\x9A\x9C\x9E\x9F\x92";

/* Return the number of actual character keys, excluding the shared action row. */
static int character_count(int page)
{
    return page == 0 ? (int)sizeof(basic_characters) - 1 : (int)sizeof(accented_characters) - 1;
}

/* Return a key's CP-1252 byte; blank grid positions never accept input. */
int virtual_keyboard_character(int page, int key)
{
    if (page < 0 || page >= VIRTUAL_KEYBOARD_PAGE_COUNT || key < 0 || key >= character_count(page))
        return 0;
    return page == 0 ? basic_characters[key] : accented_characters[key];
}

/* Reset focus and input edges without altering the caller's text buffer. */
void virtual_keyboard_begin(struct virtual_keyboard *keyboard)
{
    keyboard->key = 0;
    keyboard->page = 0;
    keyboard->text_selected = 1;
    keyboard->previous_x = 0;
    keyboard->previous_y = 0;
    keyboard->previous_left = 0;
    keyboard->previous_right = 0;
    keyboard->previous_type = 0;
    keyboard->previous_space = 0;
    keyboard->previous_erase = 0;
    keyboard->erase_next_frame = 0;
}

/* Navigate pages and ragged rows, then apply bounded text edits or accept. */
int virtual_keyboard_update(struct virtual_keyboard *keyboard, char *text, int capacity,
                            const struct virtual_keyboard_input *input)
{
    int old_key = keyboard->key;
    int old_page = keyboard->page;
    int row = keyboard->key / VIRTUAL_KEYBOARD_COLUMNS;
    int column = keyboard->key % VIRTUAL_KEYBOARD_COLUMNS;
    int row_size;
    int length = 0;
    int result = VIRTUAL_KEYBOARD_UNCHANGED;
    int byte;
    int type_pressed = input->type && !keyboard->previous_type;
    int space_pressed = input->space && !keyboard->previous_space;
    int erase_pressed = 0;
    keyboard->previous_type = input->type;
    keyboard->previous_space = input->space;
    if (input->erase) {
        if (!keyboard->previous_erase) {
            erase_pressed = 1;
            keyboard->erase_next_frame = input->frame + VIRTUAL_KEYBOARD_ERASE_DELAY;
        } else if (input->frame >= keyboard->erase_next_frame) {
            erase_pressed = 1;
            keyboard->erase_next_frame = input->frame + VIRTUAL_KEYBOARD_ERASE_INTERVAL;
        }
    }
    keyboard->previous_erase = input->erase;
    if (space_pressed)
        type_pressed = 0;
    if (capacity < 1)
        return result;
    if (input->page_left && !keyboard->previous_left)
        keyboard->page = (keyboard->page + VIRTUAL_KEYBOARD_PAGE_COUNT - 1) % VIRTUAL_KEYBOARD_PAGE_COUNT;
    if (input->page_right && !keyboard->previous_right)
        keyboard->page = (keyboard->page + 1) % VIRTUAL_KEYBOARD_PAGE_COUNT;
    keyboard->previous_left = input->page_left;
    keyboard->previous_right = input->page_right;
    if (input->y != 0 && input->y != keyboard->previous_y) {
        row += input->y;
        if (row < 0) row = VIRTUAL_KEYBOARD_ROWS;
        if (row > VIRTUAL_KEYBOARD_ROWS) row = 0;
    }
    row_size = row == VIRTUAL_KEYBOARD_ROWS ? VIRTUAL_KEYBOARD_ACTION_COUNT :
               character_count(keyboard->page) - row * VIRTUAL_KEYBOARD_COLUMNS;
    if (row_size > VIRTUAL_KEYBOARD_COLUMNS) row_size = VIRTUAL_KEYBOARD_COLUMNS;
    if (column >= row_size) column = row_size - 1;
    if (input->x != 0 && input->x != keyboard->previous_x) {
        column += input->x;
        if (column < 0) column = row_size - 1;
        if (column >= row_size) column = 0;
    }
    keyboard->previous_x = input->x;
    keyboard->previous_y = input->y;
    keyboard->key = row * VIRTUAL_KEYBOARD_COLUMNS + column;
    if (keyboard->key != old_key || keyboard->page != old_page)
        result |= VIRTUAL_KEYBOARD_MOVED;
    if (input->accept || (type_pressed && keyboard->key == VIRTUAL_KEYBOARD_ACCEPT))
        return result | VIRTUAL_KEYBOARD_ACCEPTED;
    while (length < capacity - 1 && text[length] != 0)
        ++length;
    if (erase_pressed || (type_pressed && keyboard->key == VIRTUAL_KEYBOARD_DELETE)) {
        if (length == 0)
            return result;
        if (keyboard->text_selected) length = 0;
        else if (length > 0) --length;
        text[length] = 0;
        keyboard->text_selected = 0;
        return result | VIRTUAL_KEYBOARD_EDITED;
    }
    if (!type_pressed && !space_pressed)
        return result;
    byte = space_pressed || keyboard->key == VIRTUAL_KEYBOARD_SPACE ? ' ' :
           virtual_keyboard_character(keyboard->page, keyboard->key);
    if (byte == 0)
        return result;
    if (keyboard->text_selected) length = 0;
    if (length >= capacity - 1)
        return result;
    text[length++] = (char)byte;
    text[length] = 0;
    keyboard->text_selected = 0;
    return result | VIRTUAL_KEYBOARD_EDITED;
}
