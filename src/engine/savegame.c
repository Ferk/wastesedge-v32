#include "engine/savegame.h"

#include "content_ids.h"

struct save_writer {
    int *words;
    int capacity;
    int count;
    int failed;
};

struct save_reader {
    const int *words;
    int count;
    int position;
    int failed;
};

static struct world_state restored_world;

static void write_word(struct save_writer *writer, int value)
{
    if (writer->count >= writer->capacity) {
        writer->failed = 1;
        return;
    }
    writer->words[writer->count++] = value;
}

static int read_word(struct save_reader *reader)
{
    if (reader->position >= reader->count) {
        reader->failed = 1;
        return 0;
    }
    return reader->words[reader->position++];
}

static int hexadecimal_digit(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return 0;
}

unsigned int savegame_content_fingerprint(void)
{
    unsigned int value = 0;
    int index;
    /* The generated signature is SHA-256 text. Its first 32 bits are enough
     * to prevent numeric content IDs from being interpreted against a
     * different compiled data set. */
    for (index = 0; index < 8 && generated_content_signature[index] != 0; ++index)
        value = (value << 4) | (unsigned int)hexadecimal_digit(
            generated_content_signature[index]);
    return value;
}

unsigned int savegame_checksum(const int *words, int word_count)
{
    unsigned int hash = 2166136261u;
    int index;
    for (index = 0; index < word_count; ++index) {
        hash ^= (unsigned int)words[index];
        hash *= 16777619u;
    }
    return hash;
}

int savegame_encode(const struct world_state *world, int *words, int capacity)
{
    struct save_writer writer;
    int index;
    writer.words = words;
    writer.capacity = capacity;
    writer.count = 0;
    writer.failed = 0;

    write_word(&writer, SAVEGAME_FORMAT_VERSION);
    write_word(&writer, (int)savegame_content_fingerprint());
    write_word(&writer, GENERATED_RUNTIME_ENTITY_COUNT);
    write_word(&writer, GENERATED_RUNTIME_FLAG_COUNT);
    write_word(&writer, GENERATED_RUNTIME_STATEFUL_SCENERY_COUNT);
    write_word(&writer, GENERATED_RUNTIME_DIALOGUE_USED_COUNT);

    write_word(&writer, world->scene_index);
    write_word(&writer, world->player_entity);
    write_word(&writer, world->player_x);
    write_word(&writer, world->player_y);
    write_word(&writer, world->player_facing);
    write_word(&writer, world->moving);
    write_word(&writer, world->move_x);
    write_word(&writer, world->move_y);
    write_word(&writer, world->move_progress);
    write_word(&writer, world->animation_frame);
    write_word(&writer, world->dialogue);
    write_word(&writer, world->dialogue_node);
    write_word(&writer, world->dialogue_choice);
    write_word(&writer, world->dialogue_actor);

    for (index = 0; index < GENERATED_RUNTIME_DIALOGUE_USED_COUNT; ++index)
        write_word(&writer, world->dialogue_used[index]);

    for (index = 0; index < GENERATED_RUNTIME_ENTITY_COUNT; ++index) {
        const struct world_actor_state *actor = &world->actors[index];
        write_word(&writer, actor->scene);
        write_word(&writer, actor->x);
        write_word(&writer, actor->y);
        write_word(&writer, actor->facing);
        write_word(&writer, actor->moving);
        write_word(&writer, actor->move_x);
        write_word(&writer, actor->move_y);
        write_word(&writer, actor->move_progress);
        write_word(&writer, actor->animation_frame);
        write_word(&writer, actor->visual);
        write_word(&writer, actor->visual_tick);
        write_word(&writer, actor->schedule);
        write_word(&writer, actor->schedule_step);
        write_word(&writer, actor->schedule_wait);
        write_word(&writer, actor->speech_line);
        write_word(&writer, actor->speech_wait);
        write_word(&writer, actor->speech_frames);
        write_word(&writer, actor->dialogue);
    }

    for (index = 0; index < GENERATED_RUNTIME_FLAG_COUNT; ++index)
        write_word(&writer, world->flags[index]);
    for (index = 0; index < GENERATED_RUNTIME_STATEFUL_SCENERY_COUNT; ++index)
        write_word(&writer, world->object_states[index]);

    write_word(&writer, world->active_script);
    write_word(&writer, world->script_action);
    write_word(&writer, world->script_wait);
    write_word(&writer, world->collision_override_count);
    for (index = 0; index < world->collision_override_count; ++index) {
        const struct world_collision_override *override =
            &world->collision_overrides[index];
        write_word(&writer, override->scene);
        write_word(&writer, override->x);
        write_word(&writer, override->y);
        write_word(&writer, override->walkability);
    }
    write_word(&writer, world->trigger_latch);
    write_word(&writer, world->control_enabled);
    write_word(&writer, world->music_track);
    write_word(&writer, world->transition_phase);
    write_word(&writer, world->transition_frame);
    write_word(&writer, world->transition_scene);
    write_word(&writer, world->transition_spawn);
    write_word(&writer, world->schedule_random);
    write_word(&writer, world->speech_random);
    write_word(&writer, world->game_time);
    write_word(&writer, world->game_time_subticks);
    for (index = 0; index < GENERATED_RUNTIME_ENTITY_COUNT; ++index) {
        const struct world_actor_checkpoint *checkpoint =
            &world->actor_checkpoints[index];
        write_word(&writer, checkpoint->valid);
        write_word(&writer, checkpoint->scene);
        write_word(&writer, checkpoint->x);
        write_word(&writer, checkpoint->y);
        write_word(&writer, checkpoint->facing);
        write_word(&writer, checkpoint->animation_frame);
        write_word(&writer, checkpoint->visual);
        write_word(&writer, checkpoint->visual_tick);
        write_word(&writer, checkpoint->schedule);
        write_word(&writer, checkpoint->schedule_step);
        write_word(&writer, checkpoint->schedule_wait);
        write_word(&writer, checkpoint->dialogue);
    }
    write_word(&writer, world->dialogue_auto_frame);
    write_word(&writer, world->player_hidden);
    write_word(&writer, world->transition_player_hidden);

    return writer.failed ? 0 : writer.count;
}

