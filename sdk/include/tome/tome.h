/*
 * Tome mod API.
 *
 * A code mod is a 32-bit Windows DLL that exports the four lifecycle
 * functions below. Tome passes a `tome_api` function table to
 * tome_mod_load; everything a mod does goes through that table, so Tome can
 * track and undo it when the mod is disabled.
 *
 * ABI rules:
 *   - Plain C, __cdecl, no CRT objects across the boundary. Mods may be
 *     written in any language that can produce such a DLL.
 *   - Strings are UTF-8 and owned by Tome unless stated otherwise. They stay
 *     valid until tome_mod_unload returns. <tome/utf8.h> (included below)
 *     counts and cuts them by code point and converts them to UTF-16.
 *   - The table only ever grows. Check `size` before using a field added
 *     after the API version you build against.
 */
#ifndef TOME_TOME_H
#define TOME_TOME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1: initial table. 2: resolve. 3: frames. 4: hooks. 5: events, chat commands, every screen.
 * 6: JASS strings and code. 7: settings and keybinds. 8: gameplay events (selection, chat, pause,
 * units, items, results, saved games) and selection(). 9: hooks on game.dll's imports
 * (TOME_SYMBOL_IMPORT). 10: popups and restart_game. 11: services and messages between mods.
 * 12: more events (spells, damage, target acquired, decay, detection, Esc and arrow keys,
 * destructable and item deaths, dialog clicks, skill and build menus, level end, units
 * created and removed, hero experience, player resources, day and night).
 * 13: the map script's memhack pattern (script_shadows, map_script_shadows). */
#define TOME_API_VERSION 14

#define TOME_CALL __cdecl
/* C linkage in C++ too: Tome looks the lifecycle functions up by their plain names. */
#ifdef __cplusplus
#define TOME_EXPORT extern "C" __declspec(dllexport)
#else
#define TOME_EXPORT __declspec(dllexport)
#endif

typedef struct tome_mod tome_mod; /* opaque handle identifying the calling mod */

typedef enum tome_result {
    TOME_OK = 0,
    TOME_ERR_INVALID_ARGUMENT = 1,
    TOME_ERR_NOT_DECLARED = 2,  /* native isn't listed in the mod's tome.mod.json "natives" */
    TOME_ERR_NOT_OWNER = 3,     /* native belongs to another mod */
    TOME_ERR_UNAVAILABLE = 4,   /* feature not supported on this game build, or not right now */
    TOME_ERR_WRONG_THREAD = 5,  /* must be called on the game's main thread */
    TOME_ERR_FAILED = 6         /* the game or the file system reported an error; see tome.log */
} tome_result;

/*
 * JASS native ABI (verified on 1.27b, see docs/research.md §3.3).
 *
 * The engine calls natives as __cdecl functions with one 32-bit argument per
 * JASS parameter:
 *   integer   int32_t
 *   boolean   int32_t (0 or 1)
 *   real      const float*  (a pointer to the value, not the value)
 *   handle    uint32_t handle id (unit, player, trigger, ...)
 *   string    tome_jass_string: the engine's string object (an RCString*);
 *             read it with jass_string. NULL for a null string.
 *   code      tome_jass_code: a function of the map script; run it with
 *             jass_code_call
 * Return values go in eax, so a native returning `real` returns the float's
 * bits as a uint32_t (use tome_jass_real_bits), and one returning `string`
 * returns an entry of the script's string table, made with
 * jass_string_create. Returning nothing: return 0.
 */
typedef int32_t tome_jass_integer;
typedef int32_t tome_jass_boolean;
typedef uint32_t tome_jass_handle;
typedef const float* tome_jass_real_arg;
typedef uint32_t tome_jass_real_ret;
typedef const void* tome_jass_string;    /* a string argument */
typedef uint32_t tome_jass_string_ret;   /* a string result; 0 is null */
typedef uint32_t tome_jass_code;         /* a code argument; 0 is null */

typedef enum tome_log_level {
    TOME_LOG_DEBUG = 0,
    TOME_LOG_INFO = 1,
    TOME_LOG_WARN = 2,
    TOME_LOG_ERROR = 3
} tome_log_level;

typedef struct tome_game_info {
    uint32_t size;               /* sizeof(tome_game_info) as known to Tome */
    const char* build_name;      /* e.g. "1.27b" */
    const char* file_version;    /* e.g. "1.27.1.7085" */
    uintptr_t game_base;         /* load address of game.dll; add RVAs to this */
} tome_game_info;

/*
 * An entry of Tome's mapping database for the running build (mappings/<build>.json
 * in the Tome repository). Names are stable across builds; addresses aren't.
 */
typedef enum tome_symbol_kind {
    TOME_SYMBOL_FUNCTION = 1,  /* code; see `convention` and `signature` */
    TOME_SYMBOL_DATA = 2,      /* a global or a vtable */
    TOME_SYMBOL_CALL = 3,      /* an E8 rel32 call instruction */
    TOME_SYMBOL_OFFSET = 4,    /* a byte offset inside a struct or vtable */
    TOME_SYMBOL_CONSTANT = 5,  /* any other build-specific number */
    /* API version 9: a function game.dll imports from another DLL, e.g.
     * "Import.GetTickCount"; `value` is its import address table slot. */
    TOME_SYMBOL_IMPORT = 6
} tome_symbol_kind;

typedef struct tome_symbol {
    uint32_t size;               /* sizeof(tome_symbol) as known to Tome */
    const char* name;
    int32_t kind;                /* tome_symbol_kind */
    /* FUNCTION, DATA, CALL, IMPORT: the absolute address (game_base + rva).
     * OFFSET, CONSTANT: the number itself. */
    uintptr_t value;
    uint32_t rva;                /* 0 for OFFSET and CONSTANT */
    const char* convention;      /* "cdecl", "stdcall", "fastcall", "thiscall"; "" unless FUNCTION or IMPORT */
    const char* signature;       /* C-style prototype, or "" */
} tome_symbol;

/*
 * Frames (API version 3): the game's UI objects, the same ones 1.31's
 * Blz*Frame* natives work with. A tome_frame* is the game's own CFrame*. It
 * stays valid while the screen it belongs to exists: for the main menu, until
 * the next TOME_UI_MAIN_MENU callback; for the in-game UI, until the game
 * ends. Look frames up again by name in each screen's on_ui callback
 * instead of keeping pointers. Frame functions must be called on the game's
 * main thread (lifecycle callbacks after startup, UI and click callbacks,
 * JASS natives); elsewhere they fail with TOME_ERR_WRONG_THREAD.
 */
typedef struct tome_frame tome_frame;

/* Anchor points, numbered like 1.31's FRAMEPOINT_*. */
typedef enum tome_frame_point {
    TOME_FRAMEPOINT_TOPLEFT = 0,
    TOME_FRAMEPOINT_TOP = 1,
    TOME_FRAMEPOINT_TOPRIGHT = 2,
    TOME_FRAMEPOINT_LEFT = 3,
    TOME_FRAMEPOINT_CENTER = 4,
    TOME_FRAMEPOINT_RIGHT = 5,
    TOME_FRAMEPOINT_BOTTOMLEFT = 6,
    TOME_FRAMEPOINT_BOTTOM = 7,
    TOME_FRAMEPOINT_BOTTOMRIGHT = 8
} tome_frame_point;

/*
 * Screens, for on_ui. Each callback runs once for every time its screen is
 * built: create the mod's frames for that screen there.
 */
