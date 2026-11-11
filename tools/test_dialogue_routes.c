/* Host regression tests for early dialogue branches and achievement routes. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "content_ids.h"
#include "engine/world.h"
#include "engine/achievements.h"

/* Execute a dialogue script until its opening line is displayed. */
static void talk(struct world_state *world, int script)
{
    struct world_actions idle = {0};
    world->dialogue = -1;
    world->active_script = script;
    world->script_action = 0;
    world->script_wait = 0;
    world_update(world, &idle);
    world_update(world, &idle);
    assert(world->dialogue >= 0);
}

/* Match the legacy player-name token against the default authoring name. */
static int same_text(const char *actual, const char *expected)
{
    const char *token = strstr(actual, "$name");
    if (token) {
        int prefix = (int)(token - actual);
        return !strncmp(actual, expected, prefix) &&
               !strncmp(expected + prefix, "Banec", 5) &&
               !strcmp(token + 5, expected + prefix + 5);
    }
    return !strcmp(actual, expected);
}

/* Advance a line or select a visible response by its authored text. */
static void advance(struct world_state *world, const char *choice)
{
    struct world_actions interact = {0, 0, 1, 0, 0, 0};
    int i;
    if (choice) {
        for (i = 0; i < world_dialogue_choice_count(world); ++i)
            if (same_text(world_dialogue_choice_at(world, i), choice)) break;
        if (i == world_dialogue_choice_count(world)) {
            fprintf(stderr, "Missing choice: %s\nAt: %s\n", choice, world_dialogue_text(world));
            assert(0);
        }
        world->dialogue_choice = i;
    }
    world_update(world, &interact);
}

/* Find the first node with the supplied text in a compiled dialogue. */
static const struct generated_runtime_dialogue_node *node(int dialogue, const char *text)
{
    const struct generated_runtime_dialogue *d = &generated_runtime_dialogues[dialogue];
    const struct generated_runtime_dialogue_node *found = 0;
    int i;
    for (i = 0; i < d->node_count; ++i)
        if (same_text(d->nodes[i].text, text) &&
            (!found || d->nodes[i].choice_count > found->choice_count)) found = &d->nodes[i];
    assert(found != 0);
    return found;
}

/* Check the first-meeting route, then ensure a repeat is not spuriously accusatory. */
static void test_erek(void)
{
    struct world_state w;
    const struct generated_runtime_dialogue *d = &generated_runtime_dialogues[GENERATED_DIALOGUE_EREK_START];
    const struct generated_runtime_dialogue_node *gem_node = node(GENERATED_DIALOGUE_EREK_START,
        "I am sorry, but I haven't seen them up close, so I have to rely on my Master's word. I would know the difference if I had them in my hand, but that's unlikely.");
    achievements_initialize();
    world_initialize(&w);
    w.flags[gem_node->choices[0].conditions[0].flag] = 1;
    talk(&w, GENERATED_SCRIPT_TALK_TO_EREK);
    advance(&w, 0);
    assert(strstr(world_dialogue_text(&w), "what was the name") != 0);
    advance(&w, 0);
    advance(&w, "I am Banec, Lady Silverhair's man. I have some questions.");
    advance(&w, "Well, what do you believe, Erek?");
    advance(&w, "I know of nothing that could possibly support such claims. I really would know what has happened!");
    while (strcmp(world_dialogue_text(&w), "And with that, she left us alone.")) advance(&w, 0);
    advance(&w, "Wait a moment! What exactly is a Catseye?");
    advance(&w, "I see. And Fingolson's stones were truly of such nature?");
    advance(&w, "Is that your opinion, or that of Master Fingolson?");
    advance(&w, 0);
    advance(&w, 0);
    assert(w.achievement_progress[7] == generated_runtime_achievements[7].initial);
    advance(&w, "Unlikely perhaps, but not impossible. What do you make of this one, Erek?");
    assert(achievements_unlocked() & (1u << 7));
    /* Simulate Orloth hearing the player after Erek already knows. */
    w.flags[d->starts[1].conditions[1].flag] = 1;
    talk(&w, GENERATED_SCRIPT_TALK_TO_EREK);
    assert(!strcmp(world_dialogue_text(&w), "Erek gives you a welcoming nod as you approach."));
    /* If Erek does not know, the same exposure forces the confrontation. */
    w.flags[d->starts[1].conditions[0].flag] = 0;
    talk(&w, GENERATED_SCRIPT_TALK_TO_EREK);
    assert(strstr(world_dialogue_text(&w), "raises his finger accusingly") != 0);
    advance(&w, 0);
    advance(&w, "I did not know that this was of any importance to you, Erek.");
    assert(w.flags[d->starts[1].conditions[0].flag] == 1);
    /* Alek's disclosure independently causes the same confrontation. */
    w.flags[d->starts[1].conditions[0].flag] = 0;
    w.flags[d->starts[1].conditions[1].flag] = 0;
    w.flags[d->starts[2].conditions[1].flag] = 1;
    talk(&w, GENERATED_SCRIPT_TALK_TO_EREK);
    assert(strstr(world_dialogue_text(&w), "raises his finger accusingly") != 0);
    /* Knowing his name changes only the first introduction. */
    world_initialize(&w);
    w.flags[d->starts[4].flag] = 1;
    talk(&w, GENERATED_SCRIPT_TALK_TO_EREK);
    advance(&w, 0);
    assert(strstr(world_dialogue_text(&w), "You are Erek Stonebreaker") != 0);
}

/* Check polite Lucia and Alek's introduction continue to their original branches. */
static void test_openings(void)
{
    struct world_state w;
    achievements_initialize();
    world_initialize(&w);
    talk(&w, GENERATED_SCRIPT_TALK_TO_LUCIA);
    advance(&w, 0);
    advance(&w, "Sorry madam, I did not mean to trespass, but I need to talk to you.");
    assert(strstr(world_dialogue_text(&w), "without interruptions") != 0);
    advance(&w, "My name is Banec. I am investigating the theft.");
    assert(strstr(world_dialogue_text(&w), "You should have said so") != 0);
    assert(w.achievement_progress[8] & (1u << 18));
    world_initialize(&w);
    talk(&w, GENERATED_SCRIPT_TALK_TO_ALEK);
    advance(&w, 0);
    advance(&w, "That's Banec, you ass! I'm working for Lady Silverhair.");
    advance(&w, 0);
    assert(strstr(world_dialogue_text(&w), "Not before I get some answers") != 0);
    advance(&w, 0);
    assert(strstr(world_dialogue_text(&w), "I don't like the implication") != 0);
}