static int valid_facing(int facing)
{
    return facing >= WORLD_NORTH && facing <= WORLD_EAST;
}

static int validate_restored_world(const struct world_state *world)
{
    int index;
    if (world->scene_index < 0 || world->scene_index >= generated_runtime_scene_count)
        return 0;
    if (world->player_entity < 0 ||
        world->player_entity >= generated_runtime_scenes[world->scene_index].entity_count ||
        !generated_runtime_scenes[world->scene_index]
             .entities[world->player_entity].player_controlled ||
        !valid_facing(world->player_facing) ||
        (world->player_hidden != 0 && world->player_hidden != 1) ||
        (world->transition_player_hidden != 0 &&
         world->transition_player_hidden != 1))
        return 0;
    if (world->dialogue < -1 || world->dialogue >= generated_runtime_dialogue_count)
        return 0;
    if (world->dialogue >= 0 &&
        (world->dialogue_node < 0 ||
         world->dialogue_node >= generated_runtime_dialogues[world->dialogue].node_count))
        return 0;
    if (world->active_script < -1 || world->active_script >= generated_runtime_script_count)
        return 0;
    if (world->active_script >= 0 &&
        (world->script_action < 0 ||
         world->script_action > generated_runtime_scripts[world->active_script].action_count))
        return 0;
    if (world->music_track < -1 || world->music_track >= generated_runtime_music_count)
        return 0;
    if (world->transition_phase < WORLD_TRANSITION_NONE ||
        world->transition_phase > WORLD_TRANSITION_FADE_IN)
        return 0;
    if (world->collision_override_count < 0 ||
        world->collision_override_count > WORLD_COLLISION_OVERRIDE_CAPACITY)
        return 0;
    for (index = 0; index < GENERATED_RUNTIME_ENTITY_COUNT; ++index) {
        const struct world_actor_state *actor = &world->actors[index];
        if (actor->scene < -1 || actor->scene >= generated_runtime_scene_count ||
            !valid_facing(actor->facing) ||
            actor->visual < -1 || actor->visual >= generated_runtime_actor_visual_count ||
            actor->schedule < -1 || actor->schedule >= generated_runtime_schedule_count ||
            actor->dialogue < -1 || actor->dialogue >= generated_runtime_dialogue_count)
            return 0;
        if (actor->scene >= 0 &&
            (actor->x < 0 || actor->y < 0 ||
             actor->x >= generated_runtime_scenes[actor->scene].width ||
             actor->y >= generated_runtime_scenes[actor->scene].height))
            return 0;
    }
    for (index = 0; index < GENERATED_RUNTIME_ENTITY_COUNT; ++index) {
        const struct world_actor_checkpoint *checkpoint =
            &world->actor_checkpoints[index];
        if (checkpoint->valid != 0 && checkpoint->valid != 1)
            return 0;
        if (!checkpoint->valid)
            continue;
        if (checkpoint->scene < -1 ||
            checkpoint->scene >= generated_runtime_scene_count ||
            !valid_facing(checkpoint->facing) ||
            checkpoint->visual < -1 ||
            checkpoint->visual >= generated_runtime_actor_visual_count ||
            checkpoint->schedule < -1 ||
            checkpoint->schedule >= generated_runtime_schedule_count ||
            checkpoint->dialogue < -1 ||
            checkpoint->dialogue >= generated_runtime_dialogue_count)
            return 0;
        if (checkpoint->scene >= 0 &&
            (checkpoint->x < 0 || checkpoint->y < 0 ||
             checkpoint->x >= generated_runtime_scenes[checkpoint->scene].width ||
             checkpoint->y >= generated_runtime_scenes[checkpoint->scene].height))
            return 0;
    }
    for (index = 0; index < world->collision_override_count; ++index) {
        const struct world_collision_override *override =
            &world->collision_overrides[index];
        if (override->scene < 0 || override->scene >= generated_runtime_scene_count ||
            override->x < 0 || override->y < 0 ||
            override->x >= generated_runtime_scenes[override->scene].width ||
            override->y >= generated_runtime_scenes[override->scene].height ||
            override->walkability < 0 || override->walkability > 15)
            return 0;
    }
    return 1;
}

