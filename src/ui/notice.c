/* Screen-independent placement of compact image-and-text notices. */
#include "ui/notice.h"

#define NOTICE_WIDTH 330
#define NOTICE_MIN_HEIGHT 105
#define NOTICE_PADDING 12
#define NOTICE_IMAGE_GAP 12

/* Fit the image and wrapped text in a centered, variable-height window. */
void ui_notice_layout(struct ui_notice_layout *layout, int viewport_width, int viewport_height,
                      int image_width, int image_height, int text_height)
{
    int content_height = image_height > text_height ? image_height : text_height;
    layout->width = NOTICE_WIDTH < viewport_width - 2 * NOTICE_PADDING ?
                    NOTICE_WIDTH : viewport_width - 2 * NOTICE_PADDING;
    layout->height = content_height + 2 * NOTICE_PADDING;
    if (layout->height < NOTICE_MIN_HEIGHT)
        layout->height = NOTICE_MIN_HEIGHT;
    if (layout->height > viewport_height - 2 * NOTICE_PADDING)
        layout->height = viewport_height - 2 * NOTICE_PADDING;
    layout->x = (viewport_width - layout->width) / 2;
    layout->y = viewport_height * 11 / 24 - layout->height / 2;
    if (layout->y < NOTICE_PADDING)
        layout->y = NOTICE_PADDING;
    if (layout->y + layout->height > viewport_height - NOTICE_PADDING)
        layout->y = viewport_height - NOTICE_PADDING - layout->height;
    layout->image_x = layout->x + NOTICE_PADDING;
    layout->image_y = layout->y + (layout->height - image_height) / 2;
    layout->text_x = layout->image_x + image_width + (image_width > 0 ? NOTICE_IMAGE_GAP : 0);
    layout->text_width = layout->x + layout->width - NOTICE_PADDING - layout->text_x;
    layout->text_y = layout->y + (layout->height - text_height) / 2;
}