typedef enum tome_ui_event {
    /* The main menu was just built (at startup and every time the player
     * returns to it). */
    TOME_UI_MAIN_MENU = 1,
    /* API version 5: the other menu screens, each built when the player opens it. */
    TOME_UI_SINGLE_PLAYER = 2,
    TOME_UI_CUSTOM_GAME = 3,
    TOME_UI_OPTIONS = 4,
    TOME_UI_CAMPAIGN = 5,
    TOME_UI_CUSTOM_CAMPAIGN = 6,
    TOME_UI_LOAD_SAVED_GAME = 7,
    TOME_UI_VIEW_REPLAY = 8,
    TOME_UI_LAN = 9,
    TOME_UI_BATTLE_NET = 10,
    TOME_UI_LOADING = 11,       /* the loading screen of a map */
    TOME_UI_SCORE_SCREEN = 12,  /* after a game */
    /* The in-game UI, once loading has finished (just before
     * TOME_EVENT_GAME_START). Its root frame is ui_root(TOME_UI_GAME). */
    TOME_UI_GAME = 13
} tome_ui_event;

typedef void (TOME_CALL *tome_ui_fn)(tome_mod* mod, int32_t event, void* user);
typedef void (TOME_CALL *tome_click_fn)(tome_mod* mod, tome_frame* button, void* user);

/*
 * Events (API version 5). Handlers run on the game's main thread, in the
 * order they were registered.
 */
typedef enum tome_event {
    /* A map is about to load: its natives are bound, but its script hasn't
     * run. The script is already compiled (war3map.j, common.j and
     * Blizzard.j were read when the map was picked), but none of its other
     * files have been read. Mods' asset changes with "reload": "map" apply
     * right before this, so a changed script file takes effect from the
     * next map on. */
    TOME_EVENT_MAP_LOAD = 1,
    /* Loading has finished and the game is running (single player, LAN,
     * saved games, replays). TOME_UI_GAME callbacks have run just before. A
     * handler registered while a game runs (the mod was enabled mid-game) is
     * called once for that game, on the main thread, after
     * tome_mod_enable returns. */
    TOME_EVENT_GAME_START = 2,
    /* The player is leaving the game (quitting to the menus, loading another
     * map, or exiting the program), before the game is torn down. Once per
     * TOME_EVENT_GAME_START. Not when the process is killed or crashes. */
    TOME_EVENT_GAME_END = 3,
    /* Once per frame the game draws, in menus, loading screens and games.
     * Follows the screen, not the game: use it for UI. Keep it cheap. */
    TOME_EVENT_TICK = 4,
    /* Once per step of the game's simulation, in a game only: game time,
     * the clock JASS timers run on. It follows the game speed and stops
     * while the game is paused. In 1.27b a step is 0.005 s of game time
     * (200 a second at the normal speed). Keep it cheap. */
    TOME_EVENT_GAME_TICK = 5,
    /* API version 8. */
    /* The local player's selection changed, however it happened (clicks,
     * drag boxes, control groups, Tab, triggers, a selected unit dying),
     * including to nothing: units, unit_count, leader. In a game only;
     * checked once per frame. A handler registered mid-game hears of the
     * next change: read the current selection with selection(). */
    TOME_EVENT_SELECTION = 6,
    /* A player's chat message was shown in the message area, the local
     * player's own included: player, player_name, text, recipient. Lines
     * with a chat command (/tome, a mod's) aren't sent, so they don't show. */
    TOME_EVENT_CHAT = 7,
    /* The game was paused (a player's pause, the Esc menu in single
     * player, a trigger's PauseGame) or resumed: player. */
    TOME_EVENT_PAUSE = 8,
    TOME_EVENT_RESUME = 9,
    /* Unit events: any player's units, as the game's JASS player-unit
     * events see them, but whether or not the map listens. They run inside
     * the game's simulation: keep them cheap, and remember that changing
     * the game from them only on this client desyncs a multiplayer game. */
    /* A unit died: unit, and other = the killer (NULL if none). */
    TOME_EVENT_UNIT_DEATH = 10,
    /* A unit finished training: unit = the new unit, other = the building. */
    TOME_EVENT_UNIT_TRAINED = 11,
    /* A building was finished: unit. */
    TOME_EVENT_CONSTRUCTION_FINISHED = 12,
    /* A building finished upgrading (Town Hall to Keep): unit, with its new type. */
    TOME_EVENT_UPGRADE_FINISHED = 13,
    /* A research finished: unit = where, object_id and object_name = what. */
    TOME_EVENT_RESEARCH_FINISHED = 14,
    /* A hero gained a level: unit, level. */
    TOME_EVENT_HERO_LEVEL = 15,
    /* A unit was summoned: unit = the summoned unit, other = the summoner. */
    TOME_EVENT_UNIT_SUMMONED = 16,
    /* A unit picked up (or was given), dropped (or lost) or used an item:
     * unit, item, object_id and object_name = the item's type. */
    TOME_EVENT_ITEM_PICKUP = 17,
    TOME_EVENT_ITEM_DROP = 18,
    TOME_EVENT_ITEM_USE = 19,
    /* A player won, lost or left the game: player, player_name. Once per
     * player and game each. */
    TOME_EVENT_PLAYER_VICTORY = 20,
    TOME_EVENT_PLAYER_DEFEAT = 21,
    TOME_EVENT_PLAYER_LEFT = 22,
    /* The game is about to be saved: save_name. Write the mod's own state
     * for that save now (e.g. a file named after it in config_dir). */
    TOME_EVENT_GAME_SAVE = 23,
    /* A saved game was loaded: save_name, then TOME_EVENT_GAME_START. Read
     * the mod's state for that save here. */
    TOME_EVENT_GAME_LOADED = 24,
    /* More unit events, like the ones above. */
    /* A unit is being attacked: unit = the target, other = the attacker.
     * Fires on every attack, so it's frequent in fights. */
    TOME_EVENT_UNIT_ATTACKED = 25,
    /* A unit was rescued: unit, other = the rescuer. */
    TOME_EVENT_UNIT_RESCUED = 26,
    /* Construction of a building began, or was cancelled: unit. */
    TOME_EVENT_CONSTRUCTION_STARTED = 27,
    TOME_EVENT_CONSTRUCTION_CANCELLED = 28,
    /* A building began or cancelled an upgrade: unit, object_id = the type
     * it upgrades to. */
    TOME_EVENT_UPGRADE_STARTED = 29,
    TOME_EVENT_UPGRADE_CANCELLED = 30,
    /* A building began or cancelled training: unit, object_id = the unit type. */
    TOME_EVENT_TRAINING_STARTED = 31,
    TOME_EVENT_TRAINING_CANCELLED = 32,
    /* A research began or was cancelled: unit, object_id = the research. */
    TOME_EVENT_RESEARCH_STARTED = 33,
    TOME_EVENT_RESEARCH_CANCELLED = 34,
    /* A hero learned a skill: unit, object_id = the ability, level = its level. */
    TOME_EVENT_HERO_SKILL = 35,
    /* A dead hero can be revived; its revival began, was cancelled, or
     * finished: unit. */
    TOME_EVENT_HERO_REVIVABLE = 36,
    TOME_EVENT_HERO_REVIVE_STARTED = 37,
    TOME_EVENT_HERO_REVIVE_CANCELLED = 38,
    TOME_EVENT_HERO_REVIVED = 39,
    /* A unit was given an order, by a player, the AI or a trigger: unit,
     * order_id, order_type; a point order's target x, y; a target order's
     * target as other (a unit) or item (NULL for a destructable), and the
     * target's position as x, y when the game records it (else 0, 0).
     * API version 12: a destructable target as destructable.
     * Orders are frequent (AI and creeps too): keep the handler cheap. */
    TOME_EVENT_UNIT_ORDER = 40,
    /* A unit was loaded into a transport: unit, other = the transport. */
    TOME_EVENT_UNIT_LOADED = 41,
    /* A shop sold a unit: unit = the unit sold, other = the shop, third =
     * the buyer. */
    TOME_EVENT_UNIT_SOLD = 42,
    /* A shop sold an item: unit = the shop, other = the buyer, item. */
    TOME_EVENT_ITEM_SOLD = 43,
    /* A unit sold (pawned) an item to a shop: unit = the seller, other = the
     * shop, item. */
    TOME_EVENT_ITEM_PAWNED = 44,
    /* A unit changed owner: unit (with its new owner), player = the
     * previous owner. */
    TOME_EVENT_UNIT_OWNER_CHANGED = 45,
    /* API version 12. Like the unit events above, these run inside the
     * game's simulation, for every player, whether or not the map listens. */
    /* A unit began channeling a spell, began casting it, the spell took
     * effect, finished, or the unit stopped casting (finished or not):
     * unit = the caster, object_id and object_name = the ability; the
     * target: other (a unit), item or destructable, else none; x, y = the
     * target point (0, 0 for spells without one). Item abilities count too
     * (using a potion casts its ability). */
    TOME_EVENT_SPELL_CHANNEL = 46,
    TOME_EVENT_SPELL_CAST = 47,
    TOME_EVENT_SPELL_EFFECT = 48,
    TOME_EVENT_SPELL_FINISH = 49,
    TOME_EVENT_SPELL_END_CAST = 50,
    /* A unit takes damage, before its life goes down: unit = the target,
     * other = the source (NULL if none), amount = the damage. Frequent in
     * fights. */
    TOME_EVENT_UNIT_DAMAGED = 51,
    /* A unit acquired a target to attack on its own: unit, and the target
     * as other (a unit), item or destructable. */
    TOME_EVENT_UNIT_ACQUIRED_TARGET = 52,
    /* A dead unit's corpse began to decay: unit. */
    TOME_EVENT_UNIT_DECAY = 53,
    /* A player detected an invisible unit: unit, player = the detecting
     * player. UNIT_HIDDEN is the game's EVENT_PLAYER_UNIT_HIDDEN, the same
     * way round. */
    TOME_EVENT_UNIT_DETECTED = 54,
    TOME_EVENT_UNIT_HIDDEN = 55,
    /* A player pressed Esc (in a cinematic or not; the JASS
     * EVENT_PLAYER_END_CINEMATIC): player, player_name. Every player's
     * press reaches every client. */
    TOME_EVENT_ESC_PRESSED = 56,
    /* A player pressed or released an arrow key: player, player_name, key
     * (tome_arrow_key), pressed (1 pressed, 0 released). Only while the
     * map listens for that player's arrow keys: the game doesn't send them
     * otherwise. For the local player's keys on any map, use a keybind. */
    TOME_EVENT_ARROW_KEY = 57,
    /* A destructable (tree, gate, crate...) was destroyed: destructable,
     * destructable_handle, object_id and object_name = its type. */
    TOME_EVENT_DESTRUCTABLE_DEATH = 58,
    /* An item was destroyed (its life ran out): item, item_handle,
     * object_id and object_name. Not when an item is used up or removed. */
    TOME_EVENT_ITEM_DEATH = 59,
    /* A player clicked a dialog button (DialogCreate, DialogAddButton):
     * player, player_name, dialog and button = their JASS handle ids. */
    TOME_EVENT_DIALOG_CLICK = 60,
    /* A player opened a hero's skill menu or a worker's build menu on the
     * command card (EVENT_GAME_SHOW_SKILL, EVENT_GAME_BUILD_SUBMENU):
     * player, player_name. Every player's clicks reach every client. */
    TOME_EVENT_SKILL_MENU = 61,
    TOME_EVENT_BUILD_MENU = 62,
    /* The current level ends (EVENT_GAME_END_LEVEL): ChangeLevel, LoadGame
     * from a script, or a restart, before the game is torn down. */
    TOME_EVENT_LEVEL_END = 63,
    /* A unit was created, by a script, training, a summon or the map: unit.
     * Units placed on the map are created while it loads, after MAP_LOAD.
     * unit->handle is usually 0 here: the event comes while the game
     * creates the unit, before a script's CreateUnit gets its handle
     * (compare `object` in later events instead). */
    TOME_EVENT_UNIT_CREATED = 64,
    /* A unit was taken out of the game (RemoveUnit, and abilities that
     * replace units): unit. Not when a corpse finishes decaying. */
    TOME_EVENT_UNIT_REMOVED = 65,
    /* A hero gained experience (kills, AddHeroXP, SetHeroLevel...): unit,
     * amount = the experience gained, level = the hero's level after it. */
    TOME_EVENT_HERO_XP = 66,
    /* A player's gold, lumber or food changed: player, player_name,
     * resource (tome_resource), amount = the new value, previous = the
     * value before. Checked every game tick, so several changes within one
     * tick come as one. */
    TOME_EVENT_PLAYER_RESOURCE = 67,
    /* The time of day passed 6:00 (day) or 18:00 (night), or a script set
     * it across one: amount = the time of day in hours. Checked every game
     * tick. */
    TOME_EVENT_DAY = 68,
    TOME_EVENT_NIGHT = 69
} tome_event;

