#include "platform/vircon32/vircon.h"

#include "platform/vircon32/platform.h"

#define GAME_WIDTH 427
#define GAME_HEIGHT 240
#define GAME_SCALE 1.5f
#define DISPLAY_WIDTH 640
#define DISPLAY_HEIGHT 360
#define VIEW_WIDTH DISPLAY_WIDTH
#define VIEW_X ((DISPLAY_WIDTH - VIEW_WIDTH) / 2)
#define DIALOG_X (VIEW_X + 12)
#define DIALOG_Y 88
#define DIALOG_WIDTH (VIEW_WIDTH - 24)
#define DIALOG_HEIGHT 260
#define DIALOG_LINE_HEIGHT 19
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
/* Partial replays convert between the 427x240 game grid and the 640x360
 * display grid. The 1.5x rasterizer and integer source clipping can touch a
 * few pixels beyond the mathematical sprite rectangle. Keep a conservative
 * display-space halo so no edge sample from the previous player frame can be
 * left behind as movement advances the dirty region. */
#define DIRTY_PADDING 6
#define MUSIC_CHANNEL 0

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
static int ambient_overlay_count;
static int ambient_overlay_left[GENERATED_RUNTIME_ENTITY_STORAGE_COUNT];
static int ambient_overlay_top[GENERATED_RUNTIME_ENTITY_STORAGE_COUNT];
static int ambient_overlay_right[GENERATED_RUNTIME_ENTITY_STORAGE_COUNT];
static int ambient_overlay_bottom[GENERATED_RUNTIME_ENTITY_STORAGE_COUNT];

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
static int prepared_camera_x;
static int prepared_camera_y;
static int prepared_player_left;
static int prepared_player_top;
static int prepared_player_right;
static int prepared_player_bottom;
static int prepared_dirty_valid;
static int prepared_dirty_left;
static int prepared_dirty_top;
static int prepared_dirty_right;
static int prepared_dirty_bottom;
static int prepared_replay_commands;
static int displayed_dialogue = -1;
static int displayed_node;
static int displayed_choice;
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
static int displayed_camera_x;
static int displayed_camera_y;
static int displayed_player_left;
static int displayed_player_top;
static int displayed_player_right;
static int displayed_player_bottom;
static int displayed_world_valid;
static int cpu_cycles_per_frame = DEFAULT_CPU_CYCLES_PER_FRAME;
static int applied_music_revision = -1;

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
                         x, y, 0xFFFFFFFF);
}

static void record_frame(const struct generated_runtime_frame *frame, int x, int y)
{
    record_texture(frame->texture, frame->source_x, frame->source_y,
                   frame->width, frame->height, x, y);
}

static void record_scenery_frame(const struct generated_runtime_frame *frame,
                                 int x, int y, int alpha)
{
    int color = (int)(((unsigned int)alpha << 24) | 0x00FFFFFFu);
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
                x, y, scale, scale, 0xFFFFFFFF);
}

static int record_runtime_text(int x, int y, int width, int bottom,
                               const struct generated_runtime_text *text, int color)
{
    int cursor_x = x;
    int cursor_y = y;
    int index;
    if (text == 0)
        return y;
    for (index = 0; index < text->glyph_count; ++index) {
        int glyph_index = text->glyphs[index];
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
        if (cursor_y + DIALOG_LINE_HEIGHT > bottom)
            break;
        record_draw(glyph->texture, LAST_GLYPH_REGION - glyph_index, 0, 0, 0, 0,
                    cursor_x, cursor_y, 1.0f, 1.0f, color);
        cursor_x += glyph->advance;
    }
    return cursor_y + DIALOG_LINE_HEIGHT;
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
    int choice_count = world_dialogue_choice_count(world);
    int selected_choice = world_dialogue_choice_selected(world);
    int has_portrait = speaker != 0 && speaker->portrait.texture >= 0;
    int text_x = has_portrait ? DIALOG_X + 128 : DIALOG_X + 24;
    int text_width = DIALOG_X + DIALOG_WIDTH - 20 - text_x;
    int speech_bottom;
    int choice_y;
    int index;

    /* The original conversation UI leaves the scene visible beneath a dark,
     * warm-edged translucent window. */
    record_solid_rect(DIALOG_X - 2, DIALOG_Y - 2, DIALOG_WIDTH + 4, DIALOG_HEIGHT + 4, 0xD08A642C);
    record_solid_rect(DIALOG_X + 2, DIALOG_Y + 2, DIALOG_WIDTH - 4, DIALOG_HEIGHT - 4, 0xB0101820);

    if (has_portrait) {
        record_solid_rect(DIALOG_X + 14, DIALOG_Y + 18, 104, 104, 0xD08A642C);
        record_solid_rect(DIALOG_X + 18, DIALOG_Y + 22, 96, 96, 0xC0080B10);
        record_ui_frame(&speaker->portrait, DIALOG_X + 18, DIALOG_Y + 22, 1.5f);
    }
    if (speaker != 0)
        record_runtime_text(text_x, DIALOG_Y + 16, text_width, DIALOG_Y + 42,
                            &speaker->name_text, 0xFFFFD070);
    speech_bottom = record_runtime_text(text_x, DIALOG_Y + 44, text_width, DIALOG_Y + 151,
                                        speech, 0xFFFFFFFF);
    choice_y = speech_bottom + 10;
    if (choice_y < DIALOG_Y + 138)
        choice_y = DIALOG_Y + 138;
    for (index = 0; index < choice_count; ++index) {
        int option_y = choice_y;
        int option_bottom = record_runtime_text(DIALOG_X + 36, option_y,
                                                 DIALOG_WIDTH - 58,
                                                 DIALOG_Y + DIALOG_HEIGHT - 8,
                                                 world_dialogue_choice_display(world, index),
                                                 index == selected_choice ? 0xFFFFD070 : 0xFFE7DCA8);
        if (index == selected_choice)
            record_solid_rect(DIALOG_X + 22, option_y + 3, 5,
                              option_bottom - option_y - 6, 0xFFFFD070);
        choice_y = option_bottom + 5;
    }
}

