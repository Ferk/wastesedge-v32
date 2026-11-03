/* Layout for a compact image-and-text notice over the game world. */
#ifndef WASTESEDGE_UI_NOTICE_H
#define WASTESEDGE_UI_NOTICE_H

struct ui_notice_layout {
    int x, y, width, height;
    int image_x, image_y;
    int text_x, text_y, text_width;
};

/* Center a notice around the original scene's vertical anchor, growing for text. */
void ui_notice_layout(struct ui_notice_layout *layout, int viewport_width, int viewport_height,
                      int image_width, int image_height, int text_height);

#endif