/* tome_event_info.resource for TOME_EVENT_PLAYER_RESOURCE (the JASS
 * playerstate values). */
typedef enum tome_resource {
    TOME_RESOURCE_GOLD = 1,
    TOME_RESOURCE_LUMBER = 2,
    TOME_RESOURCE_FOOD_CAP = 4,   /* food provided (farms...), up to the ceiling */
    TOME_RESOURCE_FOOD_USED = 5
} tome_resource;

/* tome_event_info.key for TOME_EVENT_ARROW_KEY. */
typedef enum tome_arrow_key {
    TOME_ARROW_LEFT = 0,
    TOME_ARROW_RIGHT = 1,
    TOME_ARROW_DOWN = 2,
    TOME_ARROW_UP = 3
} tome_arrow_key;

/* tome_event_info.order_type for TOME_EVENT_UNIT_ORDER. */
typedef enum tome_order_type {
    TOME_ORDER_IMMEDIATE = 0,  /* no target: stop, hold position, train... */
    TOME_ORDER_POINT = 1,      /* a point: move, attack-move, build... */
    TOME_ORDER_TARGET = 2      /* a unit, item or destructable */
} tome_order_type;

/* tome_unit.flags (API version 8). */
typedef enum tome_unit_flag {
    TOME_UNIT_STRUCTURE = 1,  /* a building */
    TOME_UNIT_HERO = 2,
    TOME_UNIT_DEAD = 4
} tome_unit_flag;

/* A unit as the game has it right now (API version 8). */
typedef struct tome_unit {
    uint32_t size;      /* sizeof(tome_unit) as known to Tome */
    /* The game's unit object: the same unit has the same pointer for as
     * long as it exists, so it can be compared, but don't keep it. */
    void* object;
    /* The unit's JASS handle id, for natives, if the map script has one
     * for it (it does for every unit the script created or got from an
     * event or a group, once the native that created it returned: not yet
     * in TOME_EVENT_UNIT_CREATED), else 0. Tome never makes one: a handle
     * made on one client only would shift every later handle id there. */
    uint32_t handle;
    uint32_t type_id;   /* the unit type, e.g. 'hpea' (tome_type_code makes "hpea") */
    const char* name;   /* the type's name in the game's language, e.g. "Peasant" */
    int32_t owner;      /* the owner's player slot (0 = Player 1) */
    uint32_t flags;     /* tome_unit_flag */
} tome_unit;