/* Place a host test at a compiled dialogue node, preserving world flags. */
static void visit(struct world_state *world, int dialogue, const char *text)
{
    const struct generated_runtime_dialogue *d = &generated_runtime_dialogues[dialogue];
    world->dialogue = dialogue;
    world->dialogue_node = (int)(node(dialogue, text) - d->nodes);
    world->dialogue_choice = 0;
    memset(world->dialogue_used, 0, sizeof(world->dialogue_used));
}

/* Verify the restored branches with both outcomes of their important flags. */
static void test_restored_branches(void)
{
    struct world_state w;
    const struct generated_runtime_dialogue_node *menu, *taunt;
    const char *noise = "I know that Lady Frostbloom has heard a noise outside the inn during that night.";
    const char *lucia_menu = "Now don't keep me off my work. Ask what you want to know and then out with you!";
    const char *guard = "Don't tell me you thought Fingolson would enrol you as his guard?";
    int noise_flag, singing_flag, clerk_flag, door_flag;
    achievements_initialize();
    world_initialize(&w);
    talk(&w, GENERATED_SCRIPT_TALK_TO_EREK);
    advance(&w, 0);
    advance(&w, 0);
    advance(&w, "I am Banec.");
    advance(&w, "Actually, that isn't the case. I would like to speak to you about Lady Silverhair.");
    assert(strstr(world_dialogue_text(&w), "satisfy your curiosity") != 0);
    advance(&w, "See Erek, I know Lady Silverhair well, and I want to find out what really happened here.");
    advance(&w, "Of course. The Lady Silverhair is a wealthy noble; she'd never stoop to theft!");
    assert(strstr(world_dialogue_text(&w), "not enough to prove her innocence") != 0);
    visit(&w, GENERATED_DIALOGUE_EREK_START,
        "I fear this is no good idea. He is still very upset about the theft and it is better not to bother him while he is in that mood.");
    advance(&w, "I am trying to find his gems and he is not even willing to see me!?");
    assert(strstr(world_dialogue_text(&w), "not very sociable") != 0);
    advance(&w, "I am trying to help here, Erek!");
    assert(strstr(world_dialogue_text(&w), "who am I to disobey") != 0);
    advance(&w, "Stubborn Dwarf! But if he is in no hurry to get his gems back, what can I do about it?");
    assert(strstr(world_dialogue_text(&w), "I'll see what I can do") != 0);

    menu = node(GENERATED_DIALOGUE_LUCIA_START, lucia_menu);
    /* The three authored alternatives differ only by these state conditions. */
    noise_flag = menu->choices[3].conditions[0].flag;
    singing_flag = node(GENERATED_DIALOGUE_LUCIA_START,
        "Outside the inn you say? I don't know anything about this, but I suspect that was rather Talan practising than the thief.")->choices[0].conditions[0].flag;
    clerk_flag = node(GENERATED_DIALOGUE_LUCIA_START, "Talan practising what?")->responses[0].conditions[0].flag;
    world_initialize(&w);
    w.flags[noise_flag] = 2;
    w.flags[singing_flag] = 1;
    visit(&w, GENERATED_DIALOGUE_LUCIA_START, lucia_menu);
    advance(&w, noise);
    advance(&w, "What if it wasn't!? If that guard would do his duty instead of wandering off for a singsong at nights, we'd know for sure.");
    assert(strstr(world_dialogue_text(&w), "don't be too harsh with Talan") != 0);
    w.flags[singing_flag] = 2;
    visit(&w, GENERATED_DIALOGUE_LUCIA_START, lucia_menu);
    advance(&w, noise);
    advance(&w, "What if it wasn't!? If that guard would do his duty instead of wandering off for a singsong at nights, we'd know for sure.");
    assert(strstr(world_dialogue_text(&w), "don't be too harsh with Talan") != 0);
    w.flags[singing_flag] = 0;
    w.flags[clerk_flag] = 0;
    visit(&w, GENERATED_DIALOGUE_LUCIA_START, lucia_menu);
    advance(&w, noise);
    advance(&w, "Talan practising what?");
    assert(strstr(world_dialogue_text(&w), "I cannot tell") != 0);
    w.flags[clerk_flag] = 1;
    visit(&w, GENERATED_DIALOGUE_LUCIA_START, lucia_menu);
    advance(&w, noise);
    advance(&w, "Talan practising what?");
    assert(strstr(world_dialogue_text(&w), "Telling on other people") != 0);

    taunt = node(GENERATED_DIALOGUE_ALEK_START,
        "If you don't believe me, why don't you ask Fingolson himself? Oh, I forgot. He doesn't want to talk with people of your kind, does he? Well, I cannot blame him for that.");
    door_flag = taunt->choices[0].conditions[0].flag;
    world_initialize(&w);
    visit(&w, GENERATED_DIALOGUE_ALEK_START,
        "Why should I \"spy\" on a conversation that could be heard up to Erinsford. Nah, I was about to look whether my skills might be needed.");
    advance(&w, guard);
    assert(world_dialogue_choice_count(&w) == 1);
    w.flags[door_flag] = 3;
    assert(world_dialogue_choice_count(&w) == 2);
    advance(&w, "He wasn't very enthusiastic about you, am I right?");
    assert(strstr(world_dialogue_text(&w), "because he didn't accept my offer") != 0);

    world_initialize(&w);
    visit(&w, GENERATED_DIALOGUE_ORLOTH_START, "Sure. Where shall I begin?");
    advance(&w, "Why was the Lady Silverhair accused of the theft?");
    assert(strstr(world_dialogue_text(&w), "didn't like Master Fingolson's offer") != 0);
    advance(&w, 0);
    assert(strstr(world_dialogue_text(&w), "didn't allow the guards to search her room") != 0);
}

/* Interact with Talan using his post-gate actor dialogue override. */
static void talk_to_talan(struct world_state *world)
{
    struct world_actions interact = {0, 0, 1, 0, 0, 0};
    struct world_actor_state *actor = &world->actors[GENERATED_ENTITY_TALAN];
    world->dialogue = -1;
    world->dialogue_actor = -1;
    world->active_script = -1;
    world->control_enabled = 1;
    world->moving = 0;
    actor->scene = world->scene_index;
    actor->schedule = -1;
    actor->dialogue = GENERATED_DIALOGUE_TALAN_START;
    world->player_x = actor->x;
    world->player_y = actor->y + 1;
    world->player_facing = WORLD_NORTH;
    world_update(world, &interact);
    assert(world->dialogue == GENERATED_DIALOGUE_TALAN_START);
}

