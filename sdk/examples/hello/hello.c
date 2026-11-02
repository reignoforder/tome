/* SPDX-License-Identifier: MIT */
/*
 * The smallest useful Tome mod: logs each lifecycle step, looks up a mapped
 * game function, provides JASS natives a map can call (numbers, strings
 * and callbacks), adds a "Hello"
 * button to the main menu, the single player screen and the game (frames
 * from its own ui/hello.fdf), logs game events (what the player selects,
 * chat, pause), adds a /hello chat
 * command, and has settings (config.schema.json, on its page of the mod
 * menu) and a keybind (Greet, Ctrl+G by default).
 *
 * Build it, copy build/mods/hello/ into the game's tome/mods/ folder, and watch
 * tome/tome.log while toggling it in tome/state.json.
 */
#include <tome/tome.h>

static const tome_api* api;
static tome_mod* self;
static int times_enabled;
static int clicks;
static unsigned ticks;
static unsigned game_ticks;
static unsigned orders; /* logged this game: orders are frequent */
static tome_jass_code game_start_callback; /* from HelloOnGameStart, for this game only */

/* native HelloLog takes integer value returns nothing */
static int32_t hello_log(tome_jass_integer value) {
    tome_logf(api, self, TOME_LOG_INFO, "HelloLog(%d) called from JASS", (int)value);
    return 0;
}

/* native HelloLogReal takes real value returns nothing */
static int32_t hello_log_real(tome_jass_real_arg value) {
    tome_logf(api, self, TOME_LOG_INFO, "HelloLogReal(%g) called from JASS", (double)*value);
    return 0;
}

/* native HelloDouble takes real value returns real */
static tome_jass_real_ret hello_double(tome_jass_real_arg value) {
    return tome_jass_real_bits(*value * 2.0f);
}

/* native HelloGreet takes string name returns string */
static tome_jass_string_ret hello_greet(tome_jass_string name) {
    char text[256];
    const char* who = api->jass_string(name);
    if (!who || !*who) who = "world";
    /* Letters, not bytes: JASS's StringLength would count "Ζεύς" as 8. */
    snprintf(text, sizeof(text), "Hello, %s! (%u letters)", who, (unsigned)tome_utf8_length(who));
    return api->jass_string_create(text);
}

/* native HelloCall takes code callback returns boolean */
static tome_jass_boolean hello_call(tome_jass_code callback) {
    int32_t result = 0;
    tome_result status = api->jass_code_call(callback, &result);
    tome_logf(api, self, TOME_LOG_INFO, "HelloCall: callback returned %d (status %d)", (int)result, (int)status);
    return result;
}

/* native HelloRun takes code callback returns nothing */
static int32_t hello_run(tome_jass_code callback) {
    /* Unlike HelloCall, the callback may wait: this returns at its first wait. */
    tome_result status = api->jass_code_execute(callback);
    tome_logf(api, self, TOME_LOG_INFO, "HelloRun: status %d", (int)status);
    return 0;
}

/* native HelloOnGameStart takes code callback returns nothing */
static int32_t hello_on_game_start(tome_jass_code callback) {
    game_start_callback = callback; /* called from on_game_event once the game runs */
    return 0;
}

static void show_clicks(int32_t context) {
    char text[32];
    if (clicks == 0) return;
    snprintf(text, sizeof(text), "Clicked %d", clicks);
    api->frame_set_text(api->frame_get("HelloButtonText", context), text);
}

/* `user` carries the button's create context: one per screen. */
static void TOME_CALL on_hello_clicked(tome_mod* mod, tome_frame* button, void* user) {
    (void)button;
    ++clicks;
    tome_logf(api, mod, TOME_LOG_INFO, "Hello button clicked (%d)", clicks);
    show_clicks((int32_t)(intptr_t)user);
}