/* tome_event_info.recipient for TOME_EVENT_CHAT. */
typedef enum tome_chat_recipient {
    TOME_CHAT_ALL = 0,
    TOME_CHAT_ALLIES = 1,
    TOME_CHAT_OBSERVERS = 2,  /* observers or referees */
    TOME_CHAT_PRIVATE = 3
} tome_chat_recipient;

typedef struct tome_event_info {
    uint32_t size;          /* sizeof(tome_event_info) as known to Tome */
    int32_t event;          /* tome_event */
    /* MAP_LOAD, GAME_START, GAME_END: the map, e.g. "Dustwallow Keys" and
     * "Maps\(10)DustwallowKeys.w3m". "" for TICK, or if the game doesn't say. */
    const char* map_name;
    const char* map_path;
    /* TICK: real seconds since the previous tick (0 for the first one).
     * GAME_TICK: the game time this step advances, in seconds. */
    double delta;
    /* TICK: the number of frames so far, counting from 1. */
    uint32_t frame;
    /* GAME_TICK, GAME_END: seconds of game time since the game started (or
     * a saved game was loaded), including this step. */
    double game_time;
    /* API version 8: check `size` before reading these. */
    /* MAP_LOAD, GAME_START, GAME_END: the map's description, "" if none. */
    const char* map_description;
    /* SELECTION: the selected units (at most 12, none if nothing is
     * selected) and the index in `units` of the active one, whose portrait
     * and commands show (Tab moves it), or -1. Valid during the call. */
    const tome_unit* units;
    uint32_t unit_count;
    int32_t leader;
    /* CHAT: the sender. PAUSE, RESUME: the player who paused or resumed,
     * or -1 for a trigger or the game itself. UNIT_OWNER_CHANGED: the
     * previous owner. A player slot (0 = Player 1);
     * player_name is "" for -1. */
    int32_t player;
    const char* player_name;
    /* CHAT: the message (UTF-8) and who it went to (tome_chat_recipient). */
    const char* text;
    int32_t recipient;
    /* Unit events: the unit, and the other unit involved (killer, trainer,
     * summoner, attacker, ... see each event), else NULL. Valid during the
     * call. */
    const tome_unit* unit;
    const tome_unit* other;
    /* ITEM_*, and an order on an item: the item (the game's object, valid
     * during the call). */
    void* item;
    /* The type an event is about (research, item, unit type being trained
     * or upgraded to, ability learned), e.g. 'Rhme' or 'pinv', and its name
     * in the game's language; else 0 and "". */
    uint32_t object_id;
    const char* object_name;
    /* HERO_LEVEL, HERO_XP: the hero's new level. HERO_SKILL: the skill's level. */
    int32_t level;
    /* GAME_SAVE, GAME_LOADED: the saved game, as the game names it. */
    const char* save_name;
    /* UNIT_SOLD: the buyer. */
    const tome_unit* third;
    /* UNIT_ORDER: the order id (e.g. 851986 for move; JASS OrderId2String
     * names them), its tome_order_type and its target point. */
    uint32_t order_id;
    int32_t order_type;
    double x;
    double y;
    /* API version 12: check `size` before reading these. */
    /* UNIT_DAMAGED: the damage. HERO_XP: the experience gained.
     * PLAYER_RESOURCE: the new value. DAY, NIGHT: the time of day. */
    double amount;
    /* A destructable an event is about (DESTRUCTABLE_DEATH; a spell's,
     * order's or acquired target): the game's object, valid during the
     * call, else NULL. */
    void* destructable;
    /* The JASS handle ids of `item` and `destructable`, if the map script
     * has one (see tome_unit.handle), else 0. */
    uint32_t item_handle;
    uint32_t destructable_handle;
    /* ARROW_KEY: the key (tome_arrow_key) and 1 if pressed, 0 if released. */
    int32_t key;
    int32_t pressed;
    /* DIALOG_CLICK: the dialog's and the clicked button's JASS handle ids. */
    uint32_t dialog;
    uint32_t button;
    /* PLAYER_RESOURCE: which one (tome_resource) and its value before. */
    int32_t resource;
    double previous;
    /* API version 13: check `size` before reading this. */
    /* MAP_LOAD, GAME_START, GAME_END: how many locals in the map script
     * have a global's name and another type, the trick memhack maps use to
     * read and write game memory (map_script_shadows lists them). Tome's
     * built-in guard keeps it from working, so a map that relies on it
     * doesn't load; a nonzero count only says the map tries it. */
    uint32_t script_shadows;
} tome_event_info;

typedef void (TOME_CALL *tome_event_fn)(tome_mod* mod, const tome_event_info* info, void* user);

/* API version 13: one local in the map script that has a global's name and
 * another type (see tome_event_info.script_shadows). The strings stay valid
 * until the next map loads. */
typedef struct tome_script_shadow {
    uint32_t size;            /* sizeof(tome_script_shadow) as filled by Tome */
    const char* name;         /* e.g. "bj_lastCreatedUnit" */
    const char* local_type;   /* the local's type, e.g. "integer" */
    const char* global_type;  /* the global's type, e.g. "unit" */
    int32_t line;             /* the local's line in war3map.j, from 1 */
} tome_script_shadow;

/* A chat command: `args` is the rest of the line after "/name", trimmed. */
typedef void (TOME_CALL *tome_command_fn)(tome_mod* mod, const char* args, void* user);

/*
 * Hooks (API version 4): run code when a mapped game function is called.
 * Tome owns the patch; every mod's handlers for the same function form one
 * chain, ordered by priority:
 *
 *   BEFORE handlers   may change `args`, or set `skip_original` and `result`
 *   REPLACE handlers  each runs in place of everything below it, and calls
 *                     hook_call_next to run the next REPLACE handler or,
 *                     after the last one, the game's function
 *   the game's function
 *   AFTER handlers    may read and change `result`
 *
 * Within each phase higher priorities run first (REPLACE: outermost), and
 * equal priorities run in the order they were registered. Handlers run on
 * whatever thread called the game function, usually the main thread.
 *
 * API version 9: imports can be hooked too ("Import.GetTickCount"): every
 * call game.dll makes through that import runs the chain, and the original
 * is the imported function. Calls from other DLLs aren't affected. Imports
 * like GetTickCount run very often and on many threads: keep handlers cheap
 * and thread-safe.
 */
typedef enum tome_hook_phase {
    TOME_HOOK_BEFORE = 1,
    TOME_HOOK_REPLACE = 2,
    TOME_HOOK_AFTER = 3
} tome_hook_phase;

typedef struct tome_hook_call {
    uint32_t size;               /* sizeof(tome_hook_call) as known to Tome */
    const char* name;            /* the hooked function, e.g. "CTextFrame::SetText" */
    /*
     * The arguments in the order of the mapping's signature, as 32-bit
     * words: `this` first for thiscall, floats as their bits
     * (tome_hook_arg_float). double and 64-bit integers take two words, low
     * half first (tome_hook_arg_double, tome_hook_arg_u64), so they shift
     * the index of every later argument by one. Changes made before the
     * game's function runs are what it receives.
     */
    uint32_t* args;
    int32_t arg_count;           /* words of the fixed arguments */
    uint32_t result;             /* eax: the return value */
    uint32_t result_high;        /* edx: the upper half of a 64-bit return value */
    /*
     * Set by a BEFORE handler to skip the REPLACE handlers and the game's
     * function; `result` (or `result_float`) is returned instead. Later
     * BEFORE handlers still run (and see it set), and so do the AFTER handlers.
     */
    int32_t skip_original;
    uintptr_t return_address;    /* where the game's function returns to */
    /* float and double results (st(0)) go here instead of `result`. */
    double result_float;
    /*
     * Variadic functions ("..." in the signature): Tome can't know how many
     * arguments the caller passed, so args[arg_count] onwards holds the next
     * `extra_count` words of the caller's stack, to read as the fixed
     * arguments describe (a format string, a count). They're passed on to
     * the game's function as they are then. 0 for other functions.
     */
    int32_t extra_count;
} tome_hook_call;

