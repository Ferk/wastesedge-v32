#include "platform/vircon32/vircon.h"

#include "platform/vircon32/platform.h"
#include "engine/savegame.h"

#define GAME_WIDTH 427
#define GAME_HEIGHT 240
#define GAME_SCALE 1.5f
#define DISPLAY_WIDTH 640
#define DISPLAY_HEIGHT 360
#define VIEW_WIDTH DISPLAY_WIDTH
#define VIEW_X ((DISPLAY_WIDTH - VIEW_WIDTH) / 2)
#define DIALOG_X (VIEW_X + 12)
#define DIALOG_WIDTH (VIEW_WIDTH - 24)
#define DIALOG_MAX_HEIGHT 260
#define DIALOG_BOTTOM (DISPLAY_HEIGHT - 12)
#define DIALOG_LINE_HEIGHT 19
#define DIALOG_CHOICE_GAP 5
#define SPEECH_MAX_TEXT_WIDTH 300
#define SPEECH_PADDING_X 6
#define SPEECH_PADDING_Y 3
#define SPEECH_SPRITE_GAP 4
#define MAX_DRAW_COMMANDS 4096
#define FIRST_GLYPH_REGION 512
#define LAST_GLYPH_REGION 4095
#define DEFAULT_CPU_CYCLES_PER_FRAME 250000
#define DRAW_FIXED_CYCLES 12000
#define DRAW_CYCLES_PER_COMMAND 160
#define DRAW_SAFETY_CYCLES 20000
#define MUSIC_CHANNEL 0
#define SAVE_SLOT_STRIDE 1024
#define SAVE_SLOT_HEADER_WORDS 8
#define SAVE_PREVIEW_TILES 6
#define SAVE_PREVIEW_SIZE 60
#define WALK_ANIMATION_SLOWDOWN 2
#define CUTSCENE_RANGE_CATCH_UP_STEP 8

/* Vircon32 colors use AABBGGRR ordering. Keep every authored renderer color
 * named here so alpha values and palette choices remain easy to audit. */
#define COLOR_WHITE 0xFFFFFFFFu
#define COLOR_RGB_MASK 0x00FFFFFFu
#define COLOR_DARK_BACKGROUND 0xFF101820u
#define COLOR_DIALOG_BORDER 0x90C0C0C0u
#define COLOR_DIALOG_BACKGROUND 0xA0080808u
#define COLOR_DIALOG_PORTRAIT_BACKGROUND 0x40080B10u
#define COLOR_ACCENT_GOLD 0xFFB0F8F8u
#define COLOR_TEXT_PALE 0xFFB0B0A0u
#define COLOR_SCROLL_TRACK 0x603A3828u
#define COLOR_SCROLL_THUMB 0xD0FFD070u
#define COLOR_BALLOON_BACKGROUND 0x90101010u
#define COLOR_ALPHA_BALLOON_SPEECH 0x90000000u
#define COLOR_TEXT_SHADOW 0xE0000000u
#define COLOR_SAVE_PREVIEW_BACKGROUND 0xFF080B10u
#define COLOR_PANEL_BORDER 0xFF2C6868u
#define COLOR_PANEL_BACKGROUND 0xFF101820u
#define COLOR_MENU_SELECTION 0x80404020u
#define COLOR_STATUS_SUCCESS 0xFF80FF80u
#define COLOR_STATUS_INFO 0xFF8080FFu
#define COLOR_SLOT_EMPTY_BORDER 0xFF304050u
#define COLOR_SLOT_SELECTED_BACKGROUND 0xFF302820u
#define COLOR_SLOT_BACKGROUND 0xFF182028u
#define COLOR_SLOT_SELECTED_TEXT 0xFFD0D070u
#define COLOR_SLOT_TEXT 0xFF607080u
#define COLOR_STATUS_ERROR 0xFF8080F0u
#define COLOR_SLOT_SELECTED_METADATA 0xFF8090A0u
#if SAVE_SLOT_HEADER_WORDS + SAVEGAME_MAX_WORDS > SAVE_SLOT_STRIDE
#error Save payload does not fit in a memory-card slot
#endif

/* Vircon32 has no writable framebuffer or display lists. Keep a small display
 * list in RAM so layout and depth work happen without touching the GPU, then
 * replay the finished frame in one compact draw phase. Parallel arrays avoid
 * aggregate stores that are needlessly awkward for the Wasm frontend. */
static int draw_command_count;
static int draw_command_texture[MAX_DRAW_COMMANDS];
static int draw_command_region[MAX_DRAW_COMMANDS];
static int draw_command_region_x[MAX_DRAW_COMMANDS];
static int draw_command_region_y[MAX_DRAW_COMMANDS];
static int draw_command_region_width[MAX_DRAW_COMMANDS];
static int draw_command_region_height[MAX_DRAW_COMMANDS];
static int draw_command_x[MAX_DRAW_COMMANDS];
static int draw_command_y[MAX_DRAW_COMMANDS];
static int draw_command_color[MAX_DRAW_COMMANDS];
static float draw_command_scale_x[MAX_DRAW_COMMANDS];
static float draw_command_scale_y[MAX_DRAW_COMMANDS];

static int prepared_dialogue = -1;
static int prepared_node;
static int prepared_choice;
static int prepared_frame_valid;
static int prepared_is_dialogue;
static const struct generated_runtime_scene *prepared_scene;
static int prepared_player_x;
static int prepared_player_y;
static int prepared_player_facing;
static int prepared_moving;
static int prepared_move_x;
static int prepared_move_y;
static int prepared_move_progress;
static int prepared_animation_frame;
static int prepared_visual_revision;
static int displayed_dialogue = -1;
static int displayed_node;
static int displayed_choice;
static int dialogue_scroll_dialogue = -1;
static int dialogue_scroll_node = -1;
static int dialogue_choice_scroll;
static const struct generated_runtime_scene *displayed_scene;
static int displayed_player_x;
static int displayed_player_y;
static int displayed_player_facing;
static int displayed_moving;
static int displayed_move_x;
static int displayed_move_y;
static int displayed_move_progress;
static int displayed_animation_frame;
static int displayed_visual_revision;
static int displayed_world_valid;
static int cpu_cycles_per_frame = DEFAULT_CPU_CYCLES_PER_FRAME;
static int applied_music_revision = -1;
static int save_words[SAVEGAME_MAX_WORDS];
static struct world_state save_slot_previews[PLATFORM_SAVE_SLOT_COUNT];
static int save_slot_preview_valid[PLATFORM_SAVE_SLOT_COUNT];
static int displayed_slot_menu_valid;
static int displayed_slot_menu_from_title;
static int displayed_slot_menu_saving;
static int displayed_slot_menu_selected;
static int displayed_slot_menu_result;
static int displayed_slot_menu_states[PLATFORM_SAVE_SLOT_COUNT];
static int cinematic_dialogue = -1;
static int cinematic_node;
static int cinematic_node_frame;
static int cinematic_sequence_frame;
static int cinematic_visible_glyphs;
static int cinematic_text_tick;
static int cinematic_text_hold;
static int cinematic_pending_node = -1;
static int cinematic_node_started_at[GENERATED_RUNTIME_DIALOGUE_USED_STORAGE_COUNT];
static int active_music_track = -1;
static game_signature game_card_signature;

static void define_region_rectangle(int x, int y, int width, int height)
{
    set_region_minimum(x, y);
    set_region_maximum(x + width - 1, y + height - 1);
    set_region_hotspot(x, y);
}

static void record_draw(int texture, int region, int region_x, int region_y,
                        int region_width, int region_height,
                        int x, int y, float scale_x, float scale_y, int color)
{
    int index;
    if (draw_command_count >= MAX_DRAW_COMMANDS)
        return;
    index = draw_command_count++;
    draw_command_texture[index] = texture;
    draw_command_region[index] = region;
    draw_command_region_x[index] = region_x;
    draw_command_region_y[index] = region_y;
    draw_command_region_width[index] = region_width;
    draw_command_region_height[index] = region_height;
    draw_command_x[index] = x;
    draw_command_y[index] = y;
    draw_command_scale_x[index] = scale_x;
    draw_command_scale_y[index] = scale_y;
    draw_command_color[index] = color;
}

static int view_x(int x)
{
    return VIEW_X + (x * 3) / 2;
}

static int view_y(int y)
{
    return (y * 3) / 2;
}

static void record_texture_color(int texture, int texture_x, int texture_y,
                                 int width, int height, int x, int y, int color)
{
    int source_x = texture_x;
    int source_y = texture_y;
    int visible_width = width;
    int visible_height = height;

    if (texture < 0 || x >= GAME_WIDTH || y >= GAME_HEIGHT ||
        x + width <= 0 || y + height <= 0)
        return;

    /* Vircon32 charges a draw by its complete scaled source rectangle, even
     * when most of it lies outside the screen. Change the texture region so
     * large scenery at a viewport edge only consumes pixels that can appear. */
    if (x < 0) {
        int clipped = -x;
        source_x += clipped;
        visible_width -= clipped;
        x = 0;
    }
    if (y < 0) {
        int clipped = -y;
        source_y += clipped;
        visible_height -= clipped;
        y = 0;
    }
    if (x + visible_width > GAME_WIDTH)
        visible_width = GAME_WIDTH - x;
    if (y + visible_height > GAME_HEIGHT)
        visible_height = GAME_HEIGHT - y;

    record_draw(texture, 0, source_x, source_y, visible_width, visible_height,
                view_x(x), view_y(y),
                GAME_SCALE, GAME_SCALE, color);
}

static void record_texture(int texture, int texture_x, int texture_y,
                           int width, int height, int x, int y)
{
    record_texture_color(texture, texture_x, texture_y, width, height,
                         x, y, COLOR_WHITE);
}

static void record_frame(const struct generated_runtime_frame *frame, int x, int y)
{
    record_texture(frame->texture, frame->source_x, frame->source_y,
                   frame->width, frame->height, x, y);
}

static void record_scenery_frame(const struct generated_runtime_frame *frame,
                                 int x, int y, int alpha)
{
    int color = (int)(((unsigned int)alpha << 24) | COLOR_RGB_MASK);
    record_texture_color(frame->texture, frame->source_x, frame->source_y,
                         frame->width, frame->height, x, y, color);
}

static void record_solid_rect(int x, int y, int width, int height, int color)
{
    record_draw(-1, 256, 0, 0, 0, 0,
                x, y, (float)width, (float)height, color);
}

static void record_ui_frame(const struct generated_runtime_frame *frame,
                            int x, int y, float scale)
{
    if (frame == 0 || frame->texture < 0)
        return;
    record_draw(frame->texture, 0, frame->source_x, frame->source_y,
                frame->width, frame->height,
                x, y, scale, scale, COLOR_WHITE);
}

