/* SPDX-License-Identifier: MIT-0 */
/*
 * Tome for WebAssembly mods (experimental): C for clang --target=wasm32,
 * no libc. A Wasm mod runs sandboxed: it can only call these functions, and
 * Tome copies what it hands the mod into the mod's own memory. Game objects
 * (frames, units) are handles, never pointers.
 *
 * Build with tome_add_wasm_mod() (TomeMod.cmake), which compiles
 * support/tome_wasm.c (the runtime: Tome's callbacks into the mod, and the
 * memcpy/memset/strlen the compiler may call) into the module. In
 * tome.mod.json, "wasm": "<name>.wasm" instead of "entry".
 *
 * A mod defines tome_mod_load (and optionally tome_mod_enable,
 * tome_mod_disable, tome_mod_unload); this header exports them:
 *
 *   #include <tome/tome_wasm.h>
 *
 *   static void hello(const char* args, void* user) { tome_log(TOME_LOG_INFO, args); }
 *
 *   int tome_mod_load(void) { return 0; }
 *   void tome_mod_enable(void) { tome_register_command("hello", hello, 0); }
 *
 * Callbacks run on the game's main thread, one at a time, like a DLL mod's.
 * A trap (an out-of-bounds access, unreachable, running past the
 * instruction budget, about half a second of work per call) disables the
 * mod. The functions mirror tome.h's of the same name; see there for what
 * each does. What's different here is said here.
 */
#pragma once

#if !defined(__wasm32__)
#error "tome_wasm.h is for WebAssembly mods (clang --target=wasm32); DLL mods use tome.h"
#endif

#include <stddef.h>
#include <stdint.h>

#include "tome_wasm_consts.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A frame (0 is none): Tome looks it up again on every use, so a handle to
 * a frame that's gone just fails (TOME_ERR_INVALID_ARGUMENT). */
typedef uint32_t tome_frame_t;

/* tome_unit, with handles: `object` tells units apart (the same unit has
 * the same one while it exists), `handle` is its JASS handle id or 0. */
typedef struct tome_wasm_unit {
    uint32_t size;
    uint32_t object;
    uint32_t handle;
    uint32_t type_id;
    const char* name;
    int32_t owner;
    uint32_t flags;      /* tome_unit_flag */
} tome_wasm_unit;

/* tome_event_info, with the same fields; `item` and `destructable` are
 * handles like tome_wasm_unit.object. Valid during the callback. */
typedef struct tome_wasm_event {
    uint32_t size;
    int32_t event;       /* tome_event */
    const char* map_name;
    const char* map_path;
    double delta;
    uint32_t frame;
    double game_time;
    const char* map_description;
    const tome_wasm_unit* units;
    uint32_t unit_count;
    int32_t leader;
    int32_t player;
    const char* player_name;
    const char* text;
    int32_t recipient;
    const tome_wasm_unit* unit;
    const tome_wasm_unit* other;
    uint32_t item;
    uint32_t object_id;
    const char* object_name;
    int32_t level;
    const char* save_name;
    const tome_wasm_unit* third;
    uint32_t order_id;
    int32_t order_type;
    double x;
    double y;
    double amount;
    uint32_t destructable;
    uint32_t item_handle;
    uint32_t destructable_handle;
    int32_t key;
    int32_t pressed;
    uint32_t dialog;
    uint32_t button;
    int32_t resource;
    double previous;
    uint32_t script_shadows;
} tome_wasm_event;

typedef struct tome_wasm_game_info {
    uint32_t size;
    const char* build_name;    /* e.g. "1.27b" */
    const char* file_version;  /* e.g. "1.27.1.7085"; no game_base: an address */
    /* API version 15: check `size` (16) before reading this. */
    int32_t edition;           /* TOME_EDITION_* */
} tome_wasm_game_info;

typedef struct tome_wasm_script_shadow {
    uint32_t size;
    const char* name;
    const char* local_type;
    const char* global_type;
    int32_t line;
} tome_wasm_script_shadow;

