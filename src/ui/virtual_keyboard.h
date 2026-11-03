/* Reusable controller-driven CP-1252 text entry, independent of game state. */
#ifndef WASTESEDGE_UI_VIRTUAL_KEYBOARD_H
#define WASTESEDGE_UI_VIRTUAL_KEYBOARD_H

#define VIRTUAL_KEYBOARD_COLUMNS 10
#define VIRTUAL_KEYBOARD_ROWS 7
#define VIRTUAL_KEYBOARD_CHARACTER_KEYS (VIRTUAL_KEYBOARD_COLUMNS * VIRTUAL_KEYBOARD_ROWS)
#define VIRTUAL_KEYBOARD_SPACE VIRTUAL_KEYBOARD_CHARACTER_KEYS
#define VIRTUAL_KEYBOARD_DELETE (VIRTUAL_KEYBOARD_SPACE + 1)
#define VIRTUAL_KEYBOARD_ACCEPT (VIRTUAL_KEYBOARD_SPACE + 2)
#define VIRTUAL_KEYBOARD_ACTION_COUNT 3
#define VIRTUAL_KEYBOARD_PAGE_COUNT 2
#define VIRTUAL_KEYBOARD_ERASE_DELAY 24
#define VIRTUAL_KEYBOARD_ERASE_INTERVAL 4

enum virtual_keyboard_result {
    VIRTUAL_KEYBOARD_UNCHANGED = 0,
    VIRTUAL_KEYBOARD_MOVED = 1,
    VIRTUAL_KEYBOARD_EDITED = 2,
    VIRTUAL_KEYBOARD_ACCEPTED = 4
};

struct virtual_keyboard {
    int key;
    int page;
    int text_selected;
    int previous_x, previous_y;
    int previous_left, previous_right;
    int previous_type, previous_space, previous_erase;
    int erase_next_frame;
};

struct virtual_keyboard_input {
    /* Directions and type/erase/space are held states; accept is a press edge.
     * frame is the monotonic 60 Hz display counter used for deletion repeat. */
    int x, y;
    int page_left, page_right;
    int type, erase, accept;
    int space, frame;
};

/* Select the current text so the first typed character replaces it. */
void virtual_keyboard_begin(struct virtual_keyboard *keyboard);
/* Return a key's CP-1252 byte, or zero for actions and unused positions. */
int virtual_keyboard_character(int page, int key);
/* Update focus/editing; capacity includes the terminating zero byte. */
int virtual_keyboard_update(struct virtual_keyboard *keyboard, char *text, int capacity,
                            const struct virtual_keyboard_input *input);

#endif