/* Follow both versions of Jelom's question through Talan and Lucia. */
static void test_talan_secret(void)
{
    struct world_state w;
    const struct generated_runtime_dialogue *talan = &generated_runtime_dialogues[GENERATED_DIALOGUE_TALAN_START];
    const struct generated_runtime_dialogue_node *lucia_menu = node(GENERATED_DIALOGUE_LUCIA_START,
        "Now don't keep me off my work. Ask what you want to know and then out with you!");
    const char *jelom_question = "What!? Are you telling me the kid was right after all? I don't believe it! Can't I even trust my own men any more!? Tell me, what was he doing?";
    const char *key_request = "I need to fetch something for my mistress, but the pantry is locked.";
    const char *angry_key_request = "Excuse me Lucia, I have to fetch something for Lady Silverhair from the pantry, and ...";
    int met_lucia = generated_runtime_dialogues[GENERATED_DIALOGUE_LUCIA_START].starts[0].flag;
    int reported = talan->starts[1].conditions[0].flag;
    int freed = talan->starts[1].flag;
    int apologised = talan->starts[0].flag;
    int get_item = lucia_menu->choices[0].conditions[0].flag;
    int pantry = lucia_menu->choices[0].conditions[1].flag;
    int refuse_key = lucia_menu->choices[0].actions[0].flag;
    achievements_initialize();
    world_initialize(&w);
    w.flags[met_lucia] = 1;
    w.flags[apologised] = 1;
    visit(&w, GENERATED_DIALOGUE_JELOM_START, jelom_question);
    advance(&w, "He sneaked away to take a nap.");
    assert(w.flags[reported] == 0);
    w.flags[freed] = 1;
    talk_to_talan(&w);
    advance(&w, "Hi there, Talan. Look, I am sorry that ...");
    assert(strstr(world_dialogue_text(&w), "favour for covering for me") != 0);
    advance(&w, "I should think so Talan, considering the discomfort you caused my mistress.");
    advance(&w, 0);
    assert(strstr(world_dialogue_text(&w), "visit of Master Fingolson is very unusual") != 0);
    w.flags[get_item] = 1;
    w.flags[pantry] = 1;
    talk(&w, GENERATED_SCRIPT_TALK_TO_LUCIA);
    advance(&w, 0); /* Ordinary repeat narration precedes the question menu. */
    advance(&w, key_request);
    assert(w.flags[pantry] == 2 && w.flags[refuse_key] == 0);
    assert(strstr(world_dialogue_text(&w), "spoken to your Lady") != 0);

    world_initialize(&w);
    w.flags[met_lucia] = 1;
    w.flags[apologised] = 1;
    visit(&w, GENERATED_DIALOGUE_JELOM_START, jelom_question);
    advance(&w, "He wandered off to practise singing, of all things!");
    assert(w.flags[reported] == 1);
    w.flags[freed] = 1;
    w.flags[get_item] = 1;
    w.flags[pantry] = 1;
    talk(&w, GENERATED_SCRIPT_TALK_TO_LUCIA);
    advance(&w, angry_key_request);
    assert(w.flags[refuse_key] == 1 && w.flags[pantry] == 1);
    talk_to_talan(&w);
    advance(&w, "Hi there, Talan. Look, I am sorry that ...");
    assert(w.flags[reported] == 2);
    assert(strstr(world_dialogue_text(&w), "do not hold a grudge") != 0);
    talk(&w, GENERATED_SCRIPT_TALK_TO_LUCIA);
    advance(&w, "Look, Lucia, I spoke with Talan and he understands what I had to do.");
    advance(&w, "May I have the key then?");
    assert(w.flags[reported] == 3 && w.flags[pantry] == 2);
    /* Apologizing before the first key request must not strand this route. */
    w.flags[reported] = 2;
    w.flags[refuse_key] = 0;
    w.flags[pantry] = 1;
    talk(&w, GENERATED_SCRIPT_TALK_TO_LUCIA);
    advance(&w, angry_key_request);
    assert(w.flags[refuse_key] == 1);
    advance(&w, "Look, Lucia, I spoke with Talan and he understands what I had to do.");
    advance(&w, "May I have the key then?");
    assert(w.flags[reported] == 3 && w.flags[pantry] == 2);
}

/* Exercise the original refusal/retry and preservation of both witness bits. */
static void test_gem_refusal_and_witnesses(void)
{
    struct world_state w;
    const struct generated_runtime_dialogue *erek = &generated_runtime_dialogues[GENERATED_DIALOGUE_EREK_START];
    const char *frost_menu = "Ah, the persistent young servant. Do you wish to ask me something more?";
    const char *silverhair_menu = "I have little choice. Do you have news? Or questions?";
    const char *noise_question = "I have spoken with the artist, Frostbloom. She mentioned that she had heard some strange sounds in the night, when the theft must have taken place. Did you hear anything?";
    int witness = node(GENERATED_DIALOGUE_FROSTBLOOM_START, frost_menu)->choices[0].actions[0].flag;
    achievements_initialize();
    world_initialize(&w);
    visit(&w, GENERATED_DIALOGUE_EREK_START, "But certainly it was not lying openly on the ground, was it?");
    advance(&w, "Obviously not. Now would you like to see it?");
    assert(w.flags[erek->starts[0].flag] == 1);
    assert(strstr(world_dialogue_text(&w), "I cannot help you") != 0);
    advance(&w, 0);
    talk(&w, GENERATED_SCRIPT_TALK_TO_EREK);
    advance(&w, 0);
    advance(&w, "So it be. The gem was hidden in Lady Silverhair's luggage.");
    assert(w.flags[erek->starts[0].flag] == 0);
    assert(strstr(world_dialogue_text(&w), "obvious is not necessarily the truth") != 0);
    advance(&w, 0);
    assert(strstr(world_dialogue_text(&w), "Carefully, Erek takes the gem") != 0);

    world_initialize(&w);
    visit(&w, GENERATED_DIALOGUE_FROSTBLOOM_START, frost_menu);
    advance(&w, "Did anything unusual happen last night?");
    assert(w.flags[witness] == 2);
    visit(&w, GENERATED_DIALOGUE_SILVERHAIR_START, silverhair_menu);
    advance(&w, noise_question);
    assert(w.flags[witness] == 3);
    visit(&w, GENERATED_DIALOGUE_LUCIA_START,
        "Now don't keep me off my work. Ask what you want to know and then out with you!");
    advance(&w, "I know that both Lady Silverhair and Lady Frostbloom have heard a noise outside the inn during that night.");
    assert(strstr(world_dialogue_text(&w), "rather Talan practising") != 0);
    /* Adding Frostbloom to an already-recorded Silverhair witness preserves her bit. */
    w.flags[witness] = 1;
    visit(&w, GENERATED_DIALOGUE_FROSTBLOOM_START, frost_menu);
    advance(&w, "Did anything unusual happen last night?");
    assert(w.flags[witness] == 3);
}