/* Tome writes these at fixed offsets (loader/src/core/wasm_layout.cpp). */
#define TOME_WASM_CHECK(type, field, offset) _Static_assert(offsetof(type, field) == (offset), #type "." #field)
TOME_WASM_CHECK(tome_wasm_unit, name, 16);
TOME_WASM_CHECK(tome_wasm_unit, flags, 24);
_Static_assert(sizeof(tome_wasm_unit) == 28, "tome_wasm_unit");
TOME_WASM_CHECK(tome_wasm_event, delta, 16);
TOME_WASM_CHECK(tome_wasm_event, game_time, 32);
TOME_WASM_CHECK(tome_wasm_event, units, 44);
TOME_WASM_CHECK(tome_wasm_event, player, 56);
TOME_WASM_CHECK(tome_wasm_event, unit, 72);
TOME_WASM_CHECK(tome_wasm_event, item, 80);
TOME_WASM_CHECK(tome_wasm_event, third, 100);
TOME_WASM_CHECK(tome_wasm_event, x, 112);
TOME_WASM_CHECK(tome_wasm_event, amount, 128);
TOME_WASM_CHECK(tome_wasm_event, destructable, 136);
TOME_WASM_CHECK(tome_wasm_event, previous, 168);
TOME_WASM_CHECK(tome_wasm_event, script_shadows, 176);
_Static_assert(sizeof(tome_wasm_event) == 184, "tome_wasm_event");
TOME_WASM_CHECK(tome_wasm_game_info, edition, 12);
_Static_assert(sizeof(tome_wasm_game_info) == 16, "tome_wasm_game_info");
TOME_WASM_CHECK(tome_wasm_script_shadow, line, 16);
_Static_assert(sizeof(tome_wasm_script_shadow) == 20, "tome_wasm_script_shadow");
#undef TOME_WASM_CHECK

/* --- the mod's exports ------------------------------------------------- */

#define TOME_WASM_EXPORT(name) __attribute__((export_name(#name)))
TOME_WASM_EXPORT(tome_mod_load) int tome_mod_load(void);  /* required: 0 is success */
TOME_WASM_EXPORT(tome_mod_enable) void tome_mod_enable(void);
TOME_WASM_EXPORT(tome_mod_disable) void tome_mod_disable(void);
TOME_WASM_EXPORT(tome_mod_unload) void tome_mod_unload(void);

/* --- Tome's imports, as Tome provides them ------------------------------
 * Strings are (pointer, length) UTF-8 without NUL bytes, at most 4096 bytes.
 * Out-parameters are (pointer, capacity) and return the full length (no
 * terminator; UINT32_MAX for a bad buffer). The wrappers below are easier. */