static void copy_world(struct world_state *destination,
                       const struct world_state *source)
{
    unsigned char *output = (unsigned char *)destination;
    const unsigned char *input = (const unsigned char *)source;
    unsigned int index;
    for (index = 0; index < (unsigned int)sizeof(*destination); ++index)
        output[index] = input[index];
}

int savegame_decode(struct world_state *world, const int *words, int word_count)
{
    struct save_reader reader;
    int version;
    int index;
    if (word_count <= 0 || word_count > SAVEGAME_MAX_WORDS)
        return 0;
    reader.words = words;
    reader.count = word_count;
    reader.position = 0;
    reader.failed = 0;

    version = read_word(&reader);
    if (version < SAVEGAME_MINIMUM_VERSION || version > SAVEGAME_FORMAT_VERSION ||
        (unsigned int)read_word(&reader) != savegame_content_fingerprint() ||
        read_word(&reader) != GENERATED_RUNTIME_ENTITY_COUNT ||
        read_word(&reader) != GENERATED_RUNTIME_FLAG_COUNT ||
        read_word(&reader) != GENERATED_RUNTIME_STATEFUL_SCENERY_COUNT ||
        read_word(&reader) != GENERATED_RUNTIME_DIALOGUE_USED_COUNT)
        return 0;

    world_initialize(&restored_world);
    restored_world.scene_index = read_word(&reader);
    restored_world.player_entity = read_word(&reader);
    restored_world.player_x = read_word(&reader);
    restored_world.player_y = read_word(&reader);
    restored_world.player_facing = read_word(&reader);
    restored_world.moving = read_word(&reader);
    restored_world.move_x = read_word(&reader);
    restored_world.move_y = read_word(&reader);
    restored_world.move_progress = read_word(&reader);
    restored_world.animation_frame = read_word(&reader);
    restored_world.dialogue = read_word(&reader);
    restored_world.dialogue_node = read_word(&reader);
    restored_world.dialogue_choice = read_word(&reader);
    restored_world.dialogue_actor = read_word(&reader);

    for (index = 0; index < GENERATED_RUNTIME_DIALOGUE_USED_COUNT; ++index)
        restored_world.dialogue_used[index] = (unsigned char)read_word(&reader);

    for (index = 0; index < GENERATED_RUNTIME_ENTITY_COUNT; ++index) {
        struct world_actor_state *actor = &restored_world.actors[index];
        actor->scene = read_word(&reader);
        actor->x = read_word(&reader);
        actor->y = read_word(&reader);
        actor->facing = read_word(&reader);
        actor->moving = read_word(&reader);
        actor->move_x = read_word(&reader);
        actor->move_y = read_word(&reader);
        actor->move_progress = read_word(&reader);
        actor->animation_frame = read_word(&reader);
        actor->visual = read_word(&reader);
        actor->visual_tick = read_word(&reader);
        actor->schedule = read_word(&reader);
        actor->schedule_step = read_word(&reader);
        actor->schedule_wait = read_word(&reader);
        actor->speech_line = read_word(&reader);
        actor->speech_wait = read_word(&reader);
        actor->speech_frames = read_word(&reader);
        actor->dialogue = read_word(&reader);
        /* Paths live in a shared transient work buffer and scripted speech
         * stores a ROM pointer. Both are reconstructed safely after loading. */
        actor->path_schedule_step = -1;
        actor->path_scene = -1;
        actor->path_length = 0;
        actor->path_index = 0;
        actor->speech_override = 0;
        if (actor->speech_line < 0)
            actor->speech_frames = 0;
    }

    for (index = 0; index < GENERATED_RUNTIME_FLAG_COUNT; ++index)
        restored_world.flags[index] = read_word(&reader);
    for (index = 0; index < GENERATED_RUNTIME_STATEFUL_SCENERY_COUNT; ++index)
        restored_world.object_states[index] = read_word(&reader);

    restored_world.active_script = read_word(&reader);
    restored_world.script_action = read_word(&reader);
    restored_world.script_wait = read_word(&reader);
    restored_world.collision_override_count = read_word(&reader);
    if (restored_world.collision_override_count < 0 ||
        restored_world.collision_override_count > WORLD_COLLISION_OVERRIDE_CAPACITY)
        return 0;
    for (index = 0; index < restored_world.collision_override_count; ++index) {
        struct world_collision_override *override =
            &restored_world.collision_overrides[index];
        override->scene = read_word(&reader);
        override->x = read_word(&reader);
        override->y = read_word(&reader);
        override->walkability = read_word(&reader);
    }
    restored_world.trigger_latch = read_word(&reader);
    restored_world.control_enabled = read_word(&reader);
    restored_world.music_track = read_word(&reader);
    restored_world.transition_phase = read_word(&reader);
    restored_world.transition_frame = read_word(&reader);
    restored_world.transition_scene = read_word(&reader);
    restored_world.transition_spawn = read_word(&reader);
    restored_world.schedule_random = read_word(&reader);
    restored_world.speech_random = read_word(&reader);
    if (version >= 2) {
        restored_world.game_time = read_word(&reader);
        restored_world.game_time_subticks = read_word(&reader);
    }
    if (version >= 3) {
        for (index = 0; index < GENERATED_RUNTIME_ENTITY_COUNT; ++index) {
            struct world_actor_checkpoint *checkpoint =
                &restored_world.actor_checkpoints[index];
            checkpoint->valid = read_word(&reader);
            checkpoint->scene = read_word(&reader);
            checkpoint->x = read_word(&reader);
            checkpoint->y = read_word(&reader);
            checkpoint->facing = read_word(&reader);
            checkpoint->animation_frame = read_word(&reader);
            checkpoint->visual = read_word(&reader);
            checkpoint->visual_tick = read_word(&reader);
            checkpoint->schedule = read_word(&reader);
            checkpoint->schedule_step = read_word(&reader);
            checkpoint->schedule_wait = read_word(&reader);
            checkpoint->dialogue = read_word(&reader);
        }
    }
    if (version >= 4)
        restored_world.dialogue_auto_frame = read_word(&reader);
    if (version >= 5) {
        restored_world.player_hidden = read_word(&reader);
        restored_world.transition_player_hidden = read_word(&reader);
    }

    restored_world.scene = (restored_world.scene_index >= 0 &&
                            restored_world.scene_index < generated_runtime_scene_count) ?
                           &generated_runtime_scenes[restored_world.scene_index] : 0;
    restored_world.script_path_action = -1;
    restored_world.script_path_length = 0;
    restored_world.script_path_index = 0;
    restored_world.visual_revision = 1;
    restored_world.music_revision = 1;
    if (restored_world.game_time < 0 || restored_world.game_time_subticks < 0 ||
        restored_world.dialogue_auto_frame < 0 ||
        restored_world.game_time_subticks >= WORLD_GAME_TIME_SUBTICKS_PER_UNIT ||
        reader.failed || reader.position != reader.count ||
        !validate_restored_world(&restored_world))
        return 0;
    copy_world(world, &restored_world);
    return 1;
}