/* Gem evidence accumulates in either order and repeat examinations keep responses. */
static void test_gem_assessment_order_and_repeats(void)
{
    struct world_state w;
    struct world_actions idle = {0};
    const struct generated_runtime_dialogue_node *identification = node(GENERATED_DIALOGUE_EREK_START,
        "This is a fine example of a Chrysoberyl. Perfectly suitable for a Lady's ring, "
        "were that your Mistress' intention. However, it is no Catseye and as such it is of no use "
        "in the workings of magic she had in mind, if I recall correctly.");
    const char *returns_gem = "Once caught again, the young Dwarf shakes his head and starts to smile. Finally, he returns the gem.";
    const char *inspection = "You show the jewel to Lady Silverhair, who frowns upon seeing it. "
        "She inspects it more closely, but carefully avoids touching it. "
        "She straightens then, a puzzled look on her features.";
    const char *testimony = "Could you repeat that for Jelom? He would not believe me.";
    int gem = identification->actions[0].flag;
    int convince = identification->choices[0].conditions[0].flag;
    int evidence, permission, repeat, frame;
    for (evidence = 0; evidence < 4; ++evidence) {
        for (permission = 0; permission < 3; ++permission) {
            world_initialize(&w);
            w.flags[gem] = evidence;
            w.flags[convince] = permission;
            for (repeat = 0; repeat < 2; ++repeat) {
                visit(&w, GENERATED_DIALOGUE_EREK_START, returns_gem);
                advance(&w, 0);
                assert(w.flags[gem] == (evidence | 2));
                assert(world_dialogue_choice_count(&w) == (permission == 1 ? 2 : 1));
                /* Thanks remains available even when testimony is also offered. */
                w.dialogue_choice = world_dialogue_choice_count(&w) - 1;
                advance(&w, 0);
                assert(w.flags[convince] == permission);
            }
            if (permission == 1) {
                visit(&w, GENERATED_DIALOGUE_EREK_START, returns_gem);
                advance(&w, 0);
                advance(&w, testimony);
                assert(w.flags[convince] == 2);
            }
            /* Start the separate visit after the testimony escort script. */
            w.active_script = -1;
            visit(&w, GENERATED_DIALOGUE_SILVERHAIR_START, inspection);
            advance(&w, 0);
            advance(&w, 0);
            advance(&w, 0);
            assert(w.flags[gem] == 3);
            advance(&w, 0);
            advance(&w, 0);
            advance(&w, "Of course, Lady. I have believed nothing but your innocence since I arrived. You may trust me to act on your behalf.");
            advance(&w, 0);
            for (frame = 0; frame < 4; ++frame) world_update(&w, &idle);
            assert(w.flags[gem] == 3);
        }
        /* Silverhair alone must preserve any earlier Erek assessment as well. */
        world_initialize(&w);
        w.flags[gem] = evidence;
        visit(&w, GENERATED_DIALOGUE_SILVERHAIR_START, inspection);
        advance(&w, 0);
        advance(&w, 0);
        advance(&w, 0);
        assert(w.flags[gem] == (evidence | 1));
        visit(&w, GENERATED_DIALOGUE_EREK_START, returns_gem);
        advance(&w, 0);
        assert(w.flags[gem] == 3);
    }
}

/* The two original NPC responses both remain reachable through one introduction. */
static void test_lucia_alternatives(void)
{
    struct world_state w;
    int seed, seen = 0;
    for (seed = 0; seed < 32 && seen != 3; ++seed) {
        world_initialize(&w);
        world_seed_random(&w, seed);
        talk(&w, GENERATED_SCRIPT_TALK_TO_LUCIA);
        advance(&w, 0);
        advance(&w, "Sorry madam, I did not mean to trespass, but I need to talk to you.");
        advance(&w, "I am Banec, Lady Silverhair's clerk. I'm investigating the theft.");
        if (strstr(world_dialogue_text(&w), "Then you must be from Cirdanth")) {
            seen |= 1;
            advance(&w, "Yes I am from Cirdanth.");
            assert(strstr(world_dialogue_text(&w), "The deal of his life") != 0);
        } else {
            assert(strstr(world_dialogue_text(&w), "Oh what an awful thing to happen") != 0);
            seen |= 2;
            advance(&w, 0);
            assert(strstr(world_dialogue_text(&w), "But would he listen to me?") != 0);
        }
    }
    assert(seen == 3);
}

/* Door-triggered warnings differ on first and repeat attempts. */
static void test_guarded_door(void)
{
    struct world_state w;
    world_initialize(&w);
    talk(&w, GENERATED_SCRIPT_TALK_TO_JELOM);
    assert(strstr(world_dialogue_text(&w), "Halt there!") != 0);
    advance(&w, "And just who are you to prevent me from entering, sir?");
    advance(&w, "I am Lady Silverhair's clerk. I demand to see her immediately!");
    advance(&w, "What harm could it do if I spoke to my mistress?");
    advance(&w, 0);
    advance(&w, 0);
    talk(&w, GENERATED_SCRIPT_TALK_TO_JELOM);
    assert(strstr(world_dialogue_text(&w), "Stop right there, Half-Elf!") != 0);
}