#define TOME_WASM_IMPORT(name) __attribute__((import_module("tome"), import_name(#name)))
TOME_WASM_IMPORT(log) void tome_raw_log(int32_t level, const char* text, uint32_t len);
TOME_WASM_IMPORT(mod_id) uint32_t tome_raw_mod_id(char* out, uint32_t capacity);
TOME_WASM_IMPORT(mod_version) uint32_t tome_raw_mod_version(char* out, uint32_t capacity);
TOME_WASM_IMPORT(game_info) uint32_t tome_raw_game_info(void* out, uint32_t capacity);
TOME_WASM_IMPORT(register_command) int32_t tome_raw_register_command(const char* name, uint32_t len, uint32_t callback);
TOME_WASM_IMPORT(display_text) int32_t tome_raw_display_text(const char* text, uint32_t len, float seconds);
TOME_WASM_IMPORT(setting_bool) int32_t tome_raw_setting_bool(const char* key, uint32_t len, int32_t fallback);
TOME_WASM_IMPORT(setting_int) int32_t tome_raw_setting_int(const char* key, uint32_t len, int32_t fallback);
TOME_WASM_IMPORT(setting_number) double tome_raw_setting_number(const char* key, uint32_t len, double fallback);
TOME_WASM_IMPORT(setting_string) uint32_t tome_raw_setting_string(const char* key, uint32_t len, char* out, uint32_t capacity);
TOME_WASM_IMPORT(set_setting_bool) int32_t tome_raw_set_setting_bool(const char* key, uint32_t len, int32_t value);
TOME_WASM_IMPORT(set_setting_int) int32_t tome_raw_set_setting_int(const char* key, uint32_t len, int32_t value);
TOME_WASM_IMPORT(set_setting_number) int32_t tome_raw_set_setting_number(const char* key, uint32_t len, double value);
TOME_WASM_IMPORT(set_setting_string) int32_t tome_raw_set_setting_string(const char* key, uint32_t len, const char* value, uint32_t vlen);
TOME_WASM_IMPORT(on_setting_changed) int32_t tome_raw_on_setting_changed(uint32_t callback);
TOME_WASM_IMPORT(on_event) int32_t tome_raw_on_event(int32_t event, uint32_t callback);
TOME_WASM_IMPORT(on_ui) int32_t tome_raw_on_ui(int32_t event, uint32_t callback);
TOME_WASM_IMPORT(ui_root) tome_frame_t tome_raw_ui_root(int32_t event);
TOME_WASM_IMPORT(frame_get) tome_frame_t tome_raw_frame_get(const char* name, uint32_t len, int32_t context);
TOME_WASM_IMPORT(frame_create) tome_frame_t tome_raw_frame_create(const char* name, uint32_t len, tome_frame_t parent, int32_t priority, int32_t context);
TOME_WASM_IMPORT(frame_set_point) int32_t tome_frame_set_point(tome_frame_t frame, int32_t point, tome_frame_t relative, int32_t relative_point, float x, float y);
TOME_WASM_IMPORT(frame_set_text) int32_t tome_raw_frame_set_text(tome_frame_t frame, const char* text, uint32_t len);
TOME_WASM_IMPORT(frame_set_visible) int32_t tome_frame_set_visible(tome_frame_t frame, int32_t visible);
TOME_WASM_IMPORT(frame_set_animation) int32_t tome_frame_set_animation(tome_frame_t sprite, const int32_t* tags, int32_t count);
TOME_WASM_IMPORT(frame_on_click) int32_t tome_raw_frame_on_click(tome_frame_t button, uint32_t callback);
TOME_WASM_IMPORT(load_fdf) int32_t tome_raw_load_fdf(const char* path, uint32_t len);
TOME_WASM_IMPORT(load_toc) int32_t tome_raw_load_toc(const char* path, uint32_t len);
TOME_WASM_IMPORT(on_keybind) int32_t tome_raw_on_keybind(const char* id, uint32_t len, uint32_t callback);
TOME_WASM_IMPORT(keybind_name) uint32_t tome_raw_keybind_name(const char* id, uint32_t len, char* out, uint32_t capacity);
TOME_WASM_IMPORT(popup) int32_t tome_raw_popup(const void* spec, uint32_t callback, uint32_t* id);
TOME_WASM_IMPORT(popup_close) int32_t tome_popup_close(uint32_t id);
TOME_WASM_IMPORT(restart_game) int32_t tome_restart_game(void);
TOME_WASM_IMPORT(selection) uint32_t tome_raw_selection(void* out, uint32_t capacity, uint32_t* count, int32_t* leader);
TOME_WASM_IMPORT(subscribe) int32_t tome_raw_subscribe(const char* topic, uint32_t len, uint32_t callback);
TOME_WASM_IMPORT(publish) int32_t tome_raw_publish(const char* topic, uint32_t len, const void* data, uint32_t size);
TOME_WASM_IMPORT(map_script_shadows) uint32_t tome_raw_map_script_shadows(void* out, uint32_t capacity, uint32_t* count);
TOME_WASM_IMPORT(read_game_file) uint32_t tome_raw_read_game_file(const char* name, uint32_t len, void* out, uint32_t capacity);
TOME_WASM_IMPORT(config_read) uint32_t tome_raw_config_read(const char* name, uint32_t len, void* out, uint32_t capacity);
TOME_WASM_IMPORT(config_write) int32_t tome_raw_config_write(const char* name, uint32_t len, const void* data, uint32_t size);
TOME_WASM_IMPORT(register_native) int32_t tome_raw_register_native(const char* name, uint32_t len, uint32_t callback);
TOME_WASM_IMPORT(jass_code_call) int32_t tome_jass_code_call(uint32_t code, int32_t* result);
TOME_WASM_IMPORT(jass_code_execute) int32_t tome_jass_code_execute(uint32_t code);
/* API version 16: game objects. Units, items, destructables and players are
 * handles like tome_wasm_unit.object; property ids are tome.h's
 * tome_property (TOME_PROPERTY_*), values doubles. jass_call (below) takes
 * JASS handle ids (tome_handle_of), not these handles. */
