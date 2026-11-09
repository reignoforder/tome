// SPDX-License-Identifier: MIT-0
//! Tome's imports (module `tome`) as Tome provides them, the same list as
//! `sdk/include/tome/tome_wasm.h` (`docs/wasm-mods.md` explains each).
//! Strings are (pointer, length), UTF-8 without NUL bytes; out-parameters
//! are (pointer, capacity) and return the full length (`u32::MAX` for a
//! bad buffer). Callbacks are ids the module chose (`tome_callback`).
//! The rest of the crate wraps these.
//!
//! Outside wasm32 (the crate's own unit tests) they panic.

macro_rules! imports {
    ($(fn $name:ident($($arg:ident: $ty:ty),* $(,)?) $(-> $ret:ty)?;)*) => {
        #[cfg(target_arch = "wasm32")]
        #[link(wasm_import_module = "tome")]
        unsafe extern "C" {
            $(pub fn $name($($arg: $ty),*) $(-> $ret)?;)*
        }

        #[cfg(not(target_arch = "wasm32"))]
        mod host {
            $(
                #[allow(unused_variables, clippy::missing_safety_doc)]
                pub unsafe fn $name($($arg: $ty),*) $(-> $ret)? {
                    panic!(concat!("tome.", stringify!($name), " is a WebAssembly import"))
                }
            )*
        }
        #[cfg(not(target_arch = "wasm32"))]
        pub use host::*;
    };
}

imports! {
    fn log(level: i32, text: *const u8, len: u32);
    fn mod_id(out: *mut u8, capacity: u32) -> u32;
    fn mod_version(out: *mut u8, capacity: u32) -> u32;
    fn game_info(out: *mut u8, capacity: u32) -> u32;
    fn register_command(name: *const u8, len: u32, callback: u32) -> i32;
    fn display_text(text: *const u8, len: u32, seconds: f32) -> i32;
    fn setting_bool(key: *const u8, len: u32, fallback: i32) -> i32;
    fn setting_int(key: *const u8, len: u32, fallback: i32) -> i32;
    fn setting_number(key: *const u8, len: u32, fallback: f64) -> f64;
    fn setting_string(key: *const u8, len: u32, out: *mut u8, capacity: u32) -> u32;
    fn set_setting_bool(key: *const u8, len: u32, value: i32) -> i32;
    fn set_setting_int(key: *const u8, len: u32, value: i32) -> i32;
    fn set_setting_number(key: *const u8, len: u32, value: f64) -> i32;
    fn set_setting_string(key: *const u8, len: u32, value: *const u8, value_len: u32) -> i32;
    fn on_setting_changed(callback: u32) -> i32;
    fn on_event(event: i32, callback: u32) -> i32;
    fn on_ui(event: i32, callback: u32) -> i32;
    fn ui_root(event: i32) -> u32;
    fn frame_get(name: *const u8, len: u32, context: i32) -> u32;
    fn frame_create(name: *const u8, len: u32, parent: u32, priority: i32, context: i32) -> u32;
    fn frame_set_point(frame: u32, point: i32, relative: u32, relative_point: i32, x: f32, y: f32) -> i32;
    fn frame_set_text(frame: u32, text: *const u8, len: u32) -> i32;
    fn frame_set_visible(frame: u32, visible: i32) -> i32;
    fn frame_set_animation(sprite: u32, tags: *const i32, count: i32) -> i32;
    fn frame_on_click(button: u32, callback: u32) -> i32;
    fn load_fdf(path: *const u8, len: u32) -> i32;
    fn load_toc(path: *const u8, len: u32) -> i32;
    fn on_keybind(id: *const u8, len: u32, callback: u32) -> i32;
    fn keybind_name(id: *const u8, len: u32, out: *mut u8, capacity: u32) -> u32;
    fn popup(spec: *const u32, callback: u32, id: *mut u32) -> i32;
    fn popup_close(id: u32) -> i32;
    fn restart_game() -> i32;
    fn selection(out: *mut u8, capacity: u32, count: *mut u32, leader: *mut i32) -> u32;
    fn subscribe(topic: *const u8, len: u32, callback: u32) -> i32;
    fn publish(topic: *const u8, len: u32, data: *const u8, size: u32) -> i32;
    fn map_script_shadows(out: *mut u8, capacity: u32, count: *mut u32) -> u32;
    fn read_game_file(name: *const u8, len: u32, out: *mut u8, capacity: u32) -> u32;
    fn config_read(name: *const u8, len: u32, out: *mut u8, capacity: u32) -> u32;
    fn config_write(name: *const u8, len: u32, data: *const u8, size: u32) -> i32;
    fn register_native(name: *const u8, len: u32, callback: u32) -> i32;
    fn jass_code_call(code: u32, result: *mut i32) -> i32;
    fn jass_code_execute(code: u32) -> i32;
    fn object_type(object: u32) -> i32;
    fn object_of(jass_handle: u32, kind: *mut i32) -> u32;
    fn handle_of(object: u32) -> u32;
    fn player_object(slot: i32) -> u32;
    fn unit_info(unit: u32, out: *mut u8, capacity: u32) -> u32;
    fn get_property(object: u32, property: i32, value: *mut f64) -> i32;
    fn set_property(object: u32, property: i32, value: f64) -> i32;
    fn get_keyed_property(object: u32, property: i32, key: u32, value: *mut f64) -> i32;
    fn set_keyed_property(object: u32, property: i32, key: u32, value: f64) -> i32;
    fn list_objects(kind: i32, out: *mut u32, max: i32) -> i32;
    fn host_player() -> i32;
    fn net_send(data: *const u8, size: u32) -> i32;
    fn on_net_message(callback: u32) -> i32;
    fn timer_start(clock: i32, delay: f64, interval: f64, callback: u32, timer: *mut u32) -> i32;
    fn timer_stop(timer: u32) -> i32;
    fn on_checksum(callback: u32) -> i32;
    fn version_compare(kind: i32, a: *const u8, a_len: u32, b: *const u8, b_len: u32, order: *mut i32) -> i32;
    fn version_match(kind: i32, range: *const u8, range_len: u32, version: *const u8, version_len: u32) -> i32;
    fn check_metadata(text: *const u8, size: u32, reason: *mut u8, reason_size: u32) -> i32;
    fn jass_call(name: *const u8, len: u32, args: *const u32, count: i32, result: *mut u32, text: *mut u8, capacity: u32) -> i32;
}