/* Conversation resets run on opening, while entry-specific effects still follow. */
static void test_start_actions(void)
{
    struct world_state w;
    const struct generated_runtime_dialogue *lucia = &generated_runtime_dialogues[GENERATED_DIALOGUE_LUCIA_START];
    const struct generated_runtime_dialogue *orloth = &generated_runtime_dialogues[GENERATED_DIALOGUE_ORLOTH_START];
    int i, first_run;
    world_initialize(&w);
    assert(lucia->start_action_count == 3);
    for (i = 0; i < lucia->start_action_count; ++i)
        w.flags[lucia->start_actions[i].flag] = 99;
    talk(&w, GENERATED_SCRIPT_TALK_TO_LUCIA);
    for (i = 0; i < lucia->start_action_count; ++i) {
        assert(w.flags[lucia->start_actions[i].flag] == 0);
        w.flags[lucia->start_actions[i].flag] = 99;
    }
    advance(&w, 0);
    for (i = 0; i < lucia->start_action_count; ++i)
        assert(w.flags[lucia->start_actions[i].flag] == 99);
    talk(&w, GENERATED_SCRIPT_TALK_TO_LUCIA);
    for (i = 0; i < lucia->start_action_count; ++i)
        assert(w.flags[lucia->start_actions[i].flag] == 0);
    world_initialize(&w);
    assert(orloth->start_action_count == 2);
    first_run = orloth->start_actions[0].flag;
    w.flags[first_run] = 99;
    talk(&w, GENERATED_SCRIPT_TALK_TO_ORLOTH);
    assert(w.flags[first_run] == 1);
    w.flags[orloth->starts[0].flag] = 1;
    talk(&w, GENERATED_SCRIPT_TALK_TO_ORLOTH);
    assert(w.flags[first_run] == 0);
}

/* Enter a uniquely identifiable line without relying on generated node ordering. */
static void visit_prefix(struct world_state *world, int dialogue, const char *prefix)
{
    const struct generated_runtime_dialogue *d = &generated_runtime_dialogues[dialogue];
    const char *text = 0;
    int i;
    for (i = 0; i < d->node_count; ++i) {
        if (strncmp(d->nodes[i].text, prefix, strlen(prefix))) continue;
        assert(text == 0);
        text = d->nodes[i].text;
    }
    assert(text != 0);
    visit(world, dialogue, text);
}

/* Check the presented text rather than the authored target index. */
static void expect_line(const struct world_state *world, const char *prefix)
{
    assert(world->dialogue >= 0);
    assert(!strncmp(world_dialogue_text(world), prefix, strlen(prefix)));
}

/* Select a visible original response by its distinctive opening words. */
static void choose_prefix(struct world_state *world, const char *prefix)
{
    int i;
    for (i = 0; i < world_dialogue_choice_count(world); ++i) {
        const char *text = world_dialogue_choice_at(world, i);
        if (!strncmp(text, prefix, strlen(prefix))) {
            advance(world, text);
            return;
        }
    }
    fprintf(stderr, "Missing response beginning: %s\n", prefix);
    assert(0);
}

/* Each Fellnir topic retains its own original experiment, answer and dismissal. */
static void test_fellnir_reactions(void)
{
    const struct generated_runtime_dialogue *d = &generated_runtime_dialogues[GENERATED_DIALOGUE_FELLNIR_START];
    const char *questions[] = {"Excuse me?", "Pardon me, good sir?", "Do you know anything about strange noises",
        "You've been such a great help", "I'm still trying to find information"};
    const char *reactions[] = {"Fellnir pours a blueish liquid", "Fellnir picks up a decanter",
        "As you finish your question, bubbles", "Fellnir doesn't even look up", "Fellnir looks suspiciously"};
    const char *answers[] = {"I would have thought elves", "I wouldn't open my door either",
        "I have better things to do", "Well, of course she's upset", "Ask somebody else."};
    struct world_state w;
    int i;
    for (i = 0; i < 5; ++i) {
        world_initialize(&w);
        w.flags[d->starts[0].flag] = 1;
        if (i < 4) w.flags[i == 2 ? d->starts[i + 1].conditions[0].flag : d->starts[i + 1].flag] = i == 0 ? 2 : 1;
        /* All lower-priority topics remain eligible to check ordered selection. */
        if (i < 3) w.flags[d->starts[4].flag] = 1;
        if (i < 2) w.flags[d->starts[3].conditions[0].flag] = 2;
        if (i < 1) w.flags[d->starts[2].flag] = 1;
        talk(&w, GENERATED_SCRIPT_TALK_TO_FELLNIR);
        expect_line(&w, "What do you want from me?");
        assert(world_dialogue_choice_count(&w) == 1);
        choose_prefix(&w, questions[i]);
        expect_line(&w, reactions[i]);
        advance(&w, 0);
        expect_line(&w, answers[i]);
        advance(&w, 0);
        expect_line(&w, "He turns his attention back to his work");
        advance(&w, 0);
        assert(w.dialogue < 0);
    }
}

/* Restore the original successors and choices in the remaining audited branches. */
static void test_original_flow_connections(void)
{
    struct world_state w;
    world_initialize(&w);
    visit_prefix(&w, GENERATED_DIALOGUE_TRISTAN_START, "What? You cannot seriously consider that!");
    advance(&w, 0);
    expect_line(&w, "There is a caravan with Saffron waiting for me");
    advance(&w, 0);
    expect_line(&w, "Nobody will buy any Saffron");

    visit_prefix(&w, GENERATED_DIALOGUE_ORLOTH_START, "It wasn't the argument alone.");
    choose_prefix(&w, "Tell me about the other reasons.");
    expect_line(&w, "Well, for one, the theft must have happened during the night");
    advance(&w, 0);
    expect_line(&w, "Then nobody has left the inn.");

    visit_prefix(&w, GENERATED_DIALOGUE_EREK_START, "I really cannot say why he would do that");
    choose_prefix(&w, "Could you please repeat what you told me about the argument");
    expect_line(&w, "Sure. The three of us sat together in the parlour");

    visit_prefix(&w, GENERATED_DIALOGUE_TALAN_INTRO, "I tell you I can't allow you in.");
    assert(world_dialogue_choice_count(&w) == 2);
    choose_prefix(&w, "I am sick of arguing with you.");
    expect_line(&w, "The guard hesitates for a moment");
}