TOME_WASM_IMPORT(object_type) int32_t tome_object_type_of(uint32_t object);
TOME_WASM_IMPORT(object_of) uint32_t tome_object_of(uint32_t jass_handle, int32_t* type);
TOME_WASM_IMPORT(handle_of) uint32_t tome_handle_of(uint32_t object);
TOME_WASM_IMPORT(player_object) uint32_t tome_player_object(int32_t slot);
TOME_WASM_IMPORT(unit_info) uint32_t tome_raw_unit_info(uint32_t unit, void* out, uint32_t capacity);
TOME_WASM_IMPORT(get_property) int32_t tome_get_property(uint32_t object, int32_t property, double* value);
TOME_WASM_IMPORT(set_property) int32_t tome_set_property(uint32_t object, int32_t property, double value);
TOME_WASM_IMPORT(get_keyed_property) int32_t tome_get_keyed_property(uint32_t object, int32_t property, uint32_t key, double* value);
TOME_WASM_IMPORT(set_keyed_property) int32_t tome_set_keyed_property(uint32_t object, int32_t property, uint32_t key, double value);
/* Every unit, item, destructable or player (TOME_OBJECT_*) as handles, up to `max`; how many there are, -1 if not. */
TOME_WASM_IMPORT(list_objects) int32_t tome_list_objects(int32_t type, uint32_t* out, int32_t max);
/* One argument or the result of jass_call: the member the native's signature
 * says. Handles are JASS handle ids here (tome_handle_of), not this module's
 * object handles; strings are UTF-8. */
typedef union tome_jass_value {
    int32_t integer;
    int32_t boolean;
    float real;
    uint32_t handle;
    uint32_t code;
    const char* string;
} tome_jass_value;
_Static_assert(sizeof(tome_jass_value) == 4, "one word per argument");
/* Runs a JASS native by name, as tome.h's jass_call (with the same checks); a string result is
 * copied into `text` (cut to `capacity`). Use tome_jass_call. */
TOME_WASM_IMPORT(jass_call) int32_t tome_raw_jass_call(const char* name, uint32_t len, const tome_jass_value* args, int32_t count, tome_jass_value* result, char* text, uint32_t capacity);
#undef TOME_WASM_IMPORT

/* --- the wrappers (support/tome_wasm.c) ---------------------------------- */

size_t tome_strlen(const char* s);

static inline void tome_log(int32_t level, const char* text) {
    tome_raw_log(level, text, (uint32_t)tome_strlen(text));
}
static inline int32_t tome_display_text(const char* text, float seconds) {
    return tome_raw_display_text(text, (uint32_t)tome_strlen(text), seconds);
}
/* The game build ("1.27b") and file version; valid until the next call. */
const tome_wasm_game_info* tome_game_info(void);
/* The mod's id and version into `out` (NUL-terminated, cut to fit); the full length. */
uint32_t tome_mod_id(char* out, uint32_t size);
uint32_t tome_mod_version(char* out, uint32_t size);

