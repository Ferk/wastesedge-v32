/* Measurement of compiled, byte-indexed game text. */
#ifndef WASTESEDGE_UI_TEXT_LAYOUT_H
#define WASTESEDGE_UI_TEXT_LAYOUT_H
#include "content_scene_data.h"

/* Measure wrapped text using the same word boundaries as the renderer. */
void ui_measure_text(const struct generated_runtime_text *text, int maximum_width,
                     int *width, int *height);
#endif