typedef void (TOME_CALL *tome_hook_fn)(tome_mod* mod, tome_hook_call* call, void* user);

/*
 * Settings and keybinds (API version 7).
 *
 * Settings: a mod describes them in a schema file named by tome.mod.json
 * "config" (e.g. "config.schema.json"); the mod menu generates a settings
 * page from it, and Tome keeps the values in tome/mods/config/<id>.json.
 * The schema is a small subset of JSON Schema:
 *
 *   { "type": "object",
 *     "properties": {
 *       "show":  { "type": "boolean", "title": "Show bars", "description": "...", "default": true },
 *       "scale": { "type": "number", "minimum": 0.5, "maximum": 2, "step": 0.1, "default": 1 },
 *       "count": { "type": "integer", "minimum": 0, "maximum": 10, "default": 3 },
 *       "limit": { "type": "integer", "minimum": 30, "maximum": 1000, "input": "text" },
 *       "mode":  { "type": "string", "enum": ["auto", "always"], "enumTitles": ["Auto", "Always"] },
 *       "name":  { "type": "string", "maxLength": 32, "default": "" } } }
 *
 * Integers and numbers get - and + buttons; "input": "text" gives them a box
 * to type the value in instead (it's rounded and kept in range). The menu
 * shows about three lines of each "description".
 *
 * Keybinds: tome.mod.json "keybinds" lists them,
 *   [{ "id": "toggle", "label": "Toggle bars", "default": "Ctrl+H" }]
 * and the player can change them on the mod menu's Keybinds page (saved in
 * tome/keybinds.json). Key names: modifiers Ctrl, Shift, Alt joined with
 * '+', then A-Z, 0-9, F1-F24, Num0-Num9, Space, Enter, Esc, Tab, Backspace,
 * Insert, Delete, Home, End, PageUp, PageDown, Left, Up, Right, Down, and
 * the punctuation keys; "None" is unbound. Tome's own "Open the mod menu"
 * (Ctrl+F10 by default) is listed with them.
 */
typedef void (TOME_CALL *tome_setting_fn)(tome_mod* mod, const char* key, void* user);
typedef void (TOME_CALL *tome_keybind_fn)(tome_mod* mod, const char* id, void* user);

/*
 * A popup (API version 10): a title, a message and buttons, shown over
 * whatever screen is up (main menu, other menus, games), one at a time.
 * Up to three short labels share a row; more stack in a column, six to a
 * page, with Previous and Next. Strings are UTF-8 and copied; the game's
 * color codes (|cffRRGGBB ... |r) work in them.
 */
typedef struct tome_popup {
    uint32_t size;               /* sizeof(tome_popup) */
    const char* title;           /* NULL or "" for none */
    const char* text;            /* the message; NULL or "" for none. '\n' starts a new line */
    const char* const* options;  /* the buttons' labels */
    int32_t option_count;        /* at least 1, as many as needed */
    int32_t default_option;      /* Enter picks it; -1 for none */
    int32_t cancel_option;       /* Esc picks it; -1 for none */
} tome_popup;

/* The choice a popup closed with when no button was picked: popup_close. */
#define TOME_POPUP_CLOSED (-1)

/* A service appeared (available 1) or went away (0); see on_service. */
typedef void (TOME_CALL *tome_service_fn)(tome_mod* mod, const char* name, int32_t available, void* user);

/* A message published on a topic; `data` is valid only during the call. */
typedef void (TOME_CALL *tome_message_fn)(tome_mod* mod, const char* topic, const void* data, size_t size,
                                          void* user);

/* `popup` is the id popup() gave; `choice` an index into options, or TOME_POPUP_CLOSED. */
typedef void (TOME_CALL *tome_popup_fn)(tome_mod* mod, uint32_t popup, int32_t choice, void* user);