/* Settings (tome.mod.json "config"). setting_string: NUL-terminated, cut to
 * fit, the full length returned. */
static inline int32_t tome_setting_bool(const char* key, int32_t fallback) {
    return tome_raw_setting_bool(key, (uint32_t)tome_strlen(key), fallback);
}
static inline int32_t tome_setting_int(const char* key, int32_t fallback) {
    return tome_raw_setting_int(key, (uint32_t)tome_strlen(key), fallback);
}
static inline double tome_setting_number(const char* key, double fallback) {
    return tome_raw_setting_number(key, (uint32_t)tome_strlen(key), fallback);
}
uint32_t tome_setting_string(const char* key, char* out, uint32_t size);
static inline int32_t tome_set_setting_bool(const char* key, int32_t value) {
    return tome_raw_set_setting_bool(key, (uint32_t)tome_strlen(key), value);
}
static inline int32_t tome_set_setting_int(const char* key, int32_t value) {
    return tome_raw_set_setting_int(key, (uint32_t)tome_strlen(key), value);
}
static inline int32_t tome_set_setting_number(const char* key, double value) {
    return tome_raw_set_setting_number(key, (uint32_t)tome_strlen(key), value);
}
static inline int32_t tome_set_setting_string(const char* key, const char* value) {
    return tome_raw_set_setting_string(key, (uint32_t)tome_strlen(key), value, (uint32_t)tome_strlen(value));
}

/* Callbacks: `user` comes back as it was given. At most 1024 registrations
 * per module (each enable registers again). */
typedef void (*tome_text_fn)(const char* text, void* user);
typedef void (*tome_wasm_event_fn)(const tome_wasm_event* info, void* user);
typedef void (*tome_ui_fn)(int32_t ui_event, void* user);
typedef void (*tome_click_fn)(tome_frame_t button, void* user);
typedef void (*tome_popup_fn)(uint32_t popup, int32_t choice, void* user);
typedef void (*tome_message_fn)(const char* topic, const void* data, uint32_t size, void* user);
/* A native: one 32-bit word per argument, as declared in tome.mod.json:
 * integers, booleans, handle ids and code values as they are, reals as
 * float bits (tome_real), strings as pointers to NUL-terminated copies
 * (tome_string, NULL for a null string). Returns the result word: a real's
 * bits (tome_real_bits), a string as a pointer to a NUL-terminated string
 * in the module's memory (tome_string_result; NULL for null), a handle only
 * if it's one of this call's own arguments of that type (others become
 * null). Natives returning code aren't available. */
typedef uint32_t (*tome_native_fn)(const uint32_t* args, void* user);

int32_t tome_register_command(const char* name, tome_text_fn fn, void* user);  /* fn gets the arguments */
int32_t tome_on_setting_changed(tome_text_fn fn, void* user);                  /* fn gets the key */
int32_t tome_on_event(int32_t event, tome_wasm_event_fn fn, void* user);
int32_t tome_on_ui(int32_t ui_event, tome_ui_fn fn, void* user);
int32_t tome_frame_on_click(tome_frame_t button, tome_click_fn fn, void* user);
int32_t tome_on_keybind(const char* id, tome_text_fn fn, void* user);          /* fn gets the id */
int32_t tome_subscribe(const char* topic, tome_message_fn fn, void* user);
int32_t tome_register_native(const char* name, tome_native_fn fn, void* user);
/* fn may be NULL; *id (may be NULL) gets the popup's id. */
int32_t tome_popup(const char* title, const char* text, const char* const* options, int32_t option_count,
                   int32_t default_option, int32_t cancel_option, tome_popup_fn fn, void* user, uint32_t* id);

static inline int32_t tome_publish(const char* topic, const void* data, uint32_t size) {
    return tome_raw_publish(topic, (uint32_t)tome_strlen(topic), data, size);
}
uint32_t tome_keybind_name(const char* id, char* out, uint32_t size);