/* Fingolson continues without invented answers and records both original outcomes. */
static void test_bjarn_original_responses(void)
{
    struct world_state w;
    const struct generated_runtime_dialogue_node *packing = node(GENERATED_DIALOGUE_BJARN_START,
        "You are but wasting my time! Out of my sight you go!");
    int assessment, reply;
    world_initialize(&w);
    visit_prefix(&w, GENERATED_DIALOGUE_BJARN_START, "I can't believe I am justifying myself");
    assert(world_dialogue_choice_count(&w) == 0);
    advance(&w, 0);
    expect_line(&w, "You are but wasting my time!");
    assert(w.flags[packing->actions[0].flag] == 3);

    for (assessment = 0; assessment < 2; ++assessment) {
        for (reply = 0; reply < 2; ++reply) {
            const struct generated_runtime_dialogue_node *concession;
            world_initialize(&w);
            visit_prefix(&w, GENERATED_DIALOGUE_BJARN_START,
                assessment ? "The thief? Of course she would say that!" : "Huh? Erek is young");
            assert(world_dialogue_choice_count(&w) == 2);
            choose_prefix(&w, reply ? "Don't worry Master Fingolson" : "I'm sorry, but I won't give");
            expect_line(&w, "Oh well, it's not as if you could run off with the gem");
            concession = &generated_runtime_dialogues[w.dialogue].nodes[w.dialogue_node];
            assert(w.flags[concession->actions[0].flag] == 1);
            assert(world_dialogue_choice_count(&w) == 0);
            advance(&w, 0);
            assert(w.dialogue < 0);
        }
    }
}

/* Advance linear narration only, failing if a choice unexpectedly interrupts it. */
static void advance_until_line(struct world_state *world, const char *prefix)
{
    int remaining = 40;
    while (remaining-- > 0 && world->dialogue >= 0 &&
           strncmp(world_dialogue_text(world), prefix, strlen(prefix))) {
        assert(world_dialogue_choice_count(world) == 0);
        advance(world, 0);
    }
    expect_line(world, prefix);
}

/* The first conversation must reach lodging even after an account and guest detour. */
static void test_orloth_lodging_and_returns(void)
{
    struct world_state w;
    struct world_actions idle = {0};
    int i;
    world_initialize(&w);
    talk(&w, GENERATED_SCRIPT_TALK_TO_ORLOTH);
    advance_until_line(&w, "But excuse me sir,");
    choose_prefix(&w, "What are those troubles");
    advance_until_line(&w, "And do you know what bothers me");
    choose_prefix(&w, "Do not worry, Master Redwyne.");
    choose_prefix(&w, "Well, actually I'd be glad");
    choose_prefix(&w, "First of all I'd like to know");
    choose_prefix(&w, "Yes I do.");
    advance_until_line(&w, "However, all of us could hear");
    choose_prefix(&w, "And because of that argument");
    choose_prefix(&w, "Tell me about the other reasons.");
    advance_until_line(&w, "Lady Silverhair has been confined");
    choose_prefix(&w, "Now I see a lot clearer.");
    advance_until_line(&w, "But what can I do about it?");
    choose_prefix(&w, "Thank you Master Redwyne. I think");
    /* The original does not offer a recap while the first introduction is pending. */
    for (i = 0; i < world_dialogue_choice_count(&w); ++i)
        assert(strstr(world_dialogue_choice_at(&w, i), "the theft again") == 0);
    choose_prefix(&w, "Not now. You have given me plenty");
    expect_line(&w, "I wish you luck with your inquiries");
    choose_prefix(&w, "Banec, from Cirdanth.");
    expect_line(&w, "Well then, Banec, now we've got to find you a room");
    advance(&w, 0);
    advance(&w, 0);
    for (i = 0; i < 120 && (w.dialogue != GENERATED_DIALOGUE_OLIVER_ROOM ||
         w.transition_phase != WORLD_TRANSITION_NONE); ++i) world_update(&w, &idle);
    assert(w.scene_index == GENERATED_SCENE_REDWYNE_PLAYER_ROOM);
    assert(w.dialogue == GENERATED_DIALOGUE_OLIVER_ROOM);

    /* Ending the account hands off only during the first conversation. */
    for (i = 0; i < 2; ++i) {
        const struct generated_runtime_dialogue *d = &generated_runtime_dialogues[GENERATED_DIALOGUE_ORLOTH_START];
        world_initialize(&w);
        w.flags[d->start_actions[0].flag] = i;
        visit_prefix(&w, GENERATED_DIALOGUE_ORLOTH_START, "Lady Silverhair has been confined");
        choose_prefix(&w, "In that case I'll better hurry");
        if (i) expect_line(&w, "I wish you luck with your inquiries");
        else assert(w.dialogue < 0 && w.active_script < 0);
    }

    /* Recap exits on later visits return to further questions, not out of dialogue. */
    world_initialize(&w);
    visit_prefix(&w, GENERATED_DIALOGUE_ORLOTH_START, "Sure. Where shall I begin?");
    choose_prefix(&w, "I think you were finished.");
    expect_line(&w, "Very well. Is there something else you wish to know");
    assert(world_dialogue_choice_count(&w) > 0);

    visit_prefix(&w, GENERATED_DIALOGUE_ORLOTH_START, "I still wonder why he became a tradesman");
    choose_prefix(&w, "An impolite Dwarf isn't the most pressing problem");
    expect_line(&w, "Certainly. Tristan Illig, a merchant");
}