typedef struct tome_api {
    uint32_t size;               /* sizeof(tome_api) as known to Tome */
    uint32_t api_version;        /* TOME_API_VERSION of the running Tome */
    const char* tome_version;    /* e.g. "0.1.0" */

    /* Writes one line to tome.log, tagged with the mod's id. */
    void (TOME_CALL *log)(tome_mod* mod, tome_log_level level, const char* message);

    const tome_game_info* (TOME_CALL *game_info)(void);

    const char* (TOME_CALL *mod_id)(tome_mod* mod);
    const char* (TOME_CALL *mod_version)(tome_mod* mod);
    const char* (TOME_CALL *mod_dir)(tome_mod* mod);    /* the mod's folder */
    /* tome/mods/config/<id>, created on demand: survives updates of the mod */
    const char* (TOME_CALL *config_dir)(tome_mod* mod);

    /*
     * Attaches the implementation of a JASS native. The native must be
     * declared in the mod's tome.mod.json "natives" list; Tome binds every
     * declared native into the engine when a map loads. Call this from
     * tome_mod_enable. While the mod is disabled the native returns 0 (null,
     * false, 0.0), so maps that use it keep running.
     */
    tome_result (TOME_CALL *register_native)(tome_mod* mod, const char* name, void* fn);

    /* --- API version 2 --- */

    /*
     * Looks a name up in the mapping database, e.g. "CTextFrame::SetText" or
     * "CFrame.layout". Returns NULL if the name isn't mapped on this build.
     * List the names a mod can't work without in tome.mod.json "requires":
     * Tome then refuses to load the mod on builds that lack them.
     */
    const tome_symbol* (TOME_CALL *resolve)(const char* name);

    /* --- API version 3: frames --- */

    /*
     * Calls `fn` on the main thread whenever `event` (a tome_ui_event)
     * happens, and right away (or as soon as Tome reaches the main thread) if
     * that screen already exists. Any thread. Revoked on disable. Before API
     * version 5, only TOME_UI_MAIN_MENU.
     */
    tome_result (TOME_CALL *on_ui)(tome_mod* mod, int32_t event, tome_ui_fn fn, void* user);

    /*
     * Loads frame definitions from an FDF file in the mod's folder (a relative
     * path like "ui/mymod.fdf"), on top of the game's own. Tome adds it to its
     * overlay archive under UI\FrameDef\Tome\<mod id>\, so IncludeFile works
     * as in the game's files. (FDF and TOC files in the mod's assets folder
     * are in the game's archives already; load those with load_toc.) Like the game, a name that's already defined keeps
     * its first definition: changes to a loaded file apply after a restart.
     */
    tome_result (TOME_CALL *load_fdf)(tome_mod* mod, const char* path);

    /* Loads a TOC file from the game's archives, like 1.31's BlzLoadTOCFile. */
    tome_result (TOME_CALL *load_toc)(const char* path);

    /* The frame registered under `name` and create context, or NULL. */
    tome_frame* (TOME_CALL *frame_get)(const char* name, int32_t context);

    /*
     * Creates a frame from a definition (FDF "Frame" name) as a child of
     * `parent`, which owns it. The frame and its children get their FDF names
     * with `context`, so give each copy of the same definition its own
     * context. Returns NULL if the definition doesn't exist, or if a frame
     * with that name and context exists already (the game can't tell two
     * apart, and crashes destroying them): get that one with frame_get
     * instead, e.g. after the mod was disabled and enabled again. Frames a
     * mod created are hidden when it's disabled.
     */
    tome_frame* (TOME_CALL *frame_create)(tome_mod* mod, const char* name, tome_frame* parent, int32_t priority,
                                          int32_t context);

    /* Anchors `point` of the frame to `relative_point` of `relative`, offset
     * by x, y (0.8 x 0.6 screen units, y up). Replaces that point's anchor. */
    tome_result (TOME_CALL *frame_set_point)(tome_frame* frame, int32_t point, tome_frame* relative,
                                             int32_t relative_point, float x, float y);

    /* Text frames (FDF "TEXT") only; for a text button, set its text child. */
    tome_result (TOME_CALL *frame_set_text)(tome_frame* frame, const char* text);

    /* Showing a frame also shows all its children. */
    tome_result (TOME_CALL *frame_set_visible)(tome_frame* frame, int32_t visible);

    /*
     * Plays the sequence of a sprite frame's model that matches a set of
     * animation tags, e.g. {stand, alternate}. Tag ids are build-specific:
     * resolve "AnimTag.<name>" (a CONSTANT) to get them.
     */
    tome_result (TOME_CALL *frame_set_animation)(tome_frame* sprite, const int32_t* tags, int32_t count);

    /*
     * Calls `fn` when a button (any control: FDF GLUEBUTTON, GLUETEXTBUTTON,
     * CHECKBOX, ...) is clicked, on any screen or in a game (before API
     * version 5: main menu buttons with context 0 only). Register again in
     * each on_ui callback, since the screen's buttons are new each time.
     * Registering the same button again replaces its callback. Revoked on
     * disable.
     */
    tome_result (TOME_CALL *frame_on_click)(tome_mod* mod, tome_frame* button, tome_click_fn fn, void* user);

    /* --- API version 4: hooks --- */

    /*
     * Adds `fn` to the chain of the mapped function `name` (a FUNCTION in the
     * mapping database, e.g. "CTextFrame::SetText", or since API version 9 an
     * IMPORT) in `phase` (a
     * tome_hook_phase). Any thread; takes effect for calls that start
     * afterwards. Revoked on disable; the mod's DLL is only freed once none
     * of its handlers is running. Fails with TOME_ERR_UNAVAILABLE if the
     * name isn't mapped, or if Tome can't hook that function (see tome.log:
     * e.g. an instruction Tome can't move). List the names a mod hooks in
     * tome.mod.json "requires". A few functions are reserved by Tome's own
     * built-in patches (security fixes such as the JASS memhack guard); hook
     * and unhook refuse them with TOME_ERR_NOT_OWNER so a mod can't disable a
     * fix.
     */
    tome_result (TOME_CALL *hook)(tome_mod* mod, const char* name, int32_t phase, int32_t priority, tome_hook_fn fn,
                                  void* user);

    /* Removes a handler added with the same name, phase, fn and user. */
    tome_result (TOME_CALL *unhook)(tome_mod* mod, const char* name, int32_t phase, tome_hook_fn fn, void* user);

    /*
     * From a REPLACE handler: runs the rest of the chain (lower REPLACE
     * handlers, then the game's function) with the current `args`, and
     * stores its return value in `result` (or `result_float`). Not calling
     * it skips them. Can be
     * called more than once. Anywhere else it does nothing.
     */
    void (TOME_CALL *hook_call_next)(tome_hook_call* call);

    /* --- API version 5: events, chat commands, screens --- */

    /* Calls `fn` whenever `event` (a tome_event) happens. Any thread. Revoked on disable. */
    tome_result (TOME_CALL *on_event)(tome_mod* mod, int32_t event, tome_event_fn fn, void* user);

    /*
     * Adds a chat command: when the player sends "/name ..." in the in-game
     * chat, `fn` runs (main thread) and the line isn't sent to anyone. Names
     * are 1-32 characters from a-z, 0-9, '_' and '-', compared ignoring
     * case. Fails with TOME_ERR_INVALID_ARGUMENT for a bad name, one of the
     * game's own commands or "tome", and TOME_ERR_NOT_OWNER if another mod
     * has it. Any thread. Revoked on disable.
     */
    tome_result (TOME_CALL *register_command)(tome_mod* mod, const char* name, tome_command_fn fn, void* user);

    /*
     * Shows a message to the local player in the game's message area for
     * `seconds`, like DisplayTimedTextToPlayer. In a game only
     * (TOME_ERR_UNAVAILABLE elsewhere). Main thread.
     */
    tome_result (TOME_CALL *display_text)(const char* text, float seconds);

    /*
     * The root frame of a screen (a tome_ui_event) while it's showing, else
     * NULL: the menu screen's own frame, or for TOME_UI_GAME the in-game UI.
     * A good parent for frame_create. Main thread.
     */
    tome_frame* (TOME_CALL *ui_root)(int32_t ui_event);

    /* --- API version 6: JASS strings and code --- */

    /*
     * The UTF-8 text of a string argument of a native, or NULL for a null
     * string. It belongs to the game and may go away once the native
     * returns: copy it to keep it. Any thread, while the native runs.
     */
    const char* (TOME_CALL *jass_string)(tome_jass_string value);

    /*
     * Makes the value a native returns for `text` (UTF-8; invalid sequences
     * become U+FFFD), as the game's own string natives do. Use it only as
     * that return value, from inside the native. Main thread, while a map
     * script runs; returns 0 (a null string) otherwise or for NULL.
     */
    tome_jass_string_ret (TOME_CALL *jass_string_create)(const char* text);

    /*
     * Runs a `code` argument (a function of the map script) now, like
     * TriggerEvaluate runs a condition, and stores the boolean it returns in
     * *result (0 for a function that returns nothing; result may be NULL).
     * Works from a native, and later from events, clicks and chat commands.
     * As in a condition, the function can't wait; use jass_code_execute for
     * one that does.
     *
     * Code values are indices into the map script's own table, so one from
     * an earlier map would name a different function. Tome only accepts
     * values the current map script passed to a Tome native (any mod's) and
     * fails with TOME_ERR_INVALID_ARGUMENT for others, e.g. after the map
     * the value came from ended. Main thread (TOME_ERR_WRONG_THREAD), while
     * a map script runs (TOME_ERR_UNAVAILABLE).
     */
    tome_result (TOME_CALL *jass_code_call)(tome_jass_code code, int32_t* result);

    /*
     * Runs a `code` argument like TriggerExecute runs an action: on a JASS
     * thread of its own, which may wait (TriggerSleepAction, PolledWait).
     * Returns once the function finishes or first waits; the rest runs
     * later, after the wait. There's no result. The same rules for code
     * values and threads as jass_code_call.
     */
    tome_result (TOME_CALL *jass_code_execute)(tome_jass_code code);

    /* --- API version 7: settings and keybinds --- */

    /*
     * The current value of a setting from the mod's schema, or `fallback`
     * if the schema has no such setting or it has another type. Integers
     * and numbers convert to each other (rounded). Any thread.
     */
    int32_t (TOME_CALL *setting_bool)(tome_mod* mod, const char* key, int32_t fallback);
    int32_t (TOME_CALL *setting_int)(tome_mod* mod, const char* key, int32_t fallback);
    double (TOME_CALL *setting_number)(tome_mod* mod, const char* key, double fallback);
    /*
     * Copies a string setting (UTF-8, NUL-terminated, cut to fit) into
     * `buffer` and returns its full length in bytes, like snprintf; 0 and an
     * empty buffer if there's no such string setting. `buffer` may be NULL
     * when `size` is 0, to ask for the length.
     */
    size_t (TOME_CALL *setting_string)(tome_mod* mod, const char* key, char* buffer, size_t size);

    /*
     * Change a setting as if the player had, and save it: numbers are
     * clamped to the schema's range and rounded for integers, strings cut
     * to maxLength. TOME_ERR_INVALID_ARGUMENT for an unknown setting, the
     * wrong type, or a string that isn't one of the choices. Any thread.
     */
    tome_result (TOME_CALL *set_setting_bool)(tome_mod* mod, const char* key, int32_t value);
    tome_result (TOME_CALL *set_setting_int)(tome_mod* mod, const char* key, int32_t value);
    tome_result (TOME_CALL *set_setting_number)(tome_mod* mod, const char* key, double value);
    tome_result (TOME_CALL *set_setting_string)(tome_mod* mod, const char* key, const char* value);

    /*
     * Calls `fn` on the main thread with the key of each setting that
     * changes: in the mod menu, through set_setting_*, or in its settings
     * file on disk. Any thread. Revoked on disable.
     */
    tome_result (TOME_CALL *on_setting_changed)(tome_mod* mod, tome_setting_fn fn, void* user);

    /*
     * Calls `fn` on the main thread when the player presses the key bound
     * to `id` (an entry of tome.mod.json "keybinds"), in menus and in games,
     * except while typing in the chat box or another text box. The key
     * press doesn't reach the game then. Registering the same id again
     * replaces the callback. TOME_ERR_NOT_DECLARED if the mod didn't declare
     * `id`. Any thread. Revoked on disable.
     */
    tome_result (TOME_CALL *on_keybind)(tome_mod* mod, const char* id, tome_keybind_fn fn, void* user);

    /*
     * The key bound to `id` right now ("Ctrl+H", or "None"), copied like
     * setting_string; 0 if the mod didn't declare `id`. For showing the
     * key in the mod's own UI. Any thread.
     */
    size_t (TOME_CALL *keybind_name)(tome_mod* mod, const char* id, char* buffer, size_t size);

    /* --- API version 8: units --- */

    /*
     * The local player's current selection: fills up to `max` units
     * (`unit_size` is sizeof(tome_unit); use tome_get_selection), sets
     * *leader (may be NULL) to the active unit's index or -1, and returns
     * how many units are selected (possibly more than `max`). 0 outside a
     * game. Main thread; the names stay valid until the game ends.
     */
    int32_t (TOME_CALL *selection)(tome_unit* units, uint32_t unit_size, int32_t max, int32_t* leader);

    /* --- API version 10: popups, restarting --- */

    /*
     * Shows a popup, or queues it behind the ones already waiting (any
     * mod's or Tome's). It stays until the player picks a button; then `fn`
     * (may be NULL) runs on the main thread with the choice. While it shows,
     * it takes the mouse and the keyboard (except Alt combinations), and a
     * game keeps running: single player isn't paused for it. If the screen
     * changes, it moves to the next one; the loading screen has none.
     * `*id` (may be NULL) gets an id for popup_close. Any thread.
     * TOME_ERR_INVALID_ARGUMENT for no options or an index out of range;
     * TOME_ERR_UNAVAILABLE before the game's UI is ready.
     * A disabled mod's popups go away without calling `fn`.
     */
    tome_result (TOME_CALL *popup)(tome_mod* mod, const tome_popup* popup, tome_popup_fn fn, void* user,
                                   uint32_t* id);

    /*
     * Takes away one of the mod's popups, shown or waiting; its `fn` runs
     * with TOME_POPUP_CLOSED. TOME_ERR_INVALID_ARGUMENT if it's already
     * gone or another mod's. Any thread.
     */
    tome_result (TOME_CALL *popup_close)(tome_mod* mod, uint32_t id);

    /*
     * Restarts the game: closes it as if the player had (a game in
     * progress is left, every mod is disabled and unloaded), then starts it
     * again with the same command line. Ask the player first, e.g. with a
     * popup. Any thread; TOME_ERR_UNAVAILABLE without a game window.
     */
    tome_result (TOME_CALL *restart_game)(tome_mod* mod);

    /* --- API version 11: services and messages between mods --- */

    /*
     * Offers a table of functions (any struct the mod defines and keeps
     * alive until it is disabled) to other mods under `name`: 1 to 64
     * characters of a-z, 0-9, '.', '-', '_', by convention starting with
     * the mod's id ("hp-bars.api"). `version` is a compatibility level the
     * provider raises when it changes the table incompatibly; give it a
     * struct size or a first field `size` for compatible additions.
     * TOME_ERR_INVALID_ARGUMENT for a bad name or NULL table,
     * TOME_ERR_NOT_OWNER if another mod offers `name`,
     * TOME_ERR_WRONG_THREAD off the main thread. Withdrawn on disable
     * (listeners from on_service are told afterwards, with service_get already returning NULL).
     */
    tome_result (TOME_CALL *service_register)(tome_mod* mod, const char* name, uint32_t version, const void* table);

    /* Withdraws one of the mod's services. TOME_ERR_INVALID_ARGUMENT if it isn't its own. Main thread. */
    tome_result (TOME_CALL *service_unregister)(tome_mod* mod, const char* name);

    /*
     * The table offered under `name`, or NULL if no enabled mod offers it
     * or its version isn't `version` (0 accepts any). It stays valid until
     * the provider withdraws it or is disabled: a mod that depends on the
     * provider ("depends" in tome.mod.json) is disabled first and may keep
     * the pointer; any other mod must drop it when on_service says 0.
     * Calls through the table run on the caller's thread and aren't
     * guarded for the provider's crashes. Main thread.
     */
    const void* (TOME_CALL *service_get)(tome_mod* mod, const char* name, uint32_t version);

    /*
     * Calls `fn` on the main thread when the service `name` appears or goes
     * away, in whatever order the mods load; at once if it exists already.
     * Revoked on disable. Main thread.
     */
    tome_result (TOME_CALL *on_service)(tome_mod* mod, const char* name, tome_service_fn fn, void* user);

    /*
     * Calls `fn` on the main thread for every message published on `topic`
     * (same name rules as services) by another mod, in subscription order,
     * until the mod is disabled. Main thread.
     */
    tome_result (TOME_CALL *subscribe)(tome_mod* mod, const char* topic, tome_message_fn fn, void* user);

    /*
     * Sends `size` bytes to the other mods' subscribers of `topic`, one at
     * a time, before returning (a subscriber that crashes is disabled and
     * skipped). It is not copied: define a stable layout and
     * keep it small. Returns the number of subscribers reached, or 0 for
     * none or an error. Subscribers must not publish the same topic again
     * from their callback. Main thread.
     */
    int32_t (TOME_CALL *publish)(tome_mod* mod, const char* topic, const void* data, size_t size);

    /* --- API version 13: the map script's memhack pattern --- */

    /*
     * The current map's locals that have a global's name and another type
     * (tome_event_info.script_shadows): fills up to `max` (`item_size` is
     * sizeof(tome_script_shadow); use tome_get_map_script_shadows) and
     * returns how many there are (possibly more than `max`). Globals of
     * common.j and Blizzard.j count too. 0 before the first map load.
     * Main thread.
     */
    int32_t (TOME_CALL *map_script_shadows)(tome_script_shadow* out, uint32_t item_size, int32_t max);

    /* --- API version 14: the game's files --- */

    /*
     * Reads a file as the game would, through its archives and Tome's
     * asset overlay (highest priority first), e.g. "Units\\HumanUnitFunc.txt":
     * copies up to `size` bytes into `buffer` and returns the file's whole
     * size, like setting_string (no terminating NUL); 0 if there's no such
     * file or it's empty. `buffer` may be NULL when `size` is 0, to ask for
     * the size. Main thread.
     */
    size_t (TOME_CALL *read_game_file)(const char* name, void* buffer, size_t size);
} tome_api;