/* Runs each time one of the screens is built, so the button comes back with it. */
static void TOME_CALL on_screen(tome_mod* mod, int32_t event, void* user) {
    /* A context per screen, so the frame names never collide. */
    int32_t context = event == TOME_UI_MAIN_MENU ? 0 : event;
    tome_frame* parent;
    tome_frame* anchor;
    tome_frame* button;
    float y = 0.05f;
    (void)user;
    if (event == TOME_UI_MAIN_MENU) {
        parent = api->frame_get("ControlLayer", 0);
        anchor = api->frame_get("MainMenuFrame", 0);
    } else {
        parent = anchor = api->ui_root(event);
        if (event == TOME_UI_GAME) y = 0.3f; /* above the console, over the map */
    }
    if (!parent || !anchor || api->load_fdf(mod, "ui/hello.fdf") != TOME_OK) return;

    /* After a disable and enable on the same screen, the hidden button is
     * still there: a name and context can only be created once. */
    button = api->frame_get("HelloButton", context);
    if (button) {
        api->frame_set_visible(button, 1);
    } else {
        button = api->frame_create(mod, "HelloButton", parent, 0, context);
    }
    if (!button) return;
    api->frame_set_point(button, TOME_FRAMEPOINT_BOTTOM, anchor, TOME_FRAMEPOINT_BOTTOM, 0.0f, y);
    show_clicks(context);
    if (api->frame_on_click(mod, button, on_hello_clicked, (void*)(intptr_t)context) == TOME_OK) {
        tome_logf(api, mod, TOME_LOG_INFO, "added a Hello button to the %s",
                  event == TOME_UI_MAIN_MENU ? "main menu" : event == TOME_UI_GAME ? "game" : "single player screen");
    }
}

static void TOME_CALL on_game_event(tome_mod* mod, const tome_event_info* info, void* user) {
    static const char* const names[] = {"", "map load", "game start", "game end"};
    (void)user;
    if (info->event == TOME_EVENT_TICK) {
        /* Once, to show the tick runs; it fires every frame. */
        if (++ticks == 100) tome_logf(api, mod, TOME_LOG_INFO, "100 ticks (frame %u)", (unsigned)info->frame);
        return;
    }
    if (info->event == TOME_EVENT_GAME_TICK) {
        /* Once per game: game time, which stops while the game is paused. */
        if (++game_ticks == 200) {
            tome_logf(api, mod, TOME_LOG_INFO, "200 game ticks: %.3f s of game time", info->game_time);
        }
        return;
    }
    if (info->event == TOME_EVENT_GAME_START) {
        game_ticks = 0;
        orders = 0;
        if (game_start_callback) {
            int32_t result = 0;
            tome_result status = api->jass_code_call(game_start_callback, &result);
            tome_logf(api, mod, TOME_LOG_INFO, "game start callback returned %d (status %d)", (int)result,
                      (int)status);
        }
    }
    if (info->event == TOME_EVENT_GAME_END) {
        game_start_callback = 0; /* the map's functions go away with the game */
        tome_logf(api, mod, TOME_LOG_INFO, "game end: %s (%s) after %.1f s of game time", info->map_name,
                  info->map_path, info->game_time);
        return;
    }
    tome_logf(api, mod, TOME_LOG_INFO, "%s: %s (%s)", names[info->event], info->map_name, info->map_path);
    /* Fields added in a later API version: check the size first. */
    if (info->event == TOME_EVENT_MAP_LOAD &&
        info->size >= offsetof(tome_event_info, map_description) + sizeof(info->map_description)) {
        tome_logf(api, mod, TOME_LOG_INFO, "map description: %.60s", info->map_description);
    }
    /* The memhack pattern in the map's script (Tome blocks it), and where. */
    if (info->event == TOME_EVENT_MAP_LOAD && tome_has_script_shadows(api) &&
        info->size >= offsetof(tome_event_info, script_shadows) + sizeof(info->script_shadows) &&
        info->script_shadows > 0) {
        tome_script_shadow first;
        int32_t count = tome_get_map_script_shadows(api, &first, 1);
        tome_logf(api, mod, TOME_LOG_INFO, "map script shadows: %u, %d listed (first: local %s %s on line %d, global %s)",
                  (unsigned)info->script_shadows, (int)count, first.local_type, first.name, (int)first.line,
                  first.global_type);
    }
}

/* "Footman (hfoo) of player 1" */
static const char* unit_text(const tome_unit* u, char* buffer, size_t size) {
    char code[5];
    if (!u) return "nobody";
    snprintf(buffer, size, "%s (%s) of player %d", u->name, tome_type_code(u->type_id, code), (int)u->owner + 1);
    return buffer;
}