static int record_runtime_text_clipped(int x, int y, int width, int top, int bottom,
                                       const struct generated_runtime_text *text,
                                       int color)
{
    int cursor_x = x;
    int cursor_y = y;
    int index;
    if (text == 0)
        return y;
    for (index = 0; index < text->glyph_count; ++index) {
        int glyph_index = text->glyphs[index];
        if (glyph_index == -2) {
            cursor_x = x;
            cursor_y += DIALOG_LINE_HEIGHT;
            continue;
        }
        const struct generated_runtime_glyph *glyph;
        if (glyph_index < 0) {
            cursor_x += generated_runtime_space_advance;
            continue;
        }
        glyph = &generated_runtime_glyphs[glyph_index];
        if (index == 0 || text->glyphs[index - 1] < 0) {
            int scan = index;
            int word_width = 0;
            while (scan < text->glyph_count && text->glyphs[scan] >= 0) {
                word_width += generated_runtime_glyphs[text->glyphs[scan]].advance;
                ++scan;
            }
            if (cursor_x != x && cursor_x + word_width > x + width) {
                cursor_x = x;
                cursor_y += DIALOG_LINE_HEIGHT;
            }
        }
        if (cursor_y >= bottom)
            break;
        if (cursor_y >= top && cursor_y + DIALOG_LINE_HEIGHT <= bottom)
            record_draw(glyph->texture, LAST_GLYPH_REGION - glyph_index, 0, 0, 0, 0,
                        cursor_x, cursor_y, 1.0f, 1.0f, color);
        cursor_x += glyph->advance;
    }
    return cursor_y + DIALOG_LINE_HEIGHT;
}

static int record_runtime_text(int x, int y, int width, int bottom,
                               const struct generated_runtime_text *text, int color)
{
    return record_runtime_text_clipped(x, y, width, y, bottom, text, color);
}

/* Draw explicitly authored lines centered independently, as used by the
 * original credits labels. Blank lines retain their vertical spacing. */
static void record_runtime_text_centered_lines(
    int center_x, int y, int bottom,
    const struct generated_runtime_text *text, int color)
{
    int start = 0;
    int index;
    if (text == 0)
        return;
    for (index = 0; index <= text->glyph_count && y < bottom; ++index) {
        if (index == text->glyph_count || text->glyphs[index] == -2) {
            /* Do not measure or submit complete lines that have already
             * scrolled above the display. The hardware would clip their
             * pixels, but processing the whole accumulated credits roll each
             * frame eventually exhausts the Vircon32 frame budget. */
            if (y + DIALOG_LINE_HEIGHT > 0) {
                struct generated_runtime_text line;
                int width = 0;
                int scan;
                line.glyphs = text->glyphs + start;
                line.glyph_count = index - start;
                for (scan = start; scan < index; ++scan) {
                    int glyph = text->glyphs[scan];
                    width += glyph < 0 ? generated_runtime_space_advance
                                       : generated_runtime_glyphs[glyph].advance;
                }
                record_runtime_text(center_x - width / 2, y, width + 1,
                                    y + DIALOG_LINE_HEIGHT, &line, color);
            }
            y += DIALOG_LINE_HEIGHT;
            start = index + 1;
        }
    }
}

static void measure_runtime_text(const struct generated_runtime_text *text,
                                 int maximum_width, int *width, int *height)
{
    int cursor_x = 0;
    int used_width = 0;
    int lines = 1;
    int index;
    if (text == 0 || text->glyph_count <= 0) {
        *width = 0;
        *height = DIALOG_LINE_HEIGHT;
        return;
    }
    for (index = 0; index < text->glyph_count; ++index) {
        int glyph_index = text->glyphs[index];
        if (glyph_index == -2) {
            if (cursor_x > used_width)
                used_width = cursor_x;
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
                if (cursor_x > used_width)
                    used_width = cursor_x;
                cursor_x = 0;
                ++lines;
            }
        }
        cursor_x += generated_runtime_glyphs[glyph_index].advance;
        if (cursor_x > used_width)
            used_width = cursor_x;
    }
    *width = used_width;
    *height = lines * DIALOG_LINE_HEIGHT;
}

static void record_dialogue_panel(const struct world_state *world)
{
    const struct generated_runtime_dialogue_speaker *speaker = world_dialogue_speaker(world);
    const struct generated_runtime_text *speech = world_dialogue_display(world);
    unsigned int speech_color = speaker != 0 ? speaker->speech_color : COLOR_WHITE;
    int choice_count = world_dialogue_choice_count(world);
    int selected_choice = world_dialogue_choice_selected(world);
    int has_portrait = speaker != 0 && speaker->portrait.texture >= 0;
    int text_x = has_portrait ? DIALOG_X + 128 : DIALOG_X + 24;
    int text_width = DIALOG_X + DIALOG_WIDTH - 20 - text_x;
    int choice_text_x = DIALOG_X + 36;
    int choice_text_width = DIALOG_WIDTH - 70;
    int speech_width;
    int speech_height;
    int selected_top = 0;
    int selected_bottom = 0;
    int choices_height = 0;
    int choice_content_height;
    int dialog_height;
    int dialog_y;
    int choice_view_top;
    int choice_view_bottom;
    int choice_view_height;
    int index;

    measure_runtime_text(speech, text_width, &speech_width, &speech_height);
    for (index = 0; index < choice_count; ++index) {
        int option_width;
        int option_height;
        measure_runtime_text(world_dialogue_choice_display(world, index),
                             choice_text_width, &option_width, &option_height);
        if (index == selected_choice) {
            selected_top = choices_height;
            selected_bottom = choices_height + option_height;
        }
        choices_height += option_height;
        if (index + 1 < choice_count)
            choices_height += DIALOG_CHOICE_GAP;
    }
    choice_content_height = choices_height;

    if (choice_count > 0) {
        /* Portrait conversations put answers below the portrait, matching the
         * original layout. Short answer lists now stop the panel early. */
        dialog_height = 138 + choice_content_height + 10;
    } else {
        int content_bottom = 44 + speech_height;
        if (has_portrait && content_bottom < 126)
            content_bottom = 126;
        dialog_height = content_bottom + 10;
    }
    if (dialog_height > DIALOG_MAX_HEIGHT)
        dialog_height = DIALOG_MAX_HEIGHT;
    dialog_y = DIALOG_BOTTOM - dialog_height;
    choice_view_bottom = dialog_y + dialog_height - 10;
    choice_view_top = choice_count > 0 ? dialog_y + 138 : choice_view_bottom;
    choice_view_height = choice_view_bottom - choice_view_top;

    /* The original conversation UI leaves the scene visible beneath a dark,
     * warm-edged translucent window. Anchor variable-height panels above the
     * same bottom margin so short exchanges expose more of the scene. */
    record_solid_rect(DIALOG_X - 2, dialog_y - 2,
                      DIALOG_WIDTH + 4, dialog_height + 4, COLOR_DIALOG_BORDER);
    record_solid_rect(DIALOG_X + 2, dialog_y + 2,
                      DIALOG_WIDTH - 4, dialog_height - 4, COLOR_DIALOG_BACKGROUND);

    if (has_portrait) {
        record_solid_rect(DIALOG_X + 18, dialog_y + 22, 96, 96,
                          COLOR_DIALOG_PORTRAIT_BACKGROUND);
        record_ui_frame(&speaker->portrait, DIALOG_X + 18, dialog_y + 22, 1.5f);
    }
    if (speaker != 0)
        record_runtime_text(text_x, dialog_y + 16, text_width, dialog_y + 42,
                            &speaker->name_text, COLOR_ACCENT_GOLD);
    record_runtime_text(text_x, dialog_y + 44, text_width,
                        choice_count > 0 ? choice_view_top - 6
                                         : dialog_y + dialog_height - 10,
                        speech, speech_color);

    /* Adonthell's conversation widget is a vertical scrollview. Reset it for
     * each new line, then move by whole answer rows just far enough to keep
     * the focused (possibly wrapped) answer visible. */
    if (displayed_dialogue < 0 ||
        dialogue_scroll_dialogue != world->dialogue ||
        dialogue_scroll_node != world->dialogue_node) {
        dialogue_scroll_dialogue = world->dialogue;
        dialogue_scroll_node = world->dialogue_node;
        dialogue_choice_scroll = 0;
    }
    if (selected_top < dialogue_choice_scroll) {
        dialogue_choice_scroll = selected_top;
    } else if (selected_bottom > dialogue_choice_scroll + choice_view_height) {
        int target_scroll = selected_bottom - choice_view_height;
        int option_top = 0;
        dialogue_choice_scroll = selected_top;
        for (index = 0; index < choice_count; ++index) {
            int option_width;
            int option_height;
            measure_runtime_text(world_dialogue_choice_display(world, index),
                                 choice_text_width, &option_width, &option_height);
            if (option_top >= target_scroll) {
                dialogue_choice_scroll = option_top;
                break;
            }
            option_top += option_height + DIALOG_CHOICE_GAP;
        }
    }
    if (choice_content_height > choice_view_height &&
        dialogue_choice_scroll > choice_content_height - choice_view_height)
        dialogue_choice_scroll = choice_content_height - choice_view_height;

    choices_height = 0;
    for (index = 0; index < choice_count; ++index) {
        int option_width;
        int option_height;
        int option_y;
        int visible_top;
        int visible_bottom;
        measure_runtime_text(world_dialogue_choice_display(world, index),
                             choice_text_width, &option_width, &option_height);
        option_y = choice_view_top + choices_height - dialogue_choice_scroll;
        visible_top = option_y > choice_view_top ? option_y : choice_view_top;
        visible_bottom = option_y + option_height < choice_view_bottom
                             ? option_y + option_height : choice_view_bottom;
        if (visible_bottom > visible_top) {
            record_runtime_text_clipped(choice_text_x, option_y, choice_text_width,
                                        choice_view_top, choice_view_bottom,
                                        world_dialogue_choice_display(world, index),
                                        index == selected_choice ? COLOR_ACCENT_GOLD
                                                                 : COLOR_TEXT_PALE);
            if (index == selected_choice && visible_bottom - visible_top > 6)
                record_solid_rect(DIALOG_X + 22, visible_top + 3, 5,
                                  visible_bottom - visible_top - 6, COLOR_ACCENT_GOLD);
        }
        choices_height += option_height + DIALOG_CHOICE_GAP;
    }

    if (choice_content_height > choice_view_height) {
        int track_x = DIALOG_X + DIALOG_WIDTH - 15;
        int thumb_height = (choice_view_height * choice_view_height) /
                           choice_content_height;
        int thumb_y;
        int maximum_scroll = choice_content_height - choice_view_height;
        if (thumb_height < 12)
            thumb_height = 12;
        thumb_y = choice_view_top;
        if (maximum_scroll > 0)
            thumb_y += (dialogue_choice_scroll * (choice_view_height - thumb_height)) /
                       maximum_scroll;
        record_solid_rect(track_x, choice_view_top, 4, choice_view_height,
                          COLOR_SCROLL_TRACK);
        record_solid_rect(track_x, thumb_y, 4, thumb_height, COLOR_SCROLL_THUMB);
    }
}