/*
 * Lifecycle, called by Tome on the game's main thread (once the main-thread
 * hook exists; see docs/research.md §5.6).
 *
 *   tome_mod_load     once after the DLL is loaded. Return 0 on success; any
 *                     other value means the mod failed and it stays unloaded.
 *   tome_mod_enable   register handlers here. Can run many times.
 *   tome_mod_disable  undo effects on the game that Tome can't undo for you
 *                     (units spawned, values changed). Tome revokes the mod's
 *                     registrations itself.
 *   tome_mod_unload   free memory before the DLL is unloaded.
 *
 * When the program exits normally, every running mod is disabled (dependents
 * first), then every loaded mod gets tome_mod_unload; the DLL itself isn't
 * freed then, since the game keeps tearing itself down afterwards. A killed
 * or crashed process gets neither.
 *
 * Crashes: every call Tome makes into a mod (these four, natives, hook
 * handlers, event, UI, click and command callbacks) runs under a guard. If
 * the mod faults there (access violation, division by zero, stack overflow,
 * an uncaught C++ exception, ...), Tome abandons the call (a native returns
 * 0; a REPLACE handler that hadn't called hook_call_next is skipped), logs
 * where it happened, stops calling the mod and disables it with its
 * dependents. tome_mod_disable and tome_mod_unload still run, so a mod can
 * undo what Tome can't; the DLL stays loaded for the rest of the session.
 * The mod stays off until its DLL changes, or the user disables and enables
 * it again. Threads a mod starts itself aren't guarded.
 *
 * A mod that catches its own failure (a Rust panic, say) and logs why can
 * hand it to the guard the same way: RaiseException(TOME_EXCEPTION_MOD_FAILED,
 * EXCEPTION_NONCONTINUABLE, 0, NULL) from the callback Tome called.
 *
 * tome_mod_load is required; the other three are optional.
 */