static void camera_for_player(const struct world_state *world, int *camera_x, int *camera_y)
{
    const struct generated_runtime_actor_visual *visual = &generated_runtime_actor_visuals[
        generated_runtime_actors[world->scene->entities[world->player_entity].definition].visual];
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
    const struct generated_runtime_actor *entity = &generated_runtime_actors[definition];
    const struct generated_runtime_actor_visual *visual = &generated_runtime_actor_visuals[entity->visual];
    int facing = world_actor_facing(world, definition);
    if (world_actor_is_moving(world, definition) && visual->walking_count[facing] > 0)
        return &visual->walking[facing][world_actor_animation_frame(world, definition) %
                                        visual->walking_count[facing]];
    return &visual->standing[facing];
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
    const struct generated_runtime_actor *entity = &generated_runtime_actors[definition];
    const struct generated_runtime_actor_visual *visual = &generated_runtime_actor_visuals[entity->visual];
    const struct generated_runtime_frame *frame = entity_frame(world, definition);
    int x = world_actor_pixel_x(world, definition);
    int y = world_actor_pixel_y(world, definition);
    record_frame(frame, x - visual->base_x * world->scene->tile_size + frame->offset_x - camera_x,
                 y - visual->base_y * world->scene->tile_size + frame->offset_y - camera_y);
}