static void camera_for_player(const struct world_state *world, int *camera_x, int *camera_y)
{
    int player_definition = world->scene->entities[world->player_entity].definition;
    const struct generated_runtime_actor_visual *visual = &generated_runtime_actor_visuals[
        world_actor_visual(world, player_definition)];
    int player_x = world->player_x * world->scene->tile_size;
    int player_y = world->player_y * world->scene->tile_size;
    int x;
    int y;
    int max_x = world->scene->width * world->scene->tile_size - GAME_WIDTH;
    int max_y = world->scene->height * world->scene->tile_size - GAME_HEIGHT;
    if (world->moving) {
        player_x += world->move_x * world->move_progress;
        player_y += world->move_y * world->move_progress;
    }
    x = player_x - visual->base_x * world->scene->tile_size - GAME_WIDTH / 2 + world->scene->tile_size / 2;
    y = player_y - visual->base_y * world->scene->tile_size - GAME_HEIGHT / 2 + world->scene->tile_size;
    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > max_x) x = max_x;
    if (y > max_y) y = max_y;
    *camera_x = x;
    *camera_y = y;
}

static void draw_background(const struct world_state *world, int camera_x, int camera_y)
{
    const struct generated_runtime_scene *scene = world->scene;
    int index;
    for (index = 0; index < scene->background_count; ++index) {
        const struct generated_runtime_background *item = &scene->background[index];
        if (item->x >= camera_x + GAME_WIDTH || item->y >= camera_y + GAME_HEIGHT ||
            item->x + item->frame.width <= camera_x || item->y + item->frame.height <= camera_y)
            continue;
        record_frame(&item->frame, item->x - camera_x, item->y - camera_y);
    }
}

static const struct generated_runtime_frame *entity_frame(const struct world_state *world, int definition)
{
    const struct generated_runtime_actor_visual *visual = &generated_runtime_actor_visuals[
        world_actor_visual(world, definition)];
    int facing = world_actor_facing(world, definition);
    if (world_actor_is_moving(world, definition) && visual->walking_count[facing] > 0) {
        /* The movement counter remains distance-driven. Holding each pose for
         * two source steps slows only the gait, never character movement. */
        int animation_frame = world_actor_animation_frame(world, definition) /
                              WALK_ANIMATION_SLOWDOWN;
        return &visual->walking[facing][animation_frame % visual->walking_count[facing]];
    }
    if (visual->standing_count[facing] > 1 && visual->standing_delay[facing] > 0)
        return &visual->standing[facing][
            (world_actor_visual_tick(world, definition) / visual->standing_delay[facing]) %
            visual->standing_count[facing]];
    return &visual->standing[facing][0];
}

static int entity_depth(const struct world_state *world, int definition)
{
    /* Order characters by their logical base square, not by the bottom edge of the current animation frame.
       Frame bounds can extend below that square (the Waste's Edge actors do),
       and using those bounds shifts every character one row toward the camera. */
    return world_actor_pixel_y(world, definition);
}

static void draw_entity(const struct world_state *world, int definition, int camera_x, int camera_y)
{
    const struct generated_runtime_actor_visual *visual = &generated_runtime_actor_visuals[
        world_actor_visual(world, definition)];
    const struct generated_runtime_frame *frame = entity_frame(world, definition);
    int x = world_actor_pixel_x(world, definition);
    int y = world_actor_pixel_y(world, definition);
    record_frame(frame, x - visual->base_x * world->scene->tile_size + frame->offset_x - camera_x,
                 y - visual->base_y * world->scene->tile_size + frame->offset_y - camera_y);
}

static void draw_depth_sorted_objects(const struct world_state *world, int camera_x, int camera_y)
{
    const struct generated_runtime_scene *scene = world->scene;
    int scenery_index = 0;
    int drawn = 0;
    int last_y = -1;
    int last_index = -1;
    while (drawn < generated_runtime_actor_count) {
        int index;
        int selected = -1;
        int selected_y = 0;
        for (index = 0; index < generated_runtime_actor_count; ++index) {
            int y = entity_depth(world, index);
            if (world_actor_visual(world, index) < 0 ||
                !world_actor_is_present(world, index))
                continue;
            if (y < last_y || (y == last_y && index <= last_index))
                continue;
            if (selected < 0 || y < selected_y || (y == selected_y && index < selected)) {
                selected = index;
                selected_y = y;
            }
        }
        if (selected < 0)
            break;
        while (scenery_index < scene->scenery_count &&
               scene->scenery[scenery_index].depth <= selected_y) {
            const struct generated_runtime_scenery *item = &scene->scenery[scenery_index++];
            record_scenery_frame(world_scenery_frame(world, item),
                                 item->x - camera_x, item->y - camera_y,
                                 item->alpha);
        }
        draw_entity(world, selected, camera_x, camera_y);
        last_y = selected_y;
        last_index = selected;
        ++drawn;
    }
    while (scenery_index < scene->scenery_count) {
        const struct generated_runtime_scenery *item = &scene->scenery[scenery_index++];
        record_scenery_frame(world_scenery_frame(world, item),
                             item->x - camera_x, item->y - camera_y,
                             item->alpha);
    }
}

/* Draw a compact speech box with an optional tail aimed at its speaker. */
static void record_speech_box(int x, int y, int text_width, int text_height,
                              const struct generated_runtime_text *speech,
                              unsigned int speech_color,
                              int tail_x, int tail_y)
{
    int width = text_width + SPEECH_PADDING_X * 2;
    int height = text_height + SPEECH_PADDING_Y * 2;
    unsigned int border_color =
        (speech_color & COLOR_RGB_MASK) | COLOR_ALPHA_BALLOON_SPEECH;

    /* Separate one-pixel edges keep the translucent black interior from
     * inheriting any tint from the speaker-colored border. */
    record_solid_rect(x, y, width, height, COLOR_BALLOON_BACKGROUND);
    record_solid_rect(x - 1, y - 1, width + 2, 1, border_color);
    record_solid_rect(x - 1, y + height, width + 2, 1, border_color);
    record_solid_rect(x - 1, y, 1, height, border_color);
    record_solid_rect(x + width, y, 1, height, border_color);
    if (tail_x >= 0) {
        if (tail_y >= y + height)
            record_solid_rect(tail_x - 2, y + height,
                              4, tail_y - y - height, border_color);
        else if (tail_y < y)
            record_solid_rect(tail_x - 2, tail_y,
                              4, y - tail_y, border_color);
    }
    record_runtime_text(x + SPEECH_PADDING_X, y + SPEECH_PADDING_Y,
                        text_width, y + height - SPEECH_PADDING_Y,
                        speech, speech_color);
}

/* Draw one speech balloon anchored to an actor in the current camera view. */
static void record_entity_speech(const struct world_state *world,
                                 int entity_index,
                                 const struct generated_runtime_text *speech,
                                 int camera_x, int camera_y)
{
    const struct generated_runtime_actor_visual *visual;
    const struct generated_runtime_frame *frame;
    int anchor_x;
    int sprite_top;
    int sprite_bottom;
    int text_width;
    int text_height;
    int bubble_width;
    int bubble_height;
    int bubble_x;
    int bubble_y;
    int tail_x;
    unsigned int speech_color;
    if (speech == 0 || entity_index < 0 ||
        entity_index >= generated_runtime_actor_count ||
        world_actor_visual(world, entity_index) < 0 ||
        !world_actor_is_present(world, entity_index))
        return;
    visual = &generated_runtime_actor_visuals[world_actor_visual(world, entity_index)];
    frame = entity_frame(world, entity_index);
    anchor_x = view_x(world_actor_pixel_x(world, entity_index) -
                      visual->base_x * world->scene->tile_size + frame->offset_x -
                      camera_x + frame->width / 2);
    sprite_top = view_y(world_actor_pixel_y(world, entity_index) -
                        visual->base_y * world->scene->tile_size + frame->offset_y -
                        camera_y);
    sprite_bottom = sprite_top + (frame->height * 3) / 2;
    measure_runtime_text(speech, SPEECH_MAX_TEXT_WIDTH, &text_width, &text_height);
    bubble_width = text_width + SPEECH_PADDING_X * 2;
    bubble_height = text_height + SPEECH_PADDING_Y * 2;
    bubble_x = anchor_x - bubble_width / 2;
    if (bubble_x < 6) bubble_x = 6;
    if (bubble_x + bubble_width > DISPLAY_WIDTH - 6)
        bubble_x = DISPLAY_WIDTH - 6 - bubble_width;
    bubble_y = sprite_top - bubble_height - SPEECH_SPRITE_GAP;
    if (bubble_y < 6)
        bubble_y = sprite_bottom + SPEECH_SPRITE_GAP;
    tail_x = anchor_x;
    if (tail_x < bubble_x + 4) tail_x = bubble_x + 4;
    if (tail_x > bubble_x + bubble_width - 4)
        tail_x = bubble_x + bubble_width - 4;
    speech_color = generated_runtime_actors[entity_index].speech_color;
    record_speech_box(bubble_x, bubble_y, text_width, text_height,
                      speech, speech_color, tail_x,
                      bubble_y < sprite_top ? sprite_top : sprite_bottom);
}

/* Draw every currently active ambient actor line over the map view. */
static void record_ambient_speech(const struct world_state *world,
                                  int camera_x, int camera_y)
{
    int entity_index;
    for (entity_index = 0; entity_index < generated_runtime_actor_count;
         ++entity_index)
        record_entity_speech(world, entity_index,
                             world_actor_speech(world, entity_index),
                             camera_x, camera_y);
}

void platform_poll_actions(struct world_actions *actions)
{
    static int interaction_was_held;
    static int cancel_was_held;
    static int menu_was_held;
    static int previous_choice_direction;
    int interaction_held;
    int cancel_held;
    int menu_held;
    int choice_direction;
    select_gamepad(0);
    actions->move_x = gamepad_direction_x();
    actions->move_y = gamepad_direction_y();
    choice_direction = actions->move_y;
    actions->choice_move = choice_direction != 0 && choice_direction != previous_choice_direction ? choice_direction : 0;
    previous_choice_direction = choice_direction;
    interaction_held = gamepad_button_a() > 0 || gamepad_button_x() > 0;
    actions->interact_pressed = interaction_held && !interaction_was_held;
    interaction_was_held = interaction_held;
    cancel_held = gamepad_button_b() > 0 || gamepad_button_y() > 0;
    actions->cancel_pressed = cancel_held && !cancel_was_held;
    cancel_was_held = cancel_held;
    menu_held = gamepad_button_start() > 0;
    actions->menu_pressed = menu_held && !menu_was_held;
    menu_was_held = menu_held;
}