#define TOME_EXCEPTION_MOD_FAILED 0xE0544D46u
typedef int (TOME_CALL *tome_mod_load_fn)(const tome_api* api, tome_mod* mod);
typedef void (TOME_CALL *tome_mod_lifecycle_fn)(tome_mod* mod);

#ifdef __cplusplus
}
#endif

#include <string.h>

#include "utf8.h"

static inline tome_jass_real_ret tome_jass_real_bits(float value) {
    tome_jass_real_ret bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/*
 * The address (or value) of a mapped name, or 0 if it isn't mapped or the
 * running Tome predates resolve.
 */
static inline uintptr_t tome_resolve(const tome_api* api, const char* name) {
    const tome_symbol* sym;
    if (api->size < offsetof(tome_api, resolve) + sizeof(api->resolve) || !api->resolve) return 0;
    sym = api->resolve(name);
    return sym ? sym->value : 0;
}

/* True if the running Tome has the frame API (version 3). */
static inline int tome_has_frames(const tome_api* api) {
    return api->api_version >= 3 && api->size >= offsetof(tome_api, frame_on_click) + sizeof(api->frame_on_click);
}

/* True if the running Tome has hooks (version 4). */
static inline int tome_has_hooks(const tome_api* api) {
    return api->api_version >= 4 && api->size >= offsetof(tome_api, hook_call_next) + sizeof(api->hook_call_next);
}

/* A float argument of a hooked function, and setting one. */
static inline float tome_hook_arg_float(const tome_hook_call* call, int index) {
    float value;
    memcpy(&value, &call->args[index], sizeof(value));
    return value;
}

static inline void tome_hook_set_arg_float(tome_hook_call* call, int index, float value) {
    memcpy(&call->args[index], &value, sizeof(value));
}

/* A double or 64-bit integer argument: two words starting at `index`. */
static inline double tome_hook_arg_double(const tome_hook_call* call, int index) {
    double value;
    memcpy(&value, &call->args[index], sizeof(value));
    return value;
}

static inline void tome_hook_set_arg_double(tome_hook_call* call, int index, double value) {
    memcpy(&call->args[index], &value, sizeof(value));
}

static inline uint64_t tome_hook_arg_u64(const tome_hook_call* call, int index) {
    return (uint64_t)call->args[index] | (uint64_t)call->args[index + 1] << 32;
}

static inline void tome_hook_set_arg_u64(tome_hook_call* call, int index, uint64_t value) {
    call->args[index] = (uint32_t)value;
    call->args[index + 1] = (uint32_t)(value >> 32);
}

/* True if the running Tome has events and chat commands (version 5). */
static inline int tome_has_events(const tome_api* api) {
    return api->api_version >= 5 && api->size >= offsetof(tome_api, ui_root) + sizeof(api->ui_root);
}

/* True if the running Tome has JASS strings and code (version 6). */
static inline int tome_has_jass_values(const tome_api* api) {
    return api->api_version >= 6 &&
           api->size >= offsetof(tome_api, jass_code_execute) + sizeof(api->jass_code_execute);
}

/* True if the running Tome has settings and keybinds (version 7). */
static inline int tome_has_settings(const tome_api* api) {
    return api->api_version >= 7 && api->size >= offsetof(tome_api, keybind_name) + sizeof(api->keybind_name);
}

/* True if the running Tome has the gameplay events and selection() (version 8). */
static inline int tome_has_units(const tome_api* api) {
    return api->api_version >= 8 && api->size >= offsetof(tome_api, selection) + sizeof(api->selection);
}

/* True if the running Tome has the version 12 events (spells, damage, keys, dialogs...);
 * on_event refuses them before. */
static inline int tome_has_more_events(const tome_api* api) {
    return api->api_version >= 12;
}

/* True if the running Tome can hook imports (version 9). hook() is older, so check this first. */
static inline int tome_has_import_hooks(const tome_api* api) {
    return api->api_version >= 9;
}

/* True if the running Tome has popups and restart_game (version 10). */
static inline int tome_has_popups(const tome_api* api) {
    return api->api_version >= 10 && api->size >= offsetof(tome_api, restart_game) + sizeof(api->restart_game);
}

/* True if the running Tome has services and messages between mods (version 11). */
static inline int tome_has_services(const tome_api* api) {
    return api->api_version >= 11 && api->size >= offsetof(tome_api, publish) + sizeof(api->publish);
}

/* True if the running Tome reports the map script's memhack pattern (version 13). */
static inline int tome_has_script_shadows(const tome_api* api) {
    return api->api_version >= 13 &&
           api->size >= offsetof(tome_api, map_script_shadows) + sizeof(api->map_script_shadows);
}

static inline int32_t tome_get_map_script_shadows(const tome_api* api, tome_script_shadow* out, int32_t max) {
    return api->map_script_shadows(out, (uint32_t)sizeof(tome_script_shadow), max);
}

static inline int32_t tome_get_selection(const tome_api* api, tome_unit* units, int32_t max, int32_t* leader) {
    return api->selection(units, (uint32_t)sizeof(tome_unit), max, leader);
}

/* A unit type id as its four characters: 'hpea' -> "hpea". `out` holds 5. */
static inline const char* tome_type_code(uint32_t type_id, char out[5]) {
    out[0] = (char)(type_id >> 24);
    out[1] = (char)(type_id >> 16);
    out[2] = (char)(type_id >> 8);
    out[3] = (char)type_id;
    out[4] = '\0';
    return out;
}

/* Convenience printf-style logging, formatted on the mod's side. */
#ifndef TOME_NO_LOGF
#include <stdarg.h>
#include <stdio.h>
static inline void tome_logf(const tome_api* api, tome_mod* mod, tome_log_level level, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    api->log(mod, level, buf);
}
#endif

#endif /* TOME_TOME_H */