static void player_screen_bounds(const struct world_state *world, int camera_x, int camera_y,
                                 int *left, int *top, int *right, int *bottom)
{
    const struct generated_runtime_actor *entity = &generated_runtime_actors[
        world->scene->entities[world->player_entity].definition];
    const struct generated_runtime_actor_visual *visual = &generated_runtime_actor_visuals[entity->visual];
    const struct generated_runtime_frame *frame = entity_frame(
        world, world->scene->entities[world->player_entity].definition);
    int x = world->player_x * world->scene->tile_size;
    int y = world->player_y * world->scene->tile_size;
    int width = frame->width;
    int height = frame->height;
    if (world->moving) {
        x += world->move_x * world->move_progress;
        y += world->move_y * world->move_progress;
    }
    x += -visual->base_x * world->scene->tile_size + frame->offset_x - camera_x;
    y += -visual->base_y * world->scene->tile_size + frame->offset_y - camera_y;
    if (x < 0) {
        width += x;
        x = 0;
    }
    if (y < 0) {
        height += y;
        y = 0;
    }
    if (x + width > GAME_WIDTH)
        width = GAME_WIDTH - x;
    if (y + height > GAME_HEIGHT)
        height = GAME_HEIGHT - y;
    if (width <= 0 || height <= 0) {
        *left = *right = VIEW_X;
        *top = *bottom = 0;
        return;
    }
    *left = view_x(x);
    *top = view_y(y);
    *right = *left + (width * 3) / 2;
    *bottom = *top + (height * 3) / 2;
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
            if (generated_runtime_actors[index].visual < 0 ||
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

static void record_ambient_speech(const struct world_state *world, int camera_x, int camera_y)
{
    int entity_index;
    for (entity_index = 0; entity_index < generated_runtime_actor_count; ++entity_index) {
        const struct generated_runtime_text *speech =
            world_actor_speech(world, entity_index);
        const struct generated_runtime_actor *entity = &generated_runtime_actors[entity_index];
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
        if (speech == 0 || entity->visual < 0 ||
            !world_actor_is_present(world, entity_index))
            continue;
        visual = &generated_runtime_actor_visuals[entity->visual];
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

        if (ambient_overlay_count < GENERATED_RUNTIME_ENTITY_STORAGE_COUNT) {
            int overlay = ambient_overlay_count++;
            ambient_overlay_left[overlay] = bubble_x - 1;
            ambient_overlay_top[overlay] = bubble_y - SPEECH_SPRITE_GAP - 1;
            ambient_overlay_right[overlay] = bubble_x + bubble_width + 1;
            ambient_overlay_bottom[overlay] =
                bubble_y + bubble_height + SPEECH_SPRITE_GAP + 1;
        }

        record_solid_rect(bubble_x - 1, bubble_y - 1,
                          bubble_width + 2, bubble_height + 2, 0x908A642C);
        record_solid_rect(bubble_x, bubble_y,
                          bubble_width, bubble_height, 0x78101820);
        record_runtime_text(bubble_x + SPEECH_PADDING_X,
                            bubble_y + SPEECH_PADDING_Y, text_width,
                            bubble_y + bubble_height - SPEECH_PADDING_Y,
                            speech, 0xFFFFFFFF);
        if (bubble_y < sprite_top)
            record_solid_rect(tail_x - 2, bubble_y + bubble_height,
                              4, SPEECH_SPRITE_GAP, 0x908A642C);
        else
            record_solid_rect(tail_x - 2, bubble_y - SPEECH_SPRITE_GAP,
                              4, SPEECH_SPRITE_GAP, 0x908A642C);
    }
}

void platform_poll_actions(struct world_actions *actions)
{
    static int interaction_was_held;
    static int previous_choice_direction;
    int interaction_held;
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

void platform_update_audio(const struct world_state *world)
{
    const struct generated_runtime_music *track;
    if (world->music_track < 0 || world->music_track >= generated_runtime_music_count)
        return;
    if (applied_music_revision == world->music_revision &&
        get_channel_state(MUSIC_CHANNEL) == channel_playing)
        return;
    applied_music_revision = world->music_revision;
    track = &generated_runtime_music[world->music_track];
    stop_channel(MUSIC_CHANNEL);
    select_sound(track->sound);
    set_sound_loop(track->loop);
    assign_channel_sound(MUSIC_CHANNEL, track->sound);
    set_channel_position(0);
    set_channel_volume(track->volume);
    set_channel_speed(track->speed);
    set_channel_loop(track->loop);
    play_channel(MUSIC_CHANNEL);
}

static void prepare_world_frame(const struct world_state *world)
{
    int camera_x;
    int camera_y;
    int transition_alpha;
    draw_command_count = 0;
    ambient_overlay_count = 0;
    prepared_dirty_valid = 0;
    prepared_replay_commands = 0;
    camera_for_player(world, &camera_x, &camera_y);
    prepared_camera_x = camera_x;
    prepared_camera_y = camera_y;
    player_screen_bounds(world, camera_x, camera_y,
                         &prepared_player_left, &prepared_player_top,
                         &prepared_player_right, &prepared_player_bottom);
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

static int command_intersects_rectangle(int index, int left, int top, int right, int bottom)
{
    float scaled_width = draw_command_region_width[index] * draw_command_scale_x[index];
    float scaled_height = draw_command_region_height[index] * draw_command_scale_y[index];
    int width = (int)scaled_width;
    int height = (int)scaled_height;
    int x = draw_command_x[index];
    int y = draw_command_y[index];
    if ((float)width < scaled_width) ++width;
    if ((float)height < scaled_height) ++height;
    return width > 0 && height > 0 && x < right && y < bottom &&
           x + width > left && y + height > top;
}

static void expand_dirty_for_translucent_commands(void)
{
    int changed;
    do {
        int index;
        changed = 0;
        for (index = 0; index < draw_command_count; ++index) {
            unsigned int alpha = (unsigned int)draw_command_color[index] >> 24;
            float scaled_width;
            float scaled_height;
            int width;
            int height;
            int left;
            int top;
            int right;
            int bottom;
            if (alpha >= 255 || draw_command_region_width[index] <= 0 ||
                draw_command_region_height[index] <= 0 ||
                !command_intersects_rectangle(index,
                                              prepared_dirty_left, prepared_dirty_top,
                                              prepared_dirty_right, prepared_dirty_bottom))
                continue;

            /* Partial texture clipping rounds outward so opaque images never
             * leave a seam. Doing that to an alpha-blended command can touch
             * an uncleared pixel outside the dirty rectangle and blend the
             * same wall over it once more on every movement frame. Include
             * the complete translucent draw (plus its raster halo) instead,
             * so it is always replayed onto freshly restored pixels. Repeat
             * because expanding to one overlapping object may reach another. */
            scaled_width = draw_command_region_width[index] * draw_command_scale_x[index];
            scaled_height = draw_command_region_height[index] * draw_command_scale_y[index];
            width = (int)scaled_width;
            height = (int)scaled_height;
            if ((float)width < scaled_width) ++width;
            if ((float)height < scaled_height) ++height;
            left = draw_command_x[index] - DIRTY_PADDING;
            top = draw_command_y[index] - DIRTY_PADDING;
            right = draw_command_x[index] + width + DIRTY_PADDING;
            bottom = draw_command_y[index] + height + DIRTY_PADDING;
            if (left < prepared_dirty_left) {
                prepared_dirty_left = left;
                changed = 1;
            }
            if (top < prepared_dirty_top) {
                prepared_dirty_top = top;
                changed = 1;
            }
            if (right > prepared_dirty_right) {
                prepared_dirty_right = right;
                changed = 1;
            }
            if (bottom > prepared_dirty_bottom) {
                prepared_dirty_bottom = bottom;
                changed = 1;
            }
        }
    } while (changed);
}

static int ambient_overlay_intersects_rectangle(int left, int top, int right, int bottom)
{
    int index;
    for (index = 0; index < ambient_overlay_count; ++index)
        if (ambient_overlay_left[index] < right && ambient_overlay_top[index] < bottom &&
            ambient_overlay_right[index] > left && ambient_overlay_bottom[index] > top)
            return 1;
    return 0;
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
    if (!prepared_dirty_valid)
        clear_screen(0xFF101820);
    /* Partial replay paints the opaque background first, directly over the
     * retained frame. It must not clear a rectangle: rounding a 1.5x source
     * crop to that rectangle can leave an uncovered scanline at its edge. */
    set_multiply_color(0xFFFFFFFF);
    color = 0xFFFFFFFF;
    for (index = 0; index < draw_command_count; ++index) {
        int command_region = draw_command_region[index];
        int command_x = draw_command_region_x[index];
        int command_y = draw_command_region_y[index];
        int command_width = draw_command_region_width[index];
        int command_height = draw_command_region_height[index];
        int command_draw_x = draw_command_x[index];
        int command_draw_y = draw_command_y[index];

        if (prepared_dirty_valid) {
            int output_right;
            int output_bottom;
            int source_left = 0;
            int source_top = 0;
            int source_right = command_width;
            int source_bottom = command_height;
            if (!command_intersects_rectangle(index,
                                              prepared_dirty_left, prepared_dirty_top,
                                              prepared_dirty_right, prepared_dirty_bottom))
                continue;

            output_right = command_draw_x + (command_width * 3 + 1) / 2;
            output_bottom = command_draw_y + (command_height * 3 + 1) / 2;
            /* Round the source crop inward. The former outward rounding drew
             * one scaled source row/column beyond the dirty rectangle. During
             * vertical movement those writes accumulated as horizontal strips
             * on static furniture and actors that did not need updating. The
             * six-pixel dirty halo keeps all genuinely changed pixels well
             * inside this conservative crop. */
            if (command_draw_x < prepared_dirty_left)
                source_left = ((prepared_dirty_left - command_draw_x) * 2 + 2) / 3;
            if (command_draw_y < prepared_dirty_top)
                source_top = ((prepared_dirty_top - command_draw_y) * 2 + 2) / 3;
            if (output_right > prepared_dirty_right)
                source_right = ((prepared_dirty_right - command_draw_x) * 2) / 3;
            if (output_bottom > prepared_dirty_bottom)
                source_bottom = ((prepared_dirty_bottom - command_draw_y) * 2) / 3;
            if (source_left < 0) source_left = 0;
            if (source_top < 0) source_top = 0;
            if (source_right > command_width) source_right = command_width;
            if (source_bottom > command_height) source_bottom = command_height;
            if (source_right <= source_left || source_bottom <= source_top)
                continue;

            command_x += source_left;
            command_y += source_top;
            command_width = source_right - source_left;
            command_height = source_bottom - source_top;
            command_draw_x += (source_left * 3) / 2;
            command_draw_y += (source_top * 3) / 2;
        }
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
    set_multiply_color(0xFFFFFFFF);
    set_drawing_scale(1.0f, 1.0f);
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
    displayed_camera_x = prepared_camera_x;
    displayed_camera_y = prepared_camera_y;
    displayed_player_left = prepared_player_left;
    displayed_player_top = prepared_player_top;
    displayed_player_right = prepared_player_right;
    displayed_player_bottom = prepared_player_bottom;
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

static void configure_dirty_replay(const struct world_state *world)
{
    int index;
    if (!displayed_world_valid || displayed_scene != world->scene ||
        displayed_visual_revision != world->visual_revision ||
        displayed_camera_x != prepared_camera_x ||
        displayed_camera_y != prepared_camera_y)
        return;

    prepared_dirty_left = displayed_player_left;
    if (prepared_player_left < prepared_dirty_left)
        prepared_dirty_left = prepared_player_left;
    prepared_dirty_top = displayed_player_top;
    if (prepared_player_top < prepared_dirty_top)
        prepared_dirty_top = prepared_player_top;
    prepared_dirty_right = displayed_player_right;
    if (prepared_player_right > prepared_dirty_right)
        prepared_dirty_right = prepared_player_right;
    prepared_dirty_bottom = displayed_player_bottom;
    if (prepared_player_bottom > prepared_dirty_bottom)
        prepared_dirty_bottom = prepared_player_bottom;

    prepared_dirty_left -= DIRTY_PADDING;
    prepared_dirty_top -= DIRTY_PADDING;
    prepared_dirty_right += DIRTY_PADDING;
    prepared_dirty_bottom += DIRTY_PADDING;

    expand_dirty_for_translucent_commands();

    if (prepared_dirty_left < VIEW_X) prepared_dirty_left = VIEW_X;
    if (prepared_dirty_top < 0) prepared_dirty_top = 0;
    if (prepared_dirty_right > VIEW_X + VIEW_WIDTH)
        prepared_dirty_right = VIEW_X + VIEW_WIDTH;
    if (prepared_dirty_bottom > DISPLAY_HEIGHT)
        prepared_dirty_bottom = DISPLAY_HEIGHT;
    if (prepared_dirty_right <= prepared_dirty_left ||
        prepared_dirty_bottom <= prepared_dirty_top)
        return;

    /* Ambient panels are screen-space command groups: translucent rectangles,
     * a tail, and several independently recorded glyphs. The world-texture
     * clipper cannot safely replay only a fragment of that group; its backdrop
     * would erase the panel while non-intersecting glyphs remained omitted.
     * Use the coherent full-frame path only when a player's dirty rectangle
     * actually touches a bubble. Ordinary movement keeps partial replay. */
    if (ambient_overlay_intersects_rectangle(
            prepared_dirty_left, prepared_dirty_top,
            prepared_dirty_right, prepared_dirty_bottom))
        return;

    prepared_dirty_valid = 1;
    prepared_replay_commands = 0;
    for (index = 0; index < draw_command_count; ++index) {
        if (command_intersects_rectangle(index,
                                         prepared_dirty_left, prepared_dirty_top,
                                         prepared_dirty_right, prepared_dirty_bottom))
            ++prepared_replay_commands;
    }
}

static int enough_time_to_flush(void)
{
    int replay_commands = prepared_dirty_valid ? prepared_replay_commands : draw_command_count;
    int estimated_cycles = DRAW_FIXED_CYCLES +
                           replay_commands * DRAW_CYCLES_PER_COMMAND +
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
        displayed_world_valid = 0;
    } else {
        displayed_dialogue = -1;
        remember_displayed_world();
    }
}

void platform_render_world(const struct world_state *world)
{
    int preparation_frame;

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
                                       displayed_dialogue, displayed_node, displayed_choice)) {
            return;
        }

        /* Dialogue is relatively static and glyph-heavy. Always give it the
         * existing compute/draw split rather than gambling on a partial panel. */
        prepare_world_frame(world);
        prepared_dialogue = dialogue;
        prepared_node = node;
        prepared_choice = choice;
        prepared_is_dialogue = 1;
        prepared_frame_valid = 1;
        return;
    }

    displayed_dialogue = -1;
    if (displayed_world_matches(world))
        return;

    preparation_frame = get_frame_counter();
    prepare_world_frame(world);
    remember_prepared_world(world);
    configure_dirty_replay(world);
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

void platform_present(void)
{
    end_frame();
}