int platform_frame_counter(void)
{
    return get_frame_counter();
}

static void calibrate_cpu_frame(void)
{
    int previous = get_cycle_counter();
    int current;
    do {
        current = get_cycle_counter();
        if (current < previous)
            break;
        previous = current;
    } while (1);
    if (previous > 1000)
        cpu_cycles_per_frame = previous;
}

void platform_initialize(void)
{
    int index;
    int selected_texture = -1000;
    int persistent_glyph_count = generated_runtime_glyph_count;
    /* Vircon32 resets the SPU global gain independently of per-channel gain.
     * Set it explicitly, as the official console games do, so music channels
     * are audible from the first frame on both hardware and emulators. */
    set_global_volume(1.0f);
    for (index = 0; index < game_signature_words; ++index)
        game_card_signature[index] = generated_runtime_save_signature[index];
    calibrate_cpu_frame();
    if (persistent_glyph_count > LAST_GLYPH_REGION - FIRST_GLYPH_REGION + 1)
        persistent_glyph_count = LAST_GLYPH_REGION - FIRST_GLYPH_REGION + 1;
    for (index = 0; index < persistent_glyph_count; ++index) {
        const struct generated_runtime_glyph *glyph = &generated_runtime_glyphs[index];
        if (glyph->texture != selected_texture) {
            selected_texture = glyph->texture;
            select_texture(selected_texture);
        }
        select_region(LAST_GLYPH_REGION - index);
        set_region_minimum(glyph->min_x, glyph->min_y);
        set_region_maximum(glyph->max_x, glyph->max_y);
        set_region_hotspot(glyph->hotspot_x, glyph->min_y);
    }
}

int platform_random_seed(void)
{
    int seed = get_time();
    if (seed == 0)
        seed = 1;
    srand(seed);
    return rand();
}

/* Start one generated music track immediately on the shared music channel. */
void platform_play_music_track(int track_index)
{
    const struct generated_runtime_music *track;
    if (track_index < 0 || track_index >= generated_runtime_music_count)
        return;
    track = &generated_runtime_music[track_index];
    stop_channel(MUSIC_CHANNEL);
    select_sound(track->sound);
    set_sound_loop(track->loop);
    assign_channel_sound(MUSIC_CHANNEL, track->sound);
    set_channel_position(0);
    set_channel_volume(track->volume);
    set_channel_speed(track->speed);
    set_channel_loop(track->loop);
    play_channel(MUSIC_CHANNEL);
    active_music_track = track_index;
}

void platform_update_audio(const struct world_state *world)
{
    if (world->music_track < 0 || world->music_track >= generated_runtime_music_count)
        return;
    /* A revision represents an authored request to begin a track. Do not use
     * the channel's stopped state as another request: one-shot cues naturally
     * reach that state and must remain silent until a script selects music. */
    if (applied_music_revision == world->music_revision)
        return;
    applied_music_revision = world->music_revision;
    platform_play_music_track(world->music_track);
}

void platform_stop_audio(void)
{
    stop_channel(MUSIC_CHANNEL);
    applied_music_revision = -1;
    active_music_track = -1;
}

/* Return the last authored frame used by one cutscene layer. */
static int cinematic_layer_last_frame(
    const struct generated_runtime_cutscene_layer *layer)
{
    int last_frame = layer->alpha_frames;
    if (layer->x_keyframe_count > 0 &&
        layer->x_keyframes[layer->x_keyframe_count - 1].frame > last_frame)
        last_frame = layer->x_keyframes[layer->x_keyframe_count - 1].frame;
    if (layer->y_keyframe_count > 0 &&
        layer->y_keyframes[layer->y_keyframe_count - 1].frame > last_frame)
        last_frame = layer->y_keyframes[layer->y_keyframe_count - 1].frame;
    if (layer->alpha_keyframe_count > 0 &&
        layer->alpha_keyframes[layer->alpha_keyframe_count - 1].frame > last_frame)
        last_frame = layer->alpha_keyframes[layer->alpha_keyframe_count - 1].frame;
    return last_frame;
}

/* Return how many frames active range-clock layers still need to finish. */
static int cinematic_range_frames_remaining(
    const struct generated_runtime_dialogue *dialogue, int node)
{
    int index;
    int remaining = 0;
    for (index = 0; index < dialogue->cutscene_layer_count; ++index) {
        const struct generated_runtime_cutscene_layer *layer =
            &dialogue->cutscene_layers[index];
        int last_frame;
        int started;
        int elapsed;
        if (layer->clock_mode != GENERATED_CUTSCENE_CLOCK_RANGE ||
            node < layer->first_node || node > layer->last_node)
            continue;
        started = cinematic_node_started_at[layer->first_node];
        if (started < 0)
            continue;
        last_frame = cinematic_layer_last_frame(layer);
        elapsed = cinematic_sequence_frame - started;
        if (last_frame - elapsed > remaining)
            remaining = last_frame - elapsed;
    }
    return remaining;
}

/* Snap active range-clock tracks to their authored final keyframes. */
static void finish_cinematic_ranges(
    const struct generated_runtime_dialogue *dialogue, int node)
{
    int index;
    for (index = 0; index < dialogue->cutscene_layer_count; ++index) {
        const struct generated_runtime_cutscene_layer *layer =
            &dialogue->cutscene_layers[index];
        int started;
        int remaining;
        if (layer->clock_mode != GENERATED_CUTSCENE_CLOCK_RANGE ||
            node < layer->first_node || node > layer->last_node)
            continue;
        started = cinematic_node_started_at[layer->first_node];
        if (started < 0)
            continue;
        remaining = cinematic_layer_last_frame(layer) -
                    (cinematic_sequence_frame - started);
        if (remaining > 0)
            cinematic_node_started_at[layer->first_node] -= remaining;
    }
}

/* Enter one node of the shared compiled cutscene player. */
static void enter_cinematic_node(int node)
{
    const struct generated_runtime_dialogue *dialogue =
        &generated_runtime_dialogues[cinematic_dialogue];
    const struct generated_runtime_dialogue_node *entry;
    cinematic_node = node;
    cinematic_node_frame = 0;
    cinematic_visible_glyphs = 0;
    cinematic_text_tick = 0;
    cinematic_text_hold = 0;
    cinematic_node_started_at[node] = cinematic_sequence_frame;
    entry = &dialogue->nodes[node];
    if (entry->cutscene_finish_ranges)
        finish_cinematic_ranges(dialogue, node);
    if (entry->music_track >= 0 && entry->music_delay_frames == 0 &&
        active_music_track != entry->music_track)
        platform_play_music_track(entry->music_track);
}

/* Finish the current node, or the whole cutscene at its last node. */
static int advance_cinematic_node(void)
{
    const struct generated_runtime_dialogue *dialogue =
        &generated_runtime_dialogues[cinematic_dialogue];
    const struct generated_runtime_dialogue_node *node =
        &dialogue->nodes[cinematic_node];
    if (node->next_node >= 0) {
        const struct generated_runtime_dialogue_node *next =
            &dialogue->nodes[node->next_node];
        if (next->cutscene_finish_ranges &&
            cinematic_range_frames_remaining(dialogue, cinematic_node) > 0) {
            cinematic_pending_node = node->next_node;
            cinematic_visible_glyphs = node->display.glyph_count;
            return 0;
        }
        enter_cinematic_node(node->next_node);
        return 0;
    }
    cinematic_dialogue = -1;
    cinematic_pending_node = -1;
    return 1;
}

/* Start any dialogue authored with cutscene presentation. */
void platform_begin_cutscene(int dialogue, int start_node)
{
    int node;
    cinematic_dialogue = -1;
    if (dialogue < 0 || dialogue >= generated_runtime_dialogue_count ||
        generated_runtime_dialogues[dialogue].presentation_mode !=
            GENERATED_DIALOGUE_PRESENTATION_CUTSCENE)
        return;
    if (start_node < 0 ||
        start_node >= generated_runtime_dialogues[dialogue].node_count)
        return;
    cinematic_dialogue = dialogue;
    cinematic_sequence_frame = 0;
    cinematic_pending_node = -1;
    for (node = 0; node < GENERATED_RUNTIME_DIALOGUE_USED_STORAGE_COUNT; ++node)
        cinematic_node_started_at[node] = -1;
    enter_cinematic_node(start_node);
}

/* Return the current or final node for completion-hook resolution. */
int platform_cutscene_node(void)
{
    return cinematic_node;
}

/* Apply the current cutscene node's delayed music cue and preceding fade. */
static void update_cinematic_music(
    const struct generated_runtime_dialogue_node *node)
{
    if (node->music_track < 0 || active_music_track == node->music_track)
        return;
    if (node->music_fade_out_frames > 0 &&
        cinematic_node_frame < node->music_fade_out_frames &&
        active_music_track >= 0) {
        float remaining = (float)(node->music_fade_out_frames -
                                  cinematic_node_frame) /
                          (float)node->music_fade_out_frames;
        select_channel(MUSIC_CHANNEL);
        set_channel_volume(
            generated_runtime_music[active_music_track].volume * remaining);
    }
    if (cinematic_node_frame >= node->music_delay_frames)
        platform_play_music_track(node->music_track);
}