/* Frames. */
static inline tome_frame_t tome_ui_root(int32_t ui_event) { return tome_raw_ui_root(ui_event); }
static inline tome_frame_t tome_frame_get(const char* name, int32_t context) {
    return tome_raw_frame_get(name, (uint32_t)tome_strlen(name), context);
}
static inline tome_frame_t tome_frame_create(const char* name, tome_frame_t parent, int32_t priority, int32_t context) {
    return tome_raw_frame_create(name, (uint32_t)tome_strlen(name), parent, priority, context);
}
static inline int32_t tome_frame_set_text(tome_frame_t frame, const char* text) {
    return tome_raw_frame_set_text(frame, text, (uint32_t)tome_strlen(text));
}
static inline int32_t tome_load_fdf(const char* path) { return tome_raw_load_fdf(path, (uint32_t)tome_strlen(path)); }
static inline int32_t tome_load_toc(const char* path) { return tome_raw_load_toc(path, (uint32_t)tome_strlen(path)); }

/* The selection: up to `max` units into `out` (their names stay valid until
 * the next call), *leader (may be NULL) the active one or -1; how many are
 * selected. */
int32_t tome_selection(tome_wasm_unit* out, int32_t max, int32_t* leader);

/* A unit as the selection describes it (its name stays valid until the next
 * call): TOME_OK, or TOME_ERR_INVALID_ARGUMENT if `unit` isn't a live unit. */
int32_t tome_unit_info(uint32_t unit, tome_wasm_unit* out);

/* Runs a JASS native by name: `count` arguments, the result in *result (may
 * be NULL). A string result stays valid until the next call. TOME_OK or a
 * tome_result. */
int32_t tome_jass_call(const char* name, const tome_jass_value* args, int32_t count, tome_jass_value* result);

/* A property's value, or `fallback` if it can't be read. */
static inline double tome_property_value(uint32_t object, int32_t property, double fallback) {
    double value;
    return tome_get_property(object, property, &value) == TOME_OK ? value : fallback;
}

/* A game file (through the archives and Tome's overlay) into `out`, up to
 * `size` bytes; the file's whole size (0 if there's none). */
static inline uint32_t tome_read_game_file(const char* name, void* out, uint32_t size) {
    uint32_t n = tome_raw_read_game_file(name, (uint32_t)tome_strlen(name), out, size);
    return n == UINT32_MAX ? 0 : n;
}

/* The mod's own files (instead of tome.h's config_dir): in
 * tome/mods/config/<id>/, plain names ("state.json": letters, digits,
 * . _ -), up to 1 MiB. config_read copies up to `size` bytes and returns the
 * file's size, or UINT32_MAX if there's none. */
static inline uint32_t tome_config_read(const char* name, void* out, uint32_t size) {
    return tome_raw_config_read(name, (uint32_t)tome_strlen(name), out, size);
}
static inline int32_t tome_config_write(const char* name, const void* data, uint32_t size) {
    return tome_raw_config_write(name, (uint32_t)tome_strlen(name), data, size);
}

/* JASS values in natives. */
static inline float tome_real(uint32_t word) {
    float f;
    __builtin_memcpy(&f, &word, 4);
    return f;
}
static inline uint32_t tome_real_bits(float value) {
    uint32_t w;
    __builtin_memcpy(&w, &value, 4);
    return w;
}
static inline const char* tome_string(uint32_t word) { return (const char*)(uintptr_t)word; }
static inline uint32_t tome_string_result(const char* text) { return (uint32_t)(uintptr_t)text; }

/* Small helpers for text without libc: append `text` / a decimal number to
 * the NUL-terminated string in `buf` (of `size` bytes), cut to fit; buf. */
char* tome_append(char* buf, uint32_t size, const char* text);
char* tome_append_int(char* buf, uint32_t size, int64_t value);

#ifdef __cplusplus
}
#endif