/* Knowing Jelom and receiving investigative permission remain separate states. */
static void test_jelom_permission_and_evidence(void)
{
    struct world_state w;
    const struct generated_runtime_dialogue_node *permission = node(GENERATED_DIALOGUE_JELOM_START,
        "Listen, Half-Elf! The theft and any investigations are business of the guard and none of yours! But by the looks of it, the only way to keep your nose out would be to lock you up.");
    int allowed = permission->actions[0].flag;
    int noise = node(GENERATED_DIALOGUE_OLIVER_START,
        "Can you imagine, sir? The Elven Lady robbed Master Fingolson in his sleep. And none of the adults noticed anything.")->choices[1].actions[0].flag;
    int singing = node(GENERATED_DIALOGUE_TALAN_START,
        "I'm afraid I was. Oh what terrible mistake! I am so sorry, Banec. I did not know about the noise! It is my fault that Lady Silverhair is being held in her room!")->actions[0].flag;
    world_initialize(&w);
    talk(&w, GENERATED_SCRIPT_TALK_TO_JELOM);
    choose_prefix(&w, "And just who are you");
    assert(world_dialogue_choice_count(&w) == 2);
    choose_prefix(&w, "I am Lady Silverhair's clerk. I demand");
    choose_prefix(&w, "What harm could it do");
    advance(&w, 0);
    expect_line(&w, "Now get lost!");
    assert(w.flags[allowed] == 0);
    advance(&w, 0);
    talk(&w, GENERATED_SCRIPT_TALK_TO_JELOM);
    assert(world_dialogue_choice_count(&w) == 2); /* Door demand plus ordinary questions. */
    choose_prefix(&w, "Nice to see you again, Jelom.");
    expect_line(&w, "Listen, Half-Elf! The theft and any investigations");
    assert(w.flags[allowed] == 1);
    advance(&w, 0);
    choose_prefix(&w, "Can you tell me what happened on the night");
    advance(&w, 0);
    advance(&w, 0);
    advance(&w, 0);
    talk(&w, GENERATED_SCRIPT_TALK_TO_JELOM);
    choose_prefix(&w, "Nice to see you again, Jelom.");
    expect_line(&w, "And what might those questions be");

    /* Oliver's clue can be reported before Talan has admitted singing. */
    w.flags[noise] = 1;
    talk(&w, GENERATED_SCRIPT_TALK_TO_JELOM);
    choose_prefix(&w, "Look, Jelom, I found out something");
    advance(&w, 0);
    expect_line(&w, "You trust in the overactive imagination");
    choose_prefix(&w, "But that noise might have been caused");
    expect_line(&w, "I say this is crap.");
    advance(&w, 0);
    w.flags[singing] = 2;
    talk(&w, GENERATED_SCRIPT_TALK_TO_JELOM);
    choose_prefix(&w, "Did you know that Talan left his post");
    expect_line(&w, "What!? Are you telling me the kid was right");
}

/* Both noise witnesses reach the original direct confrontation and denial state. */
static void test_talan_contextual_followups(void)
{
    struct world_state w;
    const struct generated_runtime_dialogue *d = &generated_runtime_dialogues[GENERATED_DIALOGUE_TALAN_START];
    const struct generated_runtime_dialogue_node *apology = node(GENERATED_DIALOGUE_TALAN_START,
        "I'm afraid I was. Oh what terrible mistake! I am so sorry, Banec. I did not know about the noise! It is my fault that Lady Silverhair is being held in her room!");
    int singing = apology->actions[0].flag;
    int noise = node(GENERATED_DIALOGUE_OLIVER_START,
        "Can you imagine, sir? The Elven Lady robbed Master Fingolson in his sleep. And none of the adults noticed anything.")->choices[1].actions[0].flag;
    int heard = node(GENERATED_DIALOGUE_TALAN_START, "No... no, not at all.")->actions[0].flag;
    int reported = d->starts[1].conditions[0].flag;
    int route;
    for (route = 0; route < 2; ++route) {
        world_initialize(&w);
        w.flags[singing] = 1;
        if (route == 0) {
            w.flags[noise] = 1;
            visit_prefix(&w, GENERATED_DIALOGUE_TALAN_START, "So you did not see or hear anything");
            advance(&w, 0);
            assert(w.flags[heard] == 1);
            advance(&w, 0);
            choose_prefix(&w, "According to Oliver, somebody must have been");
            expect_line(&w, "Well, I did not notice anything");
        } else {
            visit_prefix(&w, GENERATED_DIALOGUE_TALAN_START, "Frostbloom? Then it must have been a Yeti");
            advance(&w, 0);
            expect_line(&w, "But seriously, I did not notice anything");
        }
        choose_prefix(&w, "No, you missed it. Because you were off singing");
        assert(w.flags[singing] == 2);
        expect_line(&w, "I'm afraid I was.");
    }
    /* The denial gates the return-visit confrontation, not merely knowing the song. */
    world_initialize(&w);
    w.flags[d->starts[0].flag] = 1;
    w.flags[singing] = 1;
    talk_to_talan(&w);
    assert(strstr(world_dialogue_choice_at(&w, 0), "couple more questions") != 0);
    w.flags[heard] = 1;
    talk_to_talan(&w);
    assert(strstr(world_dialogue_choice_at(&w, 0), "not lied to me") != 0);
    w.flags[d->starts[1].flag] = 1;
    w.flags[reported] = 2;
    talk_to_talan(&w);
    choose_prefix(&w, "Hi there, Talan. Look, I am sorry that ...");
    expect_line(&w, "No, I understand.");

    visit_prefix(&w, GENERATED_DIALOGUE_TALAN_START, "Well, Master Fingolson can be");
    choose_prefix(&w, "Wasn't the argument rather Fingolson's fault");
    expect_line(&w, "I ... I don't know.");
    visit_prefix(&w, GENERATED_DIALOGUE_TALAN_START, "Well, Master Fingolson can be");
    choose_prefix(&w, "And you say Lady Silverhair was accused");
    expect_line(&w, "Well, I... ah, perhaps...");
}

/* Original optional praise, imprisonment explanation and farewell labels stay in place. */
static void test_remaining_original_choices(void)
{
    struct world_state w;
    world_initialize(&w);
    visit_prefix(&w, GENERATED_DIALOGUE_FROSTBLOOM_START, "Back again, eh? You do hunger");
    assert(world_dialogue_choice_count(&w) == 1);
    choose_prefix(&w, "I try to be diligent");
    assert(world_dialogue_choice_count(&w) == 2);
    choose_prefix(&w, "I have seen the lovely Yeti");
    expect_line(&w, "Do you really think so?");

    visit_prefix(&w, GENERATED_DIALOGUE_OLIVER_START, "Yes, sir. Lady Silverbeard.");
    choose_prefix(&w, "Why would anyone want to lock her up?");
    expect_line(&w, "Can you imagine, sir?");

    visit_prefix(&w, GENERATED_DIALOGUE_EREK_START, "And with that, she left us alone.");
    choose_prefix(&w, "I see. Thank you Erek, you've been most helpful.");
    assert(w.dialogue < 0);
}