/* Advance the shared paused cutscene player. A reveals or advances one line. */
int platform_update_cutscene(const struct world_actions *actions)
{
    const struct generated_runtime_dialogue *dialogue;
    const struct generated_runtime_dialogue_node *node;
    const struct generated_runtime_text *text;
    int remaining;
    if (cinematic_dialogue < 0)
        return 1;
    dialogue = &generated_runtime_dialogues[cinematic_dialogue];
    node = &dialogue->nodes[cinematic_node];
    text = &node->display;
    if (actions->cancel_pressed || actions->menu_pressed) {
        cinematic_dialogue = -1;
        cinematic_pending_node = -1;
        return 1;
    }
    if (cinematic_pending_node >= 0) {
        int step;
        remaining = cinematic_range_frames_remaining(dialogue, cinematic_node);
        if (remaining <= 0) {
            int pending = cinematic_pending_node;
            cinematic_pending_node = -1;
            enter_cinematic_node(pending);
            return 0;
        }
        step = actions->interact_pressed ? remaining
                                        : CUTSCENE_RANGE_CATCH_UP_STEP;
        if (step > remaining)
            step = remaining;
        cinematic_node_frame += step;
        cinematic_sequence_frame += step;
        update_cinematic_music(node);
        /* Leave the completed endpoint visible for this render. The next
         * update enters the pending node, avoiding a one-frame visual skip. */
        return 0;
    }
    if (node->hold_at_end) {
        if (actions->interact_pressed || actions->cancel_pressed ||
            actions->menu_pressed) {
            cinematic_node_frame = node->auto_advance_frames;
            cinematic_visible_glyphs = text->glyph_count;
        } else if (cinematic_node_frame < node->auto_advance_frames) {
            ++cinematic_node_frame;
            ++cinematic_sequence_frame;
        }
        update_cinematic_music(node);
        return 0;
    }
    if (actions->interact_pressed) {
        if (node->cutscene_text_mode == GENERATED_CUTSCENE_TEXT_TYPEOVER &&
            cinematic_visible_glyphs < text->glyph_count) {
            cinematic_visible_glyphs = text->glyph_count;
            cinematic_text_tick = 0;
            cinematic_text_hold = 0;
            return 0;
        }
        return advance_cinematic_node();
    }
    ++cinematic_node_frame;
    ++cinematic_sequence_frame;
    update_cinematic_music(node);
    if (node->cutscene_text_mode == GENERATED_CUTSCENE_TEXT_TYPEOVER) {
        if (cinematic_visible_glyphs < text->glyph_count) {
            ++cinematic_text_tick;
            if (cinematic_text_tick >= node->cutscene_text_rate) {
                cinematic_text_tick = 0;
                ++cinematic_visible_glyphs;
            }
        } else {
            ++cinematic_text_hold;
            if (cinematic_text_hold >= node->cutscene_text_hold &&
                (node->auto_advance_frames <= 0 ||
                 cinematic_node_frame >= node->auto_advance_frames))
                return advance_cinematic_node();
        }
    } else if (node->auto_advance_frames > 0 &&
               cinematic_node_frame >= node->auto_advance_frames) {
        return advance_cinematic_node();
    }
    return 0;
}

static void prepare_world_frame(const struct world_state *world)
{
    int camera_x;
    int camera_y;
    int transition_alpha;
    draw_command_count = 0;
    camera_for_player(world, &camera_x, &camera_y);
    draw_background(world, camera_x, camera_y);
    draw_depth_sorted_objects(world, camera_x, camera_y);
    if (world_dialogue_is_open(world))
        record_dialogue_panel(world);
    else
        record_ambient_speech(world, camera_x, camera_y);
    transition_alpha = world_transition_alpha(world);
    if (transition_alpha > 0)
        record_solid_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                          (int)((unsigned int)transition_alpha << 24));
}

/* Draw a timed cinematic conversation as an actor-anchored speech balloon
 * over the live map instead of using the modal conversation panel. */
static void prepare_balloon_dialogue_frame(const struct world_state *world)
{
    const struct generated_runtime_dialogue_speaker *speaker =
        world_dialogue_speaker(world);
    int camera_x;
    int camera_y;
    int transition_alpha;
    draw_command_count = 0;
    camera_for_player(world, &camera_x, &camera_y);
    draw_background(world, camera_x, camera_y);
    draw_depth_sorted_objects(world, camera_x, camera_y);
    if (speaker != 0)
        record_entity_speech(world, speaker->entity,
                             world_dialogue_display(world),
                             camera_x, camera_y);
    transition_alpha = world_transition_alpha(world);
    if (transition_alpha > 0)
        record_solid_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                          (int)((unsigned int)transition_alpha << 24));
}

static void flush_prepared_frame(void)
{
    int selected_texture = -1000;
    int selected_region = -1;
    int region_x = -1;
    int region_y = -1;
    int region_width = -1;
    int region_height = -1;
    int color = 0;
    float scale_x = -1.0f;
    float scale_y = -1.0f;
    int index;

    set_blending_mode(0x20);
    clear_screen(COLOR_DARK_BACKGROUND);
    set_multiply_color(COLOR_WHITE);
    color = COLOR_WHITE;
    for (index = 0; index < draw_command_count; ++index) {
        int command_region = draw_command_region[index];
        int command_x = draw_command_region_x[index];
        int command_y = draw_command_region_y[index];
        int command_width = draw_command_region_width[index];
        int command_height = draw_command_region_height[index];
        int command_draw_x = draw_command_x[index];
        int command_draw_y = draw_command_y[index];

        if (draw_command_texture[index] != selected_texture) {
            selected_texture = draw_command_texture[index];
            select_texture(selected_texture);
            /* Region definitions are owned by textures. A texture switch
             * restores that texture's selected-region data, so cached region
             * identity and dimensions no longer describe the active state. */
            selected_region = -1;
            region_x = -1;
            region_y = -1;
            region_width = -1;
            region_height = -1;
        }
        if (command_region != selected_region) {
            selected_region = command_region;
            select_region(selected_region);
        }
        if (command_width > 0 &&
            (command_x != region_x || command_y != region_y ||
             command_width != region_width || command_height != region_height)) {
            region_x = command_x;
            region_y = command_y;
            region_width = command_width;
            region_height = command_height;
            define_region_rectangle(region_x, region_y, region_width, region_height);
        }
        if (draw_command_color[index] != color) {
            color = draw_command_color[index];
            set_multiply_color(color);
        }
        if (draw_command_scale_x[index] != scale_x || draw_command_scale_y[index] != scale_y) {
            scale_x = draw_command_scale_x[index];
            scale_y = draw_command_scale_y[index];
            set_drawing_scale(scale_x, scale_y);
        }
        set_drawing_point(command_draw_x, command_draw_y);
        draw_region_zoomed();
    }
    set_multiply_color(COLOR_WHITE);
    set_drawing_scale(1.0f, 1.0f);
}

/* Return a cutscene image width expressed in legacy 320-pixel source space. */
static int cutscene_logical_width(const struct generated_runtime_frame *frame)
{
    if (frame->native_size)
        return (frame->width + 1) / 2;
    return frame->width;
}

/* Record one cutscene image in the Vircon32 display space.
 *
 * Legacy artwork uses 320x240 source pixels and receives the original
 * 2x/1.5x display transform. Larger manifest rectangles are authored at
 * display resolution, so they retain their native pixel dimensions. */
static void record_cutscene_image(int image, int x, int y, int alpha)
{
    const struct generated_runtime_frame *frame;
    int source_x;
    int source_y;
    int width;
    int height;
    int color;
    if (image < 0 || image >= generated_runtime_cutscene_image_count)
        return;
    frame = &generated_runtime_cutscene_images[image];
    source_x = frame->source_x;
    source_y = frame->source_y;
    width = frame->width;
    height = frame->height;

    if (frame->native_size) {
        int display_x = x * 2;
        int display_y = (y * 3) / 2;

        if (display_x < 0) {
            source_x -= display_x;
            width += display_x;
            display_x = 0;
        }
        if (display_y < 0) {
            source_y -= display_y;
            height += display_y;
            display_y = 0;
        }
        if (display_x + width > DISPLAY_WIDTH)
            width = DISPLAY_WIDTH - display_x;
        if (display_y + height > DISPLAY_HEIGHT)
            height = DISPLAY_HEIGHT - display_y;
        if (width <= 0 || height <= 0)
            return;
        color = (int)(((unsigned int)alpha << 24) | COLOR_RGB_MASK);
        record_draw(frame->texture, 0, source_x, source_y,
                    width, height, display_x, display_y,
                    1.0f, 1.0f, color);
        return;
    }

    /* The approach artwork is taller than the viewport and moves through
     * negative coordinates. Crop it explicitly so partially visible images
     * keep drawing after their origin has moved above or left of the screen. */
    if (x < 0) {
        source_x -= x;
        width += x;
        x = 0;
    }
    if (y < 0) {
        source_y -= y;
        height += y;
        y = 0;
    }
    if (x + width > 320)
        width = 320 - x;
    if (y + height > 240)
        height = 240 - y;
    if (width <= 0 || height <= 0)
        return;

    color = (int)(((unsigned int)alpha << 24) | COLOR_RGB_MASK);
    record_draw(frame->texture, 0, source_x, source_y,
                width, height, x * 2, (y * 3) / 2,
                2.0f, 1.5f, color);
}

/* Evaluate a linearly interpolated integer track at the requested frame. */
static int cutscene_keyframed_value(
    const struct generated_runtime_cutscene_keyframe *keys,
    int key_count, int frame, int fallback)
{
    int index;
    if (keys == 0 || key_count <= 0)
        return fallback;
    if (frame <= keys[0].frame)
        return keys[0].value;
    for (index = 1; index < key_count; ++index) {
        int span;
        if (frame > keys[index].frame)
            continue;
        span = keys[index].frame - keys[index - 1].frame;
        return keys[index - 1].value +
               (keys[index].value - keys[index - 1].value) *
               (frame - keys[index - 1].frame) / span;
    }
    return keys[key_count - 1].value;
}

/* Resolve the reusable clock selected by one authored cutscene layer. */
static int cutscene_layer_clock(
    const struct generated_runtime_cutscene_layer *layer,
    int node, int node_frame, int sequence_frame, const int *node_started_at)
{
    if (layer->clock_mode == GENERATED_CUTSCENE_CLOCK_SEQUENCE)
        return sequence_frame;
    if (layer->clock_mode == GENERATED_CUTSCENE_CLOCK_RANGE &&
        node_started_at != 0 && node_started_at[layer->first_node] >= 0)
        return sequence_frame - node_started_at[layer->first_node];
    return node_frame;
}

/* Draw one authored cutscene layer using node, sequence, or range timing. */
static void record_cutscene_layer(
    const struct generated_runtime_cutscene_layer *layer,
    int node, int node_frame, int sequence_frame, const int *node_started_at)
{
    const struct generated_runtime_frame *source;
    int clock = cutscene_layer_clock(
        layer, node, node_frame, sequence_frame, node_started_at);
    int image_offset = 0;
    int image;
    int x;
    int y;
    int alpha = 255;
    if (node < layer->first_node || node > layer->last_node)
        return;
    if (layer->image_count > 1 && layer->frame_delay > 0)
        image_offset = (clock / layer->frame_delay) % layer->image_count;
    image = layer->images[image_offset];
    if (image < 0 || image >= generated_runtime_cutscene_image_count)
        return;
    source = &generated_runtime_cutscene_images[image];
    x = cutscene_keyframed_value(layer->x_keyframes,
                                 layer->x_keyframe_count, clock, layer->x);
    y = cutscene_keyframed_value(layer->y_keyframes,
                                 layer->y_keyframe_count, clock, layer->y);
    alpha = cutscene_keyframed_value(layer->alpha_keyframes,
                                     layer->alpha_keyframe_count, clock, alpha);
    if (layer->alpha_frames > 0 && clock < layer->alpha_frames)
        alpha = clock * 255 / layer->alpha_frames;
    if (layer->wrap_x_delay > 0) {
        int logical_width = cutscene_logical_width(source);
        x += ((clock / layer->wrap_x_delay) * layer->wrap_x_step) % logical_width;
        while (x <= -logical_width)
            x += logical_width;
        while (x >= logical_width)
            x -= logical_width;
        record_cutscene_image(image, x, y, alpha);
        if (x > 0)
            record_cutscene_image(image, x - logical_width, y, alpha);
        else if (x + logical_width < 320)
            record_cutscene_image(image, x + logical_width, y, alpha);
        return;
    }
    record_cutscene_image(image, x, y, alpha);
}