/* API version 8: what the player selects, chat, pause, units, results, saves. */
static void TOME_CALL on_play_event(tome_mod* mod, const tome_event_info* info, void* user) {
    char code[5], a[128], b[128];
    uint32_t i;
    (void)user;
    switch (info->event) {
    case TOME_EVENT_SELECTION:
        if (info->unit_count == 0) {
            tome_logf(api, mod, TOME_LOG_INFO, "selected nothing");
            return;
        }
        tome_logf(api, mod, TOME_LOG_INFO, "selected %u unit%s", (unsigned)info->unit_count,
                  info->unit_count == 1 ? "" : "s");
        for (i = 0; i < info->unit_count; ++i) {
            const tome_unit* u = &info->units[i];
            tome_logf(api, mod, TOME_LOG_INFO, "  %s%s (%s) of player %d%s%s, handle 0x%x",
                      (int32_t)i == info->leader ? "* " : "", u->name, tome_type_code(u->type_id, code),
                      (int)u->owner + 1, u->flags & TOME_UNIT_STRUCTURE ? ", a building" : "",
                      u->flags & TOME_UNIT_HERO ? ", a hero" : "", (unsigned)u->handle);
        }
        return;
    case TOME_EVENT_CHAT:
        tome_logf(api, mod, TOME_LOG_INFO, "chat from %s (player %d, to %d): %s", info->player_name,
                  (int)info->player + 1, (int)info->recipient, info->text);
        return;
    case TOME_EVENT_PAUSE:
    case TOME_EVENT_RESUME:
        tome_logf(api, mod, TOME_LOG_INFO, "game %s by %s", info->event == TOME_EVENT_PAUSE ? "paused" : "resumed",
                  info->player < 0 ? "a trigger" : info->player_name);
        return;
    case TOME_EVENT_UNIT_DEATH:
        tome_logf(api, mod, TOME_LOG_INFO, "%s died, killed by %s", unit_text(info->unit, a, sizeof(a)),
                  unit_text(info->other, b, sizeof(b)));
        return;
    case TOME_EVENT_UNIT_TRAINED:
        tome_logf(api, mod, TOME_LOG_INFO, "%s trained in %s", unit_text(info->unit, a, sizeof(a)),
                  unit_text(info->other, b, sizeof(b)));
        return;
    case TOME_EVENT_CONSTRUCTION_FINISHED:
        tome_logf(api, mod, TOME_LOG_INFO, "%s built", unit_text(info->unit, a, sizeof(a)));
        return;
    case TOME_EVENT_UPGRADE_FINISHED:
        tome_logf(api, mod, TOME_LOG_INFO, "upgraded to %s", unit_text(info->unit, a, sizeof(a)));
        return;
    case TOME_EVENT_RESEARCH_FINISHED:
        tome_logf(api, mod, TOME_LOG_INFO, "%s (%s) researched in %s", info->object_name,
                  tome_type_code(info->object_id, code), unit_text(info->unit, a, sizeof(a)));
        return;
    case TOME_EVENT_HERO_LEVEL:
        tome_logf(api, mod, TOME_LOG_INFO, "%s reached level %d", unit_text(info->unit, a, sizeof(a)),
                  (int)info->level);
        return;
    case TOME_EVENT_UNIT_SUMMONED:
        tome_logf(api, mod, TOME_LOG_INFO, "%s summoned by %s", unit_text(info->unit, a, sizeof(a)),
                  unit_text(info->other, b, sizeof(b)));
        return;
    case TOME_EVENT_ITEM_PICKUP:
    case TOME_EVENT_ITEM_DROP:
    case TOME_EVENT_ITEM_USE:
        tome_logf(api, mod, TOME_LOG_INFO, "%s %s %s (%s)", unit_text(info->unit, a, sizeof(a)),
                  info->event == TOME_EVENT_ITEM_PICKUP ? "picked up"
                  : info->event == TOME_EVENT_ITEM_DROP ? "dropped" : "used",
                  info->object_name, tome_type_code(info->object_id, code));
        return;
    case TOME_EVENT_PLAYER_VICTORY:
    case TOME_EVENT_PLAYER_DEFEAT:
    case TOME_EVENT_PLAYER_LEFT:
        tome_logf(api, mod, TOME_LOG_INFO, "player %d (%s) %s", (int)info->player + 1, info->player_name,
                  info->event == TOME_EVENT_PLAYER_VICTORY ? "won"
                  : info->event == TOME_EVENT_PLAYER_DEFEAT ? "lost" : "left");
        return;
    case TOME_EVENT_GAME_SAVE:
        tome_logf(api, mod, TOME_LOG_INFO, "saving the game: %s", info->save_name);
        return;
    case TOME_EVENT_GAME_LOADED:
        tome_logf(api, mod, TOME_LOG_INFO, "loaded a saved game: %s", info->save_name);
        return;
    case TOME_EVENT_UNIT_ORDER:
        if (++orders > 20) return;
        if (info->order_type == TOME_ORDER_IMMEDIATE) {
            tome_logf(api, mod, TOME_LOG_INFO, "%s ordered: %u", unit_text(info->unit, a, sizeof(a)),
                      (unsigned)info->order_id);
        } else if (info->other || info->item) {
            tome_logf(api, mod, TOME_LOG_INFO, "%s ordered: %u on %s at %.0f, %.0f", unit_text(info->unit, a, sizeof(a)),
                      (unsigned)info->order_id, info->item ? info->object_name : unit_text(info->other, b, sizeof(b)),
                      info->x, info->y);
        } else {
            tome_logf(api, mod, TOME_LOG_INFO, "%s ordered: %u to %.0f, %.0f", unit_text(info->unit, a, sizeof(a)),
                      (unsigned)info->order_id, info->x, info->y);
        }
        return;
    case TOME_EVENT_UNIT_OWNER_CHANGED:
        tome_logf(api, mod, TOME_LOG_INFO, "%s changed owner from player %d", unit_text(info->unit, a, sizeof(a)),
                  (int)info->player + 1);
        return;
    case TOME_EVENT_HERO_SKILL:
        tome_logf(api, mod, TOME_LOG_INFO, "%s learned %s (%s) level %d", unit_text(info->unit, a, sizeof(a)),
                  info->object_name, tome_type_code(info->object_id, code), (int)info->level);
        return;
    default: {
        /* The rest: the unit, the other unit, and the type or item they're about. */
        static const char* const names[] = {"attacked", "rescued", "construction started", "construction cancelled",
                                            "upgrade started", "upgrade cancelled", "training started",
                                            "training cancelled", "research started", "research cancelled", "",
                                            "revivable", "revival started", "revival cancelled", "revived", "",
                                            "loaded", "sold", "sold an item", "pawned an item"};
        char what[96] = "";
        if (info->event < TOME_EVENT_UNIT_ATTACKED || info->event > TOME_EVENT_ITEM_PAWNED) return;
        if (info->object_id) {
            snprintf(what, sizeof(what), ", %s (%s)", info->object_name, tome_type_code(info->object_id, code));
        }
        tome_logf(api, mod, TOME_LOG_INFO, "%s: %s%s%s%s", names[info->event - TOME_EVENT_UNIT_ATTACKED],
                  unit_text(info->unit, a, sizeof(a)), info->other ? ", other " : "",
                  info->other ? unit_text(info->other, b, sizeof(b)) : "", what);
        return;
    }
    }
}

