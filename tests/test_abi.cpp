// tome.h is append-only: a field, once released, keeps its offset forever, or
// mods built against an older header read the wrong one. These are the
// offsets on i686 (the only target) as of API version 13. Adding a field at
// the end of a struct passes; moving, removing or resizing one fails to
// compile. List new fields here once they're released.
//
// First generated from bindgen's layout (sdk/rust/tome-sys/src/bindings.rs).
#include <tome/tome.h>

#include <cstddef>

#include "test.h"

static_assert(sizeof(void*) == 4, "mods are 32-bit");

// tome_game_info
static_assert(offsetof(tome_game_info, size) == 0);
static_assert(offsetof(tome_game_info, build_name) == 4);
static_assert(offsetof(tome_game_info, file_version) == 8);
static_assert(offsetof(tome_game_info, game_base) == 12);

// tome_symbol
static_assert(offsetof(tome_symbol, size) == 0);
static_assert(offsetof(tome_symbol, name) == 4);
static_assert(offsetof(tome_symbol, kind) == 8);
static_assert(offsetof(tome_symbol, value) == 12);
static_assert(offsetof(tome_symbol, rva) == 16);
static_assert(offsetof(tome_symbol, convention) == 20);
static_assert(offsetof(tome_symbol, signature) == 24);

// tome_unit
static_assert(offsetof(tome_unit, size) == 0);
static_assert(offsetof(tome_unit, object) == 4);
static_assert(offsetof(tome_unit, handle) == 8);
static_assert(offsetof(tome_unit, type_id) == 12);
static_assert(offsetof(tome_unit, name) == 16);
static_assert(offsetof(tome_unit, owner) == 20);
static_assert(offsetof(tome_unit, flags) == 24);

// tome_event_info
static_assert(offsetof(tome_event_info, size) == 0);
static_assert(offsetof(tome_event_info, event) == 4);
static_assert(offsetof(tome_event_info, map_name) == 8);
static_assert(offsetof(tome_event_info, map_path) == 12);
static_assert(offsetof(tome_event_info, delta) == 16);
static_assert(offsetof(tome_event_info, frame) == 24);
static_assert(offsetof(tome_event_info, game_time) == 32);
static_assert(offsetof(tome_event_info, map_description) == 40);
static_assert(offsetof(tome_event_info, units) == 44);
static_assert(offsetof(tome_event_info, unit_count) == 48);
static_assert(offsetof(tome_event_info, leader) == 52);
static_assert(offsetof(tome_event_info, player) == 56);
static_assert(offsetof(tome_event_info, player_name) == 60);
static_assert(offsetof(tome_event_info, text) == 64);
static_assert(offsetof(tome_event_info, recipient) == 68);
static_assert(offsetof(tome_event_info, unit) == 72);
static_assert(offsetof(tome_event_info, other) == 76);
static_assert(offsetof(tome_event_info, item) == 80);
static_assert(offsetof(tome_event_info, object_id) == 84);
static_assert(offsetof(tome_event_info, object_name) == 88);
static_assert(offsetof(tome_event_info, level) == 92);
static_assert(offsetof(tome_event_info, save_name) == 96);
static_assert(offsetof(tome_event_info, third) == 100);
static_assert(offsetof(tome_event_info, order_id) == 104);
static_assert(offsetof(tome_event_info, order_type) == 108);
static_assert(offsetof(tome_event_info, x) == 112);
static_assert(offsetof(tome_event_info, y) == 120);
static_assert(offsetof(tome_event_info, amount) == 128);
static_assert(offsetof(tome_event_info, destructable) == 136);
static_assert(offsetof(tome_event_info, item_handle) == 140);
static_assert(offsetof(tome_event_info, destructable_handle) == 144);
static_assert(offsetof(tome_event_info, key) == 148);
static_assert(offsetof(tome_event_info, pressed) == 152);
static_assert(offsetof(tome_event_info, dialog) == 156);
static_assert(offsetof(tome_event_info, button) == 160);
static_assert(offsetof(tome_event_info, resource) == 164);
static_assert(offsetof(tome_event_info, previous) == 168);
static_assert(offsetof(tome_event_info, script_shadows) == 176);

// tome_script_shadow
static_assert(offsetof(tome_script_shadow, size) == 0);
static_assert(offsetof(tome_script_shadow, name) == 4);
static_assert(offsetof(tome_script_shadow, local_type) == 8);
static_assert(offsetof(tome_script_shadow, global_type) == 12);
static_assert(offsetof(tome_script_shadow, line) == 16);

