/* Shared layout measurement for dialogue, notices and speech balloons. */
#include "ui/text_layout.h"

/* Measure a glyph sequence, wrapping whole words at the requested width. */
void ui_measure_text(const struct generated_runtime_text *text, int maximum_width,
                     int *width, int *height)
{
    int cursor_x = 0;
    int used_width = 0;
    int lines = 1;
    int index;
    if (text == 0 || text->glyph_count <= 0) {
        *width = 0;
        *height = generated_runtime_line_height;
        return;
    }
    for (index = 0; index < text->glyph_count; ++index) {
        int glyph_index = text->glyphs[index];
        if (glyph_index == -2) {
            if (cursor_x > used_width) used_width = cursor_x;
            cursor_x = 0;
            ++lines;
            continue;
        }
        if (glyph_index < 0) {
            cursor_x += generated_runtime_space_advance;
            continue;
        }
        if (index == 0 || text->glyphs[index - 1] < 0) {
            int scan = index;
            int word_width = 0;
            while (scan < text->glyph_count && text->glyphs[scan] >= 0) {
                word_width += generated_runtime_glyphs[text->glyphs[scan]].advance;
                ++scan;
            }
            if (cursor_x > 0 && cursor_x + word_width > maximum_width) {
                if (cursor_x > used_width) used_width = cursor_x;
                cursor_x = 0;
                ++lines;
            }
        }
        cursor_x += generated_runtime_glyphs[glyph_index].advance;
        if (cursor_x > used_width) used_width = cursor_x;
    }
    *width = used_width;
    *height = lines * generated_runtime_line_height;
}