/* Draw text with the presentation selected by compiled dialogue data. */
static void record_cutscene_text(const struct generated_runtime_dialogue *dialogue,
                                 int node_index, int node_frame,
                                 int visible_glyphs)
{
    const struct generated_runtime_dialogue_node *node = &dialogue->nodes[node_index];
    const struct generated_runtime_text *text = &node->display;
    const struct generated_runtime_dialogue_speaker *speaker =
        &dialogue->speakers[node->speaker];
    unsigned int color = speaker->speech_color;
    if (node->cutscene_text_mode == GENERATED_CUTSCENE_TEXT_NONE)
        return;
    if (node->cutscene_text_mode == GENERATED_CUTSCENE_TEXT_BUBBLE) {
        int text_width;
        int text_height;
        int x = node->cutscene_text_x;
        measure_runtime_text(text, node->cutscene_text_width,
                             &text_width, &text_height);
        if (x < 0)
            x = (DISPLAY_WIDTH - text_width - SPEECH_PADDING_X * 2) / 2;
        if (x < 8)
            x = 8;
        if (x + text_width + SPEECH_PADDING_X * 2 > DISPLAY_WIDTH - 8)
            x = DISPLAY_WIDTH - 8 - text_width - SPEECH_PADDING_X * 2;
        record_speech_box(x, node->cutscene_text_y, text_width, text_height,
                          text, color, -1, -1);
    } else if (node->cutscene_text_mode == GENERATED_CUTSCENE_TEXT_TYPEOVER) {
        struct generated_runtime_text visible = *text;
        int glyphs = visible_glyphs >= 0
                         ? visible_glyphs
                         : node_frame / node->cutscene_text_rate;
        if (glyphs < visible.glyph_count)
            visible.glyph_count = glyphs;
        if (node->cutscene_text_shadow)
            record_runtime_text(node->cutscene_text_x + 1,
                                node->cutscene_text_y + 1,
                                node->cutscene_text_width,
                                node->cutscene_text_bottom + 1,
                                &visible, COLOR_TEXT_SHADOW);
        record_runtime_text(node->cutscene_text_x, node->cutscene_text_y,
                            node->cutscene_text_width,
                            node->cutscene_text_bottom,
                            &visible, color);
    } else if (node->cutscene_text_mode == GENERATED_CUTSCENE_TEXT_SCROLL) {
        int y = node->cutscene_text_y -
                node_frame / node->cutscene_text_rate;
        record_runtime_text_centered_lines(node->cutscene_text_x, y,
                                           node->cutscene_text_bottom,
                                           text, color);
    }
}

/* Render the current frame of any compiled cutscene. */
void platform_render_cutscene(void)
{
    const struct generated_runtime_dialogue *dialogue;
    int layer;
    int fade_alpha;
    draw_command_count = 0;
    if (cinematic_dialogue < 0)
        return;
    dialogue = &generated_runtime_dialogues[cinematic_dialogue];
    if (dialogue->background_color != 0)
        record_solid_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                          dialogue->background_color);
    for (layer = 0; layer < dialogue->cutscene_layer_count; ++layer)
        record_cutscene_layer(&dialogue->cutscene_layers[layer], cinematic_node,
                              cinematic_node_frame, cinematic_sequence_frame,
                              cinematic_node_started_at);
    record_cutscene_text(dialogue, cinematic_node, cinematic_node_frame,
                         cinematic_visible_glyphs);
    fade_alpha = dialogue->fade_in_frames - cinematic_sequence_frame;
    if (fade_alpha > 0) {
        fade_alpha = fade_alpha * 255 / dialogue->fade_in_frames;
        record_solid_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                          (int)((unsigned int)fade_alpha << 24));
    }
    flush_prepared_frame();
}

static int dialogue_signature_matches(int dialogue, int node, int choice,
                                      int expected_dialogue, int expected_node,
                                      int expected_choice)
{
    return dialogue == expected_dialogue && node == expected_node && choice == expected_choice;
}

static void remember_prepared_world(const struct world_state *world)
{
    prepared_scene = world->scene;
    prepared_player_x = world->player_x;
    prepared_player_y = world->player_y;
    prepared_player_facing = world->player_facing;
    prepared_moving = world->moving;
    prepared_move_x = world->move_x;
    prepared_move_y = world->move_y;
    prepared_move_progress = world->move_progress;
    prepared_animation_frame = world->animation_frame;
    prepared_visual_revision = world->visual_revision;
}

static void remember_displayed_world(void)
{
    displayed_scene = prepared_scene;
    displayed_player_x = prepared_player_x;
    displayed_player_y = prepared_player_y;
    displayed_player_facing = prepared_player_facing;
    displayed_moving = prepared_moving;
    displayed_move_x = prepared_move_x;
    displayed_move_y = prepared_move_y;
    displayed_move_progress = prepared_move_progress;
    displayed_animation_frame = prepared_animation_frame;
    displayed_visual_revision = prepared_visual_revision;
    displayed_world_valid = 1;
}

static int displayed_world_matches(const struct world_state *world)
{
    return displayed_world_valid && displayed_scene == world->scene &&
           displayed_player_x == world->player_x &&
           displayed_player_y == world->player_y &&
           displayed_player_facing == world->player_facing &&
           displayed_moving == world->moving &&
           displayed_move_x == world->move_x &&
           displayed_move_y == world->move_y &&
           displayed_move_progress == world->move_progress &&
           displayed_animation_frame == world->animation_frame &&
           displayed_visual_revision == world->visual_revision;
}

static int enough_time_to_flush(void)
{
    int estimated_cycles = DRAW_FIXED_CYCLES +
                           draw_command_count * DRAW_CYCLES_PER_COMMAND +
                           DRAW_SAFETY_CYCLES;
    return get_cycle_counter() + estimated_cycles < cpu_cycles_per_frame;
}

static void finish_prepared_frame(void)
{
    flush_prepared_frame();
    prepared_frame_valid = 0;
    if (prepared_is_dialogue) {
        displayed_dialogue = prepared_dialogue;
        displayed_node = prepared_node;
        displayed_choice = prepared_choice;
        remember_displayed_world();
    } else {
        displayed_dialogue = -1;
        remember_displayed_world();
    }
}

void platform_render_world(const struct world_state *world)
{
    const struct generated_runtime_dialogue *dialogue =
        world_dialogue_is_open(world)
            ? &generated_runtime_dialogues[world->dialogue] : 0;
    int preparation_frame;

    if (dialogue != 0 && dialogue->presentation_mode ==
                             GENERATED_DIALOGUE_PRESENTATION_MAP_BALLOON) {
        prepared_frame_valid = 0;
        displayed_world_valid = 0;
        prepare_balloon_dialogue_frame(world);
        flush_prepared_frame();
        displayed_dialogue = world->dialogue;
        displayed_node = world->dialogue_node;
        displayed_choice = world->dialogue_choice;
        return;
    }
    /* A late compute pass leaves behind a complete RAM command list without
     * touching the GPU. Give that list a draw-only frame before calculating a
     * newer view. The player simulation has already continued in main. */
    if (prepared_frame_valid) {
        if (enough_time_to_flush())
            finish_prepared_frame();
        return;
    }

    if (world_dialogue_is_open(world)) {
        int dialogue = world->dialogue;
        int node = world->dialogue_node;
        int choice = world->dialogue_choice;

        if (dialogue_signature_matches(dialogue, node, choice,
                                       displayed_dialogue, displayed_node, displayed_choice) &&
            displayed_world_matches(world)) {
            return;
        }

        /* Dialogue is relatively static and glyph-heavy. Always give it the
         * existing compute/draw split rather than gambling on a partial panel.
         * The original kept updating and drawing the world underneath modal
         * dialogue, so retain the frame only while that world snapshot also
         * matches. Otherwise schedules would advance invisibly and appear to
         * jump whenever a new dialogue node forced the next refresh. */
        prepare_world_frame(world);
        remember_prepared_world(world);
        prepared_dialogue = dialogue;
        prepared_node = node;
        prepared_choice = choice;
        prepared_is_dialogue = 1;
        prepared_frame_valid = 1;
        return;
    }

    /* A matching world snapshot rendered with a dialogue panel is not a valid
     * panel-free frame. Closing dialogue must therefore restore the world even
     * when no simulation state changed during the conversation. */
    if (displayed_dialogue >= 0)
        displayed_world_valid = 0;
    displayed_dialogue = -1;
    if (displayed_world_matches(world))
        return;

    preparation_frame = get_frame_counter();
    prepare_world_frame(world);
    remember_prepared_world(world);
    /* Actors use the complete scene replay path. Keeping the player on that
     * same path avoids phase differences between cropped and full textures,
     * which can leave one-pixel trails while the player moves. */
    prepared_is_dialogue = 0;
    prepared_frame_valid = 1;

    /* Fast path: reach 60 FPS when update + command preparation leaves enough
     * of this hardware frame for a complete replay. If calculation crossed a
     * frame boundary or ran too late, retain the old screen and draw this list
     * on a later frame instead. */
    if (get_frame_counter() != preparation_frame || !enough_time_to_flush())
        return;

    finish_prepared_frame();
}

static int save_slot_offset(int slot)
{
    return game_signature_words + slot * SAVE_SLOT_STRIDE;
}

static int storage_card_status(void)
{
    if (!card_is_connected())
        return PLATFORM_STORAGE_NO_CARD;
    if (card_is_empty())
        return PLATFORM_STORAGE_OK;
    if (!card_signature_matches(&game_card_signature))
        return PLATFORM_STORAGE_FOREIGN_CARD;
    return PLATFORM_STORAGE_OK;
}