/* API version 12: spells, damage, keys, dialogs, destructables. */
static void TOME_CALL on_more_event(tome_mod* mod, const tome_event_info* info, void* user) {
    static const char* const spells[] = {"begins channeling", "begins casting", "gets the effect of", "finishes", "stops casting"};
    static const char* const keys[] = {"Left", "Right", "Down", "Up"};
    static const char* const resources[] = {"", "gold", "lumber", "", "food cap", "food used"};
    static int damaged = 0, acquired = 0, created = 0, resource_changes = 0;
    char code[5], a[128], b[128];
    (void)user;
    switch (info->event) {
    case TOME_EVENT_SPELL_CHANNEL:
    case TOME_EVENT_SPELL_CAST:
    case TOME_EVENT_SPELL_EFFECT:
    case TOME_EVENT_SPELL_FINISH:
    case TOME_EVENT_SPELL_END_CAST:
        tome_logf(api, mod, TOME_LOG_INFO, "%s %s %s (%s)%s%s at %.0f, %.0f", unit_text(info->unit, a, sizeof(a)),
                  spells[info->event - TOME_EVENT_SPELL_CHANNEL], info->object_name,
                  tome_type_code(info->object_id, code), info->other ? " on " : "",
                  info->other ? unit_text(info->other, b, sizeof(b)) : "", info->x, info->y);
        return;
    case TOME_EVENT_UNIT_DAMAGED:
        if (++damaged > 20) return;
        tome_logf(api, mod, TOME_LOG_INFO, "%s takes %.1f damage from %s", unit_text(info->unit, a, sizeof(a)),
                  info->amount, unit_text(info->other, b, sizeof(b)));
        return;
    case TOME_EVENT_UNIT_ACQUIRED_TARGET:
        if (++acquired > 20) return;
        tome_logf(api, mod, TOME_LOG_INFO, "%s acquired %s", unit_text(info->unit, a, sizeof(a)),
                  info->other ? unit_text(info->other, b, sizeof(b)) : info->object_name);
        return;
    case TOME_EVENT_UNIT_DECAY:
        tome_logf(api, mod, TOME_LOG_INFO, "%s decays", unit_text(info->unit, a, sizeof(a)));
        return;
    case TOME_EVENT_UNIT_DETECTED:
    case TOME_EVENT_UNIT_HIDDEN:
        tome_logf(api, mod, TOME_LOG_INFO, "%s %s by player %d", unit_text(info->unit, a, sizeof(a)),
                  info->event == TOME_EVENT_UNIT_DETECTED ? "detected" : "hidden", (int)info->player + 1);
        return;
    case TOME_EVENT_ESC_PRESSED:
        tome_logf(api, mod, TOME_LOG_INFO, "%s pressed Esc", info->player_name);
        return;
    case TOME_EVENT_ARROW_KEY:
        tome_logf(api, mod, TOME_LOG_INFO, "%s %s %s", info->player_name, info->pressed ? "pressed" : "released",
                  info->key >= 0 && info->key < 4 ? keys[info->key] : "?");
        return;
    case TOME_EVENT_DESTRUCTABLE_DEATH:
    case TOME_EVENT_ITEM_DEATH:
        tome_logf(api, mod, TOME_LOG_INFO, "%s (%s) destroyed, handle 0x%x", info->object_name,
                  tome_type_code(info->object_id, code),
                  (unsigned)(info->destructable ? info->destructable_handle : info->item_handle));
        return;
    case TOME_EVENT_DIALOG_CLICK:
        tome_logf(api, mod, TOME_LOG_INFO, "%s clicked dialog 0x%x button 0x%x", info->player_name,
                  (unsigned)info->dialog, (unsigned)info->button);
        return;
    case TOME_EVENT_SKILL_MENU:
    case TOME_EVENT_BUILD_MENU:
        tome_logf(api, mod, TOME_LOG_INFO, "%s opened the %s menu", info->player_name,
                  info->event == TOME_EVENT_SKILL_MENU ? "skill" : "build");
        return;
    case TOME_EVENT_LEVEL_END:
        api->log(mod, TOME_LOG_INFO, "the level ends");
        return;
    case TOME_EVENT_UNIT_CREATED:
        /* Players' units only: a map creates many mines and creeps as it loads. */
        if (info->unit->owner >= 12 || ++created > 100) return;
        tome_logf(api, mod, TOME_LOG_INFO, "created %s", unit_text(info->unit, a, sizeof(a)));
        return;
    case TOME_EVENT_UNIT_REMOVED:
        tome_logf(api, mod, TOME_LOG_INFO, "removed %s", unit_text(info->unit, a, sizeof(a)));
        return;
    case TOME_EVENT_HERO_XP:
        tome_logf(api, mod, TOME_LOG_INFO, "%s gained %.0f experience (level %d)", unit_text(info->unit, a, sizeof(a)),
                  info->amount, (int)info->level);
        return;
    case TOME_EVENT_PLAYER_RESOURCE:
        if (++resource_changes > 40) return;
        tome_logf(api, mod, TOME_LOG_INFO, "%s's %s: %.0f -> %.0f", info->player_name,
                  info->resource > 0 && info->resource < 6 ? resources[info->resource] : "?", info->previous,
                  info->amount);
        return;
    case TOME_EVENT_DAY:
    case TOME_EVENT_NIGHT:
        tome_logf(api, mod, TOME_LOG_INFO, "%s at %.2f", info->event == TOME_EVENT_DAY ? "day breaks" : "night falls",
                  info->amount);
        return;
    default:
        return;
    }
}