// tome_hook_call
static_assert(offsetof(tome_hook_call, size) == 0);
static_assert(offsetof(tome_hook_call, name) == 4);
static_assert(offsetof(tome_hook_call, args) == 8);
static_assert(offsetof(tome_hook_call, arg_count) == 12);
static_assert(offsetof(tome_hook_call, result) == 16);
static_assert(offsetof(tome_hook_call, result_high) == 20);
static_assert(offsetof(tome_hook_call, skip_original) == 24);
static_assert(offsetof(tome_hook_call, return_address) == 28);
static_assert(offsetof(tome_hook_call, result_float) == 32);
static_assert(offsetof(tome_hook_call, extra_count) == 40);

// tome_popup
static_assert(offsetof(tome_popup, size) == 0);
static_assert(offsetof(tome_popup, title) == 4);
static_assert(offsetof(tome_popup, text) == 8);
static_assert(offsetof(tome_popup, options) == 12);
static_assert(offsetof(tome_popup, option_count) == 16);
static_assert(offsetof(tome_popup, default_option) == 20);
static_assert(offsetof(tome_popup, cancel_option) == 24);

// tome_api
static_assert(offsetof(tome_api, size) == 0);
static_assert(offsetof(tome_api, api_version) == 4);
static_assert(offsetof(tome_api, tome_version) == 8);
static_assert(offsetof(tome_api, log) == 12);
static_assert(offsetof(tome_api, game_info) == 16);
static_assert(offsetof(tome_api, mod_id) == 20);
static_assert(offsetof(tome_api, mod_version) == 24);
static_assert(offsetof(tome_api, mod_dir) == 28);
static_assert(offsetof(tome_api, config_dir) == 32);
static_assert(offsetof(tome_api, register_native) == 36);
static_assert(offsetof(tome_api, resolve) == 40);
static_assert(offsetof(tome_api, on_ui) == 44);
static_assert(offsetof(tome_api, load_fdf) == 48);
static_assert(offsetof(tome_api, load_toc) == 52);
static_assert(offsetof(tome_api, frame_get) == 56);
static_assert(offsetof(tome_api, frame_create) == 60);
static_assert(offsetof(tome_api, frame_set_point) == 64);
static_assert(offsetof(tome_api, frame_set_text) == 68);
static_assert(offsetof(tome_api, frame_set_visible) == 72);
static_assert(offsetof(tome_api, frame_set_animation) == 76);
static_assert(offsetof(tome_api, frame_on_click) == 80);
static_assert(offsetof(tome_api, hook) == 84);
static_assert(offsetof(tome_api, unhook) == 88);
static_assert(offsetof(tome_api, hook_call_next) == 92);
static_assert(offsetof(tome_api, on_event) == 96);
static_assert(offsetof(tome_api, register_command) == 100);
static_assert(offsetof(tome_api, display_text) == 104);
static_assert(offsetof(tome_api, ui_root) == 108);
static_assert(offsetof(tome_api, jass_string) == 112);
static_assert(offsetof(tome_api, jass_string_create) == 116);
static_assert(offsetof(tome_api, jass_code_call) == 120);
static_assert(offsetof(tome_api, jass_code_execute) == 124);
static_assert(offsetof(tome_api, setting_bool) == 128);
static_assert(offsetof(tome_api, setting_int) == 132);
static_assert(offsetof(tome_api, setting_number) == 136);
static_assert(offsetof(tome_api, setting_string) == 140);
static_assert(offsetof(tome_api, set_setting_bool) == 144);
static_assert(offsetof(tome_api, set_setting_int) == 148);
static_assert(offsetof(tome_api, set_setting_number) == 152);
static_assert(offsetof(tome_api, set_setting_string) == 156);
static_assert(offsetof(tome_api, on_setting_changed) == 160);
static_assert(offsetof(tome_api, on_keybind) == 164);
static_assert(offsetof(tome_api, keybind_name) == 168);
static_assert(offsetof(tome_api, selection) == 172);
static_assert(offsetof(tome_api, popup) == 176);
static_assert(offsetof(tome_api, popup_close) == 180);
static_assert(offsetof(tome_api, restart_game) == 184);
static_assert(offsetof(tome_api, service_register) == 188);
static_assert(offsetof(tome_api, service_unregister) == 192);
static_assert(offsetof(tome_api, service_get) == 196);
static_assert(offsetof(tome_api, on_service) == 200);
static_assert(offsetof(tome_api, subscribe) == 204);
static_assert(offsetof(tome_api, publish) == 208);
static_assert(offsetof(tome_api, map_script_shadows) == 212);

TEST(abi_layout_is_append_only) {
    // Everything is checked at compile time; this only keeps the file in the list.
    CHECK(offsetof(tome_api, size) == 0);
}