int platform_query_save_slots(int slot_states[PLATFORM_SAVE_SLOT_COUNT])
{
    int status = storage_card_status();
    int slot;
    displayed_slot_menu_valid = 0;
    for (slot = 0; slot < PLATFORM_SAVE_SLOT_COUNT; ++slot) {
        slot_states[slot] = PLATFORM_SAVE_SLOT_EMPTY;
        save_slot_preview_valid[slot] = 0;
    }
    if (status != PLATFORM_STORAGE_OK || card_is_empty())
        return status;
    for (slot = 0; slot < PLATFORM_SAVE_SLOT_COUNT; ++slot) {
        int offset = save_slot_offset(slot);
        int word_count;
        unsigned int expected_checksum;
        if (card_read_word(offset) != generated_runtime_save_magic)
            continue;
        word_count = card_read_word(offset + 3);
        if (card_read_word(offset + 1) < SAVEGAME_MINIMUM_VERSION ||
            card_read_word(offset + 1) > SAVEGAME_FORMAT_VERSION ||
            (unsigned int)card_read_word(offset + 2) != savegame_content_fingerprint() ||
            word_count <= 0 || word_count > SAVEGAME_MAX_WORDS) {
            slot_states[slot] = PLATFORM_SAVE_SLOT_INVALID;
            continue;
        }
        expected_checksum = (unsigned int)card_read_word(offset + 4);
        card_read_words(save_words, offset + SAVE_SLOT_HEADER_WORDS, word_count);
        if (savegame_checksum(save_words, word_count) != expected_checksum ||
            !savegame_decode(&save_slot_previews[slot], save_words, word_count)) {
            slot_states[slot] = PLATFORM_SAVE_SLOT_INVALID;
            continue;
        }
        slot_states[slot] = PLATFORM_SAVE_SLOT_USED;
        save_slot_preview_valid[slot] = 1;
    }
    return PLATFORM_STORAGE_OK;
}

int platform_save_slot(const struct world_state *world, int slot)
{
    int status;
    int offset;
    int word_count;
    unsigned int checksum;
    if (slot < 0 || slot >= PLATFORM_SAVE_SLOT_COUNT)
        return PLATFORM_STORAGE_WRITE_FAILED;
    status = storage_card_status();
    if (status != PLATFORM_STORAGE_OK)
        return status;
    word_count = savegame_encode(world, save_words, SAVEGAME_MAX_WORDS);
    if (word_count <= 0)
        return PLATFORM_STORAGE_WRITE_FAILED;
    checksum = savegame_checksum(save_words, word_count);
    if (card_is_empty()) {
        card_write_signature(&game_card_signature);
        if (!card_signature_matches(&game_card_signature))
            return PLATFORM_STORAGE_WRITE_FAILED;
    }
    offset = save_slot_offset(slot);
    /* Clear the commit marker first. A reset or removed card during the write
     * leaves an invalid slot instead of a valid-looking partial save. */
    card_write_word(offset, 0);
    card_write_word(offset + 1, SAVEGAME_FORMAT_VERSION);
    card_write_word(offset + 2, (int)savegame_content_fingerprint());
    card_write_word(offset + 3, word_count);
    card_write_word(offset + 4, (int)checksum);
    card_write_word(offset + 5, world->scene_index);
    card_write_word(offset + 6, get_date());
    card_write_word(offset + 7, get_time());
    card_write_words(save_words, offset + SAVE_SLOT_HEADER_WORDS, word_count);
    card_write_word(offset, generated_runtime_save_magic);
    if (card_read_word(offset) != generated_runtime_save_magic ||
        (unsigned int)card_read_word(offset + 4) != checksum)
        return PLATFORM_STORAGE_WRITE_FAILED;
    return PLATFORM_STORAGE_SAVED;
}

int platform_load_slot(struct world_state *world, int slot)
{
    int status;
    int offset;
    int word_count;
    unsigned int expected_checksum;
    if (slot < 0 || slot >= PLATFORM_SAVE_SLOT_COUNT)
        return PLATFORM_STORAGE_EMPTY_SLOT;
    status = storage_card_status();
    if (status != PLATFORM_STORAGE_OK)
        return status;
    if (card_is_empty())
        return PLATFORM_STORAGE_EMPTY_SLOT;
    offset = save_slot_offset(slot);
    if (card_read_word(offset) != generated_runtime_save_magic)
        return PLATFORM_STORAGE_EMPTY_SLOT;
    if (card_read_word(offset + 1) < SAVEGAME_MINIMUM_VERSION ||
        card_read_word(offset + 1) > SAVEGAME_FORMAT_VERSION ||
        (unsigned int)card_read_word(offset + 2) != savegame_content_fingerprint())
        return PLATFORM_STORAGE_INCOMPATIBLE_SAVE;
    word_count = card_read_word(offset + 3);
    if (word_count <= 0 || word_count > SAVEGAME_MAX_WORDS)
        return PLATFORM_STORAGE_CORRUPT_SLOT;
    expected_checksum = (unsigned int)card_read_word(offset + 4);
    card_read_words(save_words, offset + SAVE_SLOT_HEADER_WORDS, word_count);
    if (savegame_checksum(save_words, word_count) != expected_checksum)
        return PLATFORM_STORAGE_CORRUPT_SLOT;
    if (!savegame_decode(world, save_words, word_count))
        return PLATFORM_STORAGE_INCOMPATIBLE_SAVE;
    applied_music_revision = -1;
    prepared_frame_valid = 0;
    displayed_world_valid = 0;
    displayed_dialogue = -1;
    return PLATFORM_STORAGE_OK;
}

static void draw_direct_rectangle(int x, int y, int width, int height, int color)
{
    select_texture(-1);
    select_region(256);
    set_multiply_color(color);
    set_drawing_scale((float)width, (float)height);
    set_drawing_point(x, y);
    draw_region_zoomed();
    set_drawing_scale(1.0f, 1.0f);
}

static void draw_preview_frame(const struct generated_runtime_frame *frame,
                               int world_x, int world_y, int alpha,
                               int crop_x, int crop_y, int crop_size,
                               int destination_x, int destination_y)
{
    int left = world_x > crop_x ? world_x : crop_x;
    int top = world_y > crop_y ? world_y : crop_y;
    int right = world_x + frame->width;
    int bottom = world_y + frame->height;
    int crop_right = crop_x + crop_size;
    int crop_bottom = crop_y + crop_size;
    int output_left;
    int output_top;
    int output_right;
    int output_bottom;
    int source_width;
    int source_height;
    if (right > crop_right) right = crop_right;
    if (bottom > crop_bottom) bottom = crop_bottom;
    if (right <= left || bottom <= top || frame->texture < 0)
        return;
    output_left = destination_x + ((left - crop_x) * SAVE_PREVIEW_SIZE) / crop_size;
    output_top = destination_y + ((top - crop_y) * SAVE_PREVIEW_SIZE) / crop_size;
    output_right = destination_x + ((right - crop_x) * SAVE_PREVIEW_SIZE) / crop_size;
    output_bottom = destination_y + ((bottom - crop_y) * SAVE_PREVIEW_SIZE) / crop_size;
    source_width = right - left;
    source_height = bottom - top;
    if (output_right <= output_left || output_bottom <= output_top)
        return;
    select_texture(frame->texture);
    select_region(0);
    define_region_rectangle(frame->source_x + left - world_x,
                            frame->source_y + top - world_y,
                            source_width, source_height);
    set_multiply_color((int)(((unsigned int)alpha << 24) | COLOR_RGB_MASK));
    set_drawing_scale((float)(output_right - output_left) / (float)source_width,
                      (float)(output_bottom - output_top) / (float)source_height);
    set_drawing_point(output_left, output_top);
    draw_region_zoomed();
}

static void draw_preview_entity(const struct world_state *world, int definition,
                                int crop_x, int crop_y, int crop_size,
                                int destination_x, int destination_y)
{
    int visual_index = world_actor_visual(world, definition);
    const struct generated_runtime_actor_visual *visual;
    const struct generated_runtime_frame *frame;
    int x;
    int y;
    if (visual_index < 0 || visual_index >= generated_runtime_actor_visual_count)
        return;
    visual = &generated_runtime_actor_visuals[visual_index];
    frame = entity_frame(world, definition);
    x = world_actor_pixel_x(world, definition) -
        visual->base_x * world->scene->tile_size + frame->offset_x;
    y = world_actor_pixel_y(world, definition) -
        visual->base_y * world->scene->tile_size + frame->offset_y;
    draw_preview_frame(frame, x, y, 255, crop_x, crop_y, crop_size,
                       destination_x, destination_y);
}

static void draw_slot_preview(const struct world_state *world, int x, int y)
{
    const struct generated_runtime_scene *scene = world->scene;
    int crop_size = SAVE_PREVIEW_TILES * scene->tile_size;
    int crop_x = world_actor_pixel_x(world,
        scene->entities[world->player_entity].definition) + scene->tile_size / 2 - crop_size / 2;
    int crop_y = world_actor_pixel_y(world,
        scene->entities[world->player_entity].definition) + scene->tile_size / 2 - crop_size / 2;
    int maximum_x = scene->width * scene->tile_size - crop_size;
    int maximum_y = scene->height * scene->tile_size - crop_size;
    int scenery_index = 0;
    int drawn = 0;
    int last_y = -1;
    int last_index = -1;
    int index;
    if (maximum_x < 0)
        crop_x = maximum_x / 2;
    else {
        if (crop_x < 0) crop_x = 0;
        if (crop_x > maximum_x) crop_x = maximum_x;
    }
    if (maximum_y < 0)
        crop_y = maximum_y / 2;
    else {
        if (crop_y < 0) crop_y = 0;
        if (crop_y > maximum_y) crop_y = maximum_y;
    }

    draw_direct_rectangle(x, y, SAVE_PREVIEW_SIZE, SAVE_PREVIEW_SIZE,
                          COLOR_SAVE_PREVIEW_BACKGROUND);
    for (index = 0; index < scene->background_count; ++index) {
        const struct generated_runtime_background *item = &scene->background[index];
        draw_preview_frame(&item->frame, item->x, item->y, 255,
                           crop_x, crop_y, crop_size, x, y);
    }
    while (drawn < generated_runtime_actor_count) {
        int selected = -1;
        int selected_y = 0;
        for (index = 0; index < generated_runtime_actor_count; ++index) {
            int depth = entity_depth(world, index);
            if (world_actor_visual(world, index) < 0 ||
                !world_actor_is_present(world, index) ||
                depth < last_y || (depth == last_y && index <= last_index))
                continue;
            if (selected < 0 || depth < selected_y ||
                (depth == selected_y && index < selected)) {
                selected = index;
                selected_y = depth;
            }
        }
        if (selected < 0)
            break;
        while (scenery_index < scene->scenery_count &&
               scene->scenery[scenery_index].depth <= selected_y) {
            const struct generated_runtime_scenery *item = &scene->scenery[scenery_index++];
            draw_preview_frame(world_scenery_frame(world, item), item->x, item->y,
                               item->alpha, crop_x, crop_y, crop_size, x, y);
        }
        draw_preview_entity(world, selected, crop_x, crop_y, crop_size, x, y);
        last_y = selected_y;
        last_index = selected;
        ++drawn;
    }
    while (scenery_index < scene->scenery_count) {
        const struct generated_runtime_scenery *item = &scene->scenery[scenery_index++];
        draw_preview_frame(world_scenery_frame(world, item), item->x, item->y,
                           item->alpha, crop_x, crop_y, crop_size, x, y);
    }
    set_multiply_color(COLOR_WHITE);
    set_drawing_scale(1.0f, 1.0f);
}