/* The greeting, from the mod's settings (API version 7) when Tome has them. */
static void greet(tome_mod* mod, const char* who, const char* source) {
    char greeting[64] = "Hello", audience[32] = "world", text[256];
    int repeat = 1, i;
    double seconds = 5;
    int shout = 0;
    if (tome_has_settings(api)) {
        api->setting_string(mod, "greeting", greeting, sizeof(greeting));
        api->setting_string(mod, "audience", audience, sizeof(audience));
        shout = api->setting_bool(mod, "shout", 0);
        repeat = api->setting_int(mod, "repeat", 1);
        seconds = api->setting_number(mod, "seconds", 5);
    }
    snprintf(text, sizeof(text), "%s, %s%s", greeting, who && *who ? who : audience, shout ? "!" : ".");
    tome_logf(api, mod, TOME_LOG_INFO, "%s: %s (x%d, %.1f s)", source, text, repeat, seconds);
    for (i = 0; i < repeat; ++i) api->display_text(text, (float)seconds);
}

/* /hello [text] in the in-game chat. */
static void TOME_CALL on_hello_command(tome_mod* mod, const char* args, void* user) {
    (void)user;
    greet(mod, args, "/hello command");
}

/* The Greet key (tome.mod.json "keybinds"). */
static void TOME_CALL on_greet_key(tome_mod* mod, const char* id, void* user) {
    (void)id;
    (void)user;
    greet(mod, NULL, "Greet key");
}