/* Both examinations stay available on later visits and preserve the recovered gem. */
static void test_repeat_silverhair_and_bjarn_gem(void)
{
    struct world_state w;
    const struct generated_runtime_dialogue *silverhair = &generated_runtime_dialogues[GENERATED_DIALOGUE_SILVERHAIR_START];
    const struct generated_runtime_dialogue *bjarn = &generated_runtime_dialogues[GENERATED_DIALOGUE_BJARN_START];
    int have_gem = generated_runtime_scripts[GENERATED_SCRIPT_RECORD_FINGOLSON_GEM].actions[1].a;
    int gem = node(GENERATED_DIALOGUE_SILVERHAIR_START,
        "It cannot be. For this is no Catseye at all, and in fact is hardly worth a fraction of the other stones.")->actions[0].flag;
    int lies = node(GENERATED_DIALOGUE_BJARN_START,
        "Oh well, it's not as if you could run off with the gem I suppose. You may keep it for now, "
        "as long as you see to it that Silverhair gets what she deserves. Now be on your way!")->actions[0].flag;
    int repeat;
    world_initialize(&w);
    w.flags[have_gem] = 1;
    w.flags[silverhair->starts[1].flag] = 1;
    w.flags[silverhair->starts[2].flag] = 1;
    w.flags[silverhair->starts[3].flag] = 2;
    w.flags[bjarn->starts[0].flag] = 3;
    w.flags[gem] = 2; /* Erek's assessment already exists. */
    for (repeat = 0; repeat < 2; ++repeat) {
        talk(&w, GENERATED_SCRIPT_TALK_TO_SILVERHAIR);
        expect_line(&w, "Returned already? But what is this?");
        advance_until_line(&w, "I admit he goaded me to rage earlier");
        assert(w.flags[gem] == 3);
        choose_prefix(&w, "Of course, Lady. I have believed nothing but your innocence");
        advance(&w, 0);
        assert(w.dialogue < 0 && w.flags[have_gem] == 1);

        talk(&w, GENERATED_SCRIPT_TALK_TO_BJARN);
        advance(&w, 0);
        choose_prefix(&w, "Not even if I've found one of your 'Catseyes'!");
        choose_prefix(&w, "In Lady Silverhair's luggage.");
        choose_prefix(&w, "But Lady Silverhair swears that this is no Catseye.");
        choose_prefix(&w, "I'm sorry, but I won't give this piece of evidence");
        assert(w.flags[lies] == 1);
        advance(&w, 0);
        assert(w.dialogue < 0 && w.flags[have_gem] == 1 && w.flags[gem] == 3);
    }
    /* Without the gem, Silverhair resumes her ordinary repeat conversation. */
    w.flags[have_gem] = 0;
    talk(&w, GENERATED_SCRIPT_TALK_TO_SILVERHAIR);
    expect_line(&w, "I have little choice. Do you have news? Or questions?");
}

/* Original entry priority and a nonempty angry fallback across quest-state combinations. */
static void test_lucia_entry_priority_and_fallback(void)
{
    struct world_state w;
    const struct generated_runtime_dialogue *d = &generated_runtime_dialogues[GENERATED_DIALOGUE_LUCIA_START];
    const struct generated_runtime_dialogue_node *menu = node(GENERATED_DIALOGUE_LUCIA_START,
        "Now don't keep me off my work. Ask what you want to know and then out with you!");
    int met_flag = d->starts[0].flag;
    int reported_flag = d->starts[1].flag;
    int soothe_flag = node(GENERATED_DIALOGUE_LUCIA_START, "May I have the key then?")->actions[0].flag;
    int item_flag = menu->choices[0].conditions[0].flag;
    int pantry_flag = menu->choices[0].conditions[1].flag;
    int met, reported, soothe, item, pantry;
    for (met = 0; met < 2; ++met) {
        for (reported = 0; reported < 4; ++reported) {
            for (soothe = 0; soothe < 2; ++soothe) {
                for (item = 0; item < 3; ++item) {
                    for (pantry = 0; pantry < 3; ++pantry) {
                        const char *expected;
                        world_initialize(&w);
                        w.flags[met_flag] = met;
                        w.flags[reported_flag] = reported;
                        w.flags[soothe_flag] = soothe;
                        w.flags[item_flag] = item;
                        w.flags[pantry_flag] = pantry;
                        talk(&w, GENERATED_SCRIPT_TALK_TO_LUCIA);
                        if (!met) {
                            expect_line(&w, "There is little doubt that this busy-looking woman");
                            advance(&w, 0);
                            expect_line(&w, "You young people never learn");
                            assert(world_dialogue_choice_count(&w) == 3);
                            continue;
                        }
                        if (reported != 1 && reported != 2) {
                            expect_line(&w, "Lucia seems busy as usual");
                            advance(&w, 0);
                            expect_line(&w, "Come back to hold me off my work again");
                            assert(world_dialogue_choice_count(&w) > 0);
                            continue;
                        }
                        expect_line(&w, "Lucia does not even bother to look up");
                        assert(world_dialogue_choice_count(&w) == 1);
                        if (reported == 2 && soothe) {
                            expected = "Look, Lucia, I spoke with Talan";
                            choose_prefix(&w, expected);
                            expect_line(&w, "He's not in terrible trouble then?");
                            choose_prefix(&w, "May I have the key then?");
                            assert(w.flags[reported_flag] == 3);
                            assert(w.flags[pantry_flag] == 2 && w.flags[soothe_flag] == 0);
                        } else if (item == 1 && pantry == 1) {
                            expected = "Excuse me Lucia, I have to fetch something for Lady Silverhair";
                            choose_prefix(&w, expected);
                            expect_line(&w, "... and now you want my key!?");
                            assert(w.flags[soothe_flag] == 1 && w.flags[pantry_flag] == 1);
                        } else {
                            expected = "Excuse me Lucia, I have ...";
                            choose_prefix(&w, expected);
                            expect_line(&w, "Don't even think of asking me for anything.");
                        }
                    }
                }
            }
        }
    }
}

/* Exercise routes through the real world dialogue and achievement engine. */
int main(void)
{
    test_erek();
    test_openings();
    test_restored_branches();
    test_talan_secret();
    test_gem_refusal_and_witnesses();
    test_gem_assessment_order_and_repeats();
    test_lucia_alternatives();
    test_guarded_door();
    test_start_actions();
    test_fellnir_reactions();
    test_original_flow_connections();
    test_bjarn_original_responses();
    test_orloth_lodging_and_returns();
    test_jelom_permission_and_evidence();
    test_talan_contextual_followups();
    test_remaining_original_choices();
    test_repeat_silverhair_and_bjarn_gem();
    test_lucia_entry_priority_and_fallback();
    puts("Dialogue route tests passed");
    return 0;
}