static void draw_direct_text_centered(int y, const char *text, int color)
{
    int width = (int)strlen(text) * bios_character_width;
    set_drawing_scale(1.0f, 1.0f);
    set_multiply_color(color);
    print_at((DISPLAY_WIDTH - width) / 2, y, text);
}

static const char *storage_message(int result)
{
    if (result == PLATFORM_STORAGE_NO_CARD) return "NO MEMORY CARD";
    if (result == PLATFORM_STORAGE_FOREIGN_CARD) return "CARD BELONGS TO ANOTHER GAME";
    if (result == PLATFORM_STORAGE_EMPTY_SLOT) return "EMPTY SLOT";
    if (result == PLATFORM_STORAGE_CORRUPT_SLOT) return "SAVE DATA IS CORRUPT";
    if (result == PLATFORM_STORAGE_INCOMPATIBLE_SAVE) return "SAVE IS FROM ANOTHER BUILD";
    if (result == PLATFORM_STORAGE_WRITE_FAILED) return "COULD NOT SAVE GAME";
    if (result == PLATFORM_STORAGE_SAVED) return "GAME SAVED";
    return "";
}

static void draw_title_background(void)
{
    const struct generated_runtime_frame *frame = &generated_runtime_title_background;
    clear_screen(COLOR_DARK_BACKGROUND);
    if (frame->texture >= 0 && frame->width > 0 && frame->height > 0) {
        select_texture(frame->texture);
        select_region(0);
        define_region_rectangle(frame->source_x, frame->source_y,
                                frame->width, frame->height);
        set_multiply_color(COLOR_WHITE);
        if (frame->native_size)
            set_drawing_scale(1.0f, 1.0f);
        else
            set_drawing_scale((float)DISPLAY_WIDTH / (float)frame->width,
                              (float)DISPLAY_HEIGHT / (float)frame->height);
        set_drawing_point(0, 0);
        draw_region_zoomed();
        set_drawing_scale(1.0f, 1.0f);
    }
}

/* Draw the optional manifest-authored logo in original 320x240 coordinates. */
static void draw_title_logo(void)
{
    const struct generated_runtime_frame *frame = &generated_runtime_title_logo;
    if (frame->texture < 0 || frame->width <= 0 || frame->height <= 0)
        return;
    select_texture(frame->texture);
    select_region(0);
    define_region_rectangle(frame->source_x, frame->source_y,
                            frame->width, frame->height);
    set_multiply_color(COLOR_WHITE);
    if (frame->native_size)
        set_drawing_scale(1.0f, 1.0f);
    else
        set_drawing_scale(2.0f, 1.5f);
    set_drawing_point(frame->offset_x * 2, (frame->offset_y * 3) / 2);
    draw_region_zoomed();
    set_drawing_scale(1.0f, 1.0f);
}

static void draw_menu_panel(void)
{
    set_blending_mode(0x20);
    draw_direct_rectangle(108, 28, 424, 304, COLOR_PANEL_BORDER);
    draw_direct_rectangle(112, 32, 416, 296, COLOR_PANEL_BACKGROUND);
}

static void draw_menu_option(int y, const char *text, int selected)
{
    if (selected)
        draw_direct_rectangle(174, y - 4, 292, 28, COLOR_MENU_SELECTION);
    draw_direct_text_centered(y, text, selected ? COLOR_ACCENT_GOLD : COLOR_WHITE);
}

static void draw_menu_status(int result)
{
    const char *message = storage_message(result);
    if (message[0] != 0)
        draw_direct_text_centered(298, message,
                                  result == PLATFORM_STORAGE_SAVED ?
                                  COLOR_STATUS_SUCCESS : COLOR_STATUS_INFO);
}

static void draw_two_digits(int x, int y, unsigned int value)
{
    value %= 100;
    if (value < 10) {
        print_at(x, y, "0");
        x += bios_character_width;
    }
    print_uint_at(x, y, value);
}

static void draw_save_time(int x, int y, int game_time)
{
    unsigned int time = game_time < 0 ? 0u : (unsigned int)game_time;
    unsigned int day = time / (600u * 24u);
    unsigned int hour = (time / 600u) % 24u;
    unsigned int minute = (time / 10u) % 60u;
    print_at(x, y, "DAY");
    print_uint_at(x + bios_character_width * 4, y, day);
    print_at(x + bios_character_width * 9, y, "-");
    draw_two_digits(x + bios_character_width * 11, y, hour);
    print_at(x + bios_character_width * 13, y, ":");
    draw_two_digits(x + bios_character_width * 14, y, minute);
}

static int slot_menu_matches(int from_title, int saving, int selected,
                             const int slot_states[PLATFORM_SAVE_SLOT_COUNT],
                             int storage_result)
{
    int slot;
    if (!displayed_slot_menu_valid ||
        displayed_slot_menu_from_title != from_title ||
        displayed_slot_menu_saving != saving ||
        displayed_slot_menu_selected != selected ||
        displayed_slot_menu_result != storage_result)
        return 0;
    for (slot = 0; slot < PLATFORM_SAVE_SLOT_COUNT; ++slot)
        if (displayed_slot_menu_states[slot] != slot_states[slot])
            return 0;
    return 1;
}

static void remember_slot_menu(int from_title, int saving, int selected,
                               const int slot_states[PLATFORM_SAVE_SLOT_COUNT],
                               int storage_result)
{
    int slot;
    displayed_slot_menu_valid = 1;
    displayed_slot_menu_from_title = from_title;
    displayed_slot_menu_saving = saving;
    displayed_slot_menu_selected = selected;
    displayed_slot_menu_result = storage_result;
    for (slot = 0; slot < PLATFORM_SAVE_SLOT_COUNT; ++slot)
        displayed_slot_menu_states[slot] = slot_states[slot];
}

void platform_begin_menu(void)
{
    /* A prepared display list must not be replayed over a menu on a later
     * frame. Keep the currently displayed world as the pause backdrop. */
    prepared_frame_valid = 0;
}

void platform_invalidate_world(void)
{
    prepared_frame_valid = 0;
    displayed_world_valid = 0;
    displayed_dialogue = -1;
}

void platform_render_title_menu(int selected, int storage_result)
{
    draw_title_background();
    draw_title_logo();
    draw_direct_text_centered(96, generated_runtime_game_title, COLOR_ACCENT_GOLD);
    draw_menu_option(164, "NEW GAME", selected == 0);
    draw_menu_option(204, "LOAD GAME", selected == 1);
    draw_menu_status(storage_result);
    set_multiply_color(COLOR_WHITE);
}

void platform_render_pause_menu(int selected, int storage_result)
{
    draw_menu_panel();
    draw_direct_text_centered(58, "GAME PAUSED", COLOR_ACCENT_GOLD);
    draw_menu_option(112, "CONTINUE", selected == 0);
    draw_menu_option(152, "SAVE GAME", selected == 1);
    draw_menu_option(192, "LOAD GAME", selected == 2);
    draw_menu_option(232, "BACK TO TITLE", selected == 3);
    draw_menu_status(storage_result);
    set_multiply_color(COLOR_WHITE);
}

void platform_render_slot_menu(int from_title, int saving, int selected,
                               const int slot_states[PLATFORM_SAVE_SLOT_COUNT],
                               int storage_result)
{
    int slot;
    const char *message;
    if (slot_menu_matches(from_title, saving, selected, slot_states, storage_result))
        return;
    if (from_title)
        draw_title_background();
    set_blending_mode(0x20);
    draw_direct_rectangle(36, 6, 568, 348, COLOR_PANEL_BORDER);
    draw_direct_rectangle(40, 10, 560, 340, COLOR_PANEL_BACKGROUND);
    draw_direct_text_centered(28, saving ? "SAVE GAME" : "LOAD GAME", COLOR_ACCENT_GOLD);
    for (slot = 0; slot < PLATFORM_SAVE_SLOT_COUNT; ++slot) {
        int y = 56 + slot * 64;
        int used = slot_states[slot] == PLATFORM_SAVE_SLOT_USED &&
                   save_slot_preview_valid[slot];
        draw_direct_rectangle(58, y, 524, 62,
                              slot == selected ? COLOR_PANEL_BORDER : COLOR_SLOT_EMPTY_BORDER);
        draw_direct_rectangle(60, y + 2, 520, 58,
                              slot == selected ? COLOR_SLOT_SELECTED_BACKGROUND :
                              COLOR_SLOT_BACKGROUND);
        draw_direct_rectangle(62, y, 62, 62,
                              slot == selected ? COLOR_SLOT_SELECTED_TEXT : COLOR_SLOT_TEXT);
        if (used)
            draw_slot_preview(&save_slot_previews[slot], 63, y + 1);
        else {
            draw_direct_rectangle(63, y + 1, SAVE_PREVIEW_SIZE,
                                  SAVE_PREVIEW_SIZE, COLOR_SAVE_PREVIEW_BACKGROUND);
            set_multiply_color(slot_states[slot] == PLATFORM_SAVE_SLOT_INVALID ?
                               COLOR_STATUS_ERROR : COLOR_SLOT_SELECTED_METADATA);
            print_at(slot_states[slot] == PLATFORM_SAVE_SLOT_INVALID ? 65 : 73,
                     y + 23,
                     slot_states[slot] == PLATFORM_SAVE_SLOT_INVALID ? "ERROR" : "EMPTY");
        }
        set_multiply_color(slot == selected ? COLOR_SLOT_SELECTED_TEXT : COLOR_WHITE);
        print_at(138, y + 9, used ? save_slot_previews[slot].scene->display_name :
                 (slot_states[slot] == PLATFORM_SAVE_SLOT_INVALID ?
                  "INCOMPATIBLE SAVE" : "EMPTY SLOT"));
        print_at(508, y + 9, "SLOT");
        print_uint_at(556, y + 9, (unsigned)(slot + 1));
        if (used)
            draw_save_time(138, y + 33, save_slot_previews[slot].game_time);
    }
    message = storage_message(storage_result);
    if (message[0] != 0)
        draw_direct_text_centered(330, message,
                                  storage_result == PLATFORM_STORAGE_SAVED ?
                                  COLOR_STATUS_SUCCESS : COLOR_STATUS_INFO);
    set_multiply_color(COLOR_WHITE);
    set_drawing_scale(1.0f, 1.0f);
    remember_slot_menu(from_title, saving, selected, slot_states, storage_result);
}

void platform_present(void)
{
    end_frame();
}