/* Popups (API version 10): the Popup key opens a small menu of them. */
enum { POPUP_GREET, POPUP_MANY, POPUP_RESTART, POPUP_CLOSE };

static void TOME_CALL on_many_chosen(tome_mod* mod, uint32_t popup, int32_t choice, void* user) {
    (void)user;
    tome_logf(api, mod, TOME_LOG_INFO, "popup %u: option %d", (unsigned)popup, (int)choice);
}

static void TOME_CALL on_restart_chosen(tome_mod* mod, uint32_t popup, int32_t choice, void* user) {
    (void)popup;
    (void)user;
    if (choice == 0) {
        api->log(mod, TOME_LOG_INFO, "restarting the game");
        api->restart_game(mod);
    }
}

static void TOME_CALL on_menu_chosen(tome_mod* mod, uint32_t popup, int32_t choice, void* user) {
    static const char* const numbers[] = {"One", "Two", "Three", "Four", "Five", "Six", "Seven",
                                          "Eight", "Nine", "Ten", "Eleven", "Twelve", "Thirteen", "Fourteen"};
    static const char* const restart_options[] = {"Restart Now", "Later"};
    tome_popup p = {0};
    (void)popup;
    (void)user;
    p.size = sizeof(p);
    p.default_option = -1;
    if (choice == POPUP_GREET) {
        greet(mod, NULL, "popup");
    } else if (choice == POPUP_MANY) {
        p.title = "Pick a Number";
        p.text = "As many options as a mod likes: six to a page, with Previous and Next (or the arrow keys).";
        p.options = numbers;
        p.option_count = (int32_t)(sizeof(numbers) / sizeof(numbers[0]));
        p.cancel_option = -1;
        api->popup(mod, &p, on_many_chosen, NULL, NULL);
    } else if (choice == POPUP_RESTART) {
        p.title = "Restart Required";
        p.text = "Warcraft III closes and starts again with the same command line. Restart now?";
        p.options = restart_options;
        p.option_count = 2;
        p.default_option = 0;
        p.cancel_option = 1;
        api->popup(mod, &p, on_restart_chosen, NULL, NULL);
    }
}

/* The Popup key. */
static void TOME_CALL on_popup_key(tome_mod* mod, const char* id, void* user) {
    static const char* const options[] = {"Greet", "Many Options", "Restart Warcraft III", "Close"};
    tome_popup p = {0};
    (void)id;
    (void)user;
    if (!tome_has_popups(api)) return;
    p.size = sizeof(p);
    p.title = "Hello Tome";
    p.text = "A popup from the |cffffcc00hello|r example. Enter and Esc work too.";
    p.options = options;
    p.option_count = 4;
    p.default_option = POPUP_GREET;
    p.cancel_option = POPUP_CLOSE;
    api->popup(mod, &p, on_menu_chosen, NULL, NULL);
}

static void TOME_CALL on_setting_changed(tome_mod* mod, const char* key, void* user) {
    (void)user;
    tome_logf(api, mod, TOME_LOG_INFO, "setting changed: %s", key);
}

TOME_EXPORT int TOME_CALL tome_mod_load(const tome_api* tome, tome_mod* mod) {
    const tome_symbol* set_text;
    if (tome->api_version < 2 || tome->size < offsetof(tome_api, resolve) + sizeof(tome->resolve)) {
        return 1; /* needs resolve (API version 2); the frames are optional */
    }
    api = tome;
    self = mod;
    const tome_game_info* game = api->game_info();
    tome_logf(api, mod, TOME_LOG_INFO, "loaded (Tome %s, Warcraft III %s, game.dll at 0x%08lx)",
              api->tome_version, game->build_name, (unsigned long)game->game_base);

    /* Listed in "requires", so Tome only loads this mod where it's mapped. */
    set_text = api->resolve("CTextFrame::SetText");
    if (!set_text) return 1;
    tome_logf(api, mod, TOME_LOG_INFO, "%s is a %s function at 0x%08lx (%s)", set_text->name,
              set_text->convention, (unsigned long)set_text->value, set_text->signature);
    return 0;
}

TOME_EXPORT void TOME_CALL tome_mod_enable(tome_mod* mod) {
    int32_t i;
    ++times_enabled;
    api->register_native(mod, "HelloLog", (void*)hello_log);
    api->register_native(mod, "HelloLogReal", (void*)hello_log_real);
    api->register_native(mod, "HelloDouble", (void*)hello_double);
    if (tome_has_jass_values(api)) {
        api->register_native(mod, "HelloGreet", (void*)hello_greet);
        api->register_native(mod, "HelloCall", (void*)hello_call);
        api->register_native(mod, "HelloRun", (void*)hello_run);
        api->register_native(mod, "HelloOnGameStart", (void*)hello_on_game_start);
    }
    /* Revoked on disable, which also hides the button. */
    if (tome_has_frames(api)) api->on_ui(mod, TOME_UI_MAIN_MENU, on_screen, NULL);
    if (tome_has_events(api)) {
        api->on_ui(mod, TOME_UI_SINGLE_PLAYER, on_screen, NULL);
        api->on_ui(mod, TOME_UI_GAME, on_screen, NULL);
        api->on_event(mod, TOME_EVENT_MAP_LOAD, on_game_event, NULL);
        api->on_event(mod, TOME_EVENT_GAME_START, on_game_event, NULL);
        api->on_event(mod, TOME_EVENT_GAME_END, on_game_event, NULL);
        api->on_event(mod, TOME_EVENT_TICK, on_game_event, NULL);
        api->on_event(mod, TOME_EVENT_GAME_TICK, on_game_event, NULL);
        api->register_command(mod, "hello", on_hello_command, NULL);
    }
    if (tome_has_units(api)) {
        api->on_event(mod, TOME_EVENT_SELECTION, on_play_event, NULL);
        api->on_event(mod, TOME_EVENT_CHAT, on_play_event, NULL);
        api->on_event(mod, TOME_EVENT_PAUSE, on_play_event, NULL);
        api->on_event(mod, TOME_EVENT_RESUME, on_play_event, NULL);
        for (i = TOME_EVENT_UNIT_DEATH; i <= TOME_EVENT_UNIT_OWNER_CHANGED; ++i) api->on_event(mod, i, on_play_event, NULL);
    }
    if (tome_has_more_events(api)) {
        for (i = TOME_EVENT_SPELL_CHANNEL; i <= TOME_EVENT_NIGHT; ++i) api->on_event(mod, i, on_more_event, NULL);
    }
    if (tome_has_settings(api)) {
        api->on_setting_changed(mod, on_setting_changed, NULL);
        api->on_keybind(mod, "greet", on_greet_key, NULL);
        api->on_keybind(mod, "popup", on_popup_key, NULL);
    }
    tome_logf(api, mod, TOME_LOG_INFO, "enabled (%d time%s this session)", times_enabled,
              times_enabled == 1 ? "" : "s");
}

TOME_EXPORT void TOME_CALL tome_mod_disable(tome_mod* mod) {
    /* Tome detaches the natives, the callbacks and the command itself and
     * hides the buttons; the natives return 0 until re-enabled. */
    api->log(mod, TOME_LOG_INFO, "disabled");
}

TOME_EXPORT void TOME_CALL tome_mod_unload(tome_mod* mod) {
    api->log(mod, TOME_LOG_INFO, "unloading");
}
