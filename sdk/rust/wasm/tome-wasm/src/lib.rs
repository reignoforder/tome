//! Rust API for Tome's WebAssembly mods (experimental; `docs/wasm-mods.md`).
//!
//! A Wasm mod runs sandboxed: it can only call Tome's imports, and Tome
//! copies what it hands the mod into the mod's own memory. Game objects
//! (frames, units) are handles, never pointers. The functions mirror
//! `tome.h`'s of the same name (`sdk/include/tome/tome.h` documents each);
//! what's different here is said here.
//!
//! A mod is a `cdylib` built for `wasm32-unknown-unknown` that exports its
//! lifecycle with [`export_mod!`]; `tome.mod.json` names the `.wasm` in
//! `"wasm"`:
//!
//! ```ignore
//! fn load() {
//!     tome_wasm::info!("loaded in Warcraft III {}", tome_wasm::game_info().build_name);
//! }
//! fn enable() {
//!     tome_wasm::register_command("hello", |args| tome_wasm::info!("hello {args}")).ok();
//! }
//! tome_wasm::export_mod! { load: load, enable: enable }
//! ```
//!
//! - **Callbacks** are closures (`Fn`; keep state in `Cell`s or
//!   `RefCell`s). They run on the game's main thread, one at a time (or
//!   nested, when one causes another), and are dropped when the mod is
//!   disabled, when Tome revokes them: register them in `enable`.
//! - **A panic is a trap** (`wasm32-unknown-unknown` aborts): the panic is
//!   logged, the call abandoned and the mod disabled, as for a crash. So is
//!   running past the instruction budget (about half a second of work per
//!   call).

pub mod consts;
mod events;
mod natives;
#[doc(hidden)]
pub mod raw;
mod rt;

#[doc(hidden)]
pub mod __rt {
    pub use crate::rt::{buffer, callback, disable, enable, load, native, unload};
}

use core::fmt;
use std::borrow::Cow;
use std::rc::Rc;

pub use events::{Event, EventInfo, OwnedUnit, Unit};
pub use natives::{Args, Code, Handle, Ret, jass_code_call, jass_code_execute, register_native};
pub use rt::LoadResult;

use rt::Callback;

/// A failure Tome reported (`tome_result`).
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct Error(pub i32);

impl Error {
    pub const INVALID_ARGUMENT: Error = Error(consts::TOME_ERR_INVALID_ARGUMENT);
    pub const NOT_DECLARED: Error = Error(consts::TOME_ERR_NOT_DECLARED);
    pub const NOT_OWNER: Error = Error(consts::TOME_ERR_NOT_OWNER);
    pub const UNAVAILABLE: Error = Error(consts::TOME_ERR_UNAVAILABLE);
    pub const WRONG_THREAD: Error = Error(consts::TOME_ERR_WRONG_THREAD);
    pub const FAILED: Error = Error(consts::TOME_ERR_FAILED);
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match *self {
            Error::INVALID_ARGUMENT => "invalid argument",
            Error::NOT_DECLARED => "not declared in tome.mod.json",
            Error::NOT_OWNER => "belongs to another mod",
            Error::UNAVAILABLE => "unavailable",
            Error::WRONG_THREAD => "must be called on the main thread",
            Error::FAILED => "failed (see tome.log)",
            Error(code) => return write!(f, "error {code}"),
        })
    }
}

impl fmt::Debug for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Error({}: {self})", self.0)
    }
}

impl std::error::Error for Error {}

pub type Result<T> = core::result::Result<T, Error>;

pub(crate) fn check(result: i32) -> Result<()> {
    if result == consts::TOME_OK {
        Ok(())
    } else {
        Err(Error(result))
    }
}

/// A string for an import: (pointer, length). Tome refuses one with a NUL
/// byte or longer than 4096 bytes (logged).
pub(crate) fn str_arg(s: &str) -> (*const u8, u32) {
    (s.as_ptr(), u32::try_from(s.len()).unwrap_or(u32::MAX))
}

/// Calls an out-parameter import until the buffer fits what it returns:
/// the bytes, or `None` for `u32::MAX` (none, or a bad buffer).
pub(crate) fn read_out(first: usize, mut f: impl FnMut(*mut u8, u32) -> u32) -> Option<Vec<u8>> {
    let mut buf = vec![0u8; first];
    loop {
        let full = f(buf.as_mut_ptr(), buf.len() as u32);
        if full == u32::MAX {
            return None;
        }
        if full as usize <= buf.len() {
            buf.truncate(full as usize);
            return Some(buf);
        }
        buf = vec![0u8; full as usize];
    }
}

fn read_text(f: impl FnMut(*mut u8, u32) -> u32) -> String {
    read_out(256, f)
        .map(|b| String::from_utf8_lossy(&b).into_owned())
        .unwrap_or_default()
}

// --- logging, the mod, the game ---------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Level {
    Debug,
    Info,
    Warn,
    Error,
}

/// Logs `text` to tome.log, tagged with the mod's id.
pub fn log(level: Level, text: &str) {
    let level = match level {
        Level::Debug => consts::TOME_LOG_DEBUG,
        Level::Info => consts::TOME_LOG_INFO,
        Level::Warn => consts::TOME_LOG_WARN,
        Level::Error => consts::TOME_LOG_ERROR,
    };
    // Tome takes up to 4096 bytes without NULs: cut, as a line in the log.
    let mut text = Cow::Borrowed(text);
    if text.contains('\0') {
        text = Cow::Owned(text.replace('\0', " "));
    }
    let mut end = text.len().min(4096);
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    let (ptr, len) = str_arg(&text[..end]);
    unsafe { raw::log(level, ptr, len) }
}

#[macro_export]
macro_rules! debug { ($($t:tt)*) => { $crate::log($crate::Level::Debug, &::std::format!($($t)*)) }; }
#[macro_export]
macro_rules! info { ($($t:tt)*) => { $crate::log($crate::Level::Info, &::std::format!($($t)*)) }; }
#[macro_export]
macro_rules! warn { ($($t:tt)*) => { $crate::log($crate::Level::Warn, &::std::format!($($t)*)) }; }
#[macro_export]
macro_rules! error { ($($t:tt)*) => { $crate::log($crate::Level::Error, &::std::format!($($t)*)) }; }

/// The mod's id (`tome.mod.json` "id").
pub fn mod_id() -> String {
    read_text(|out, cap| unsafe { raw::mod_id(out, cap) })
}

/// The mod's version (`tome.mod.json` "version").
pub fn mod_version() -> String {
    read_text(|out, cap| unsafe { raw::mod_version(out, cap) })
}

/// The game's build (`tome_game_info`, without `game_base`: an address).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct GameInfo {
    /// e.g. "1.27b"
    pub build_name: String,
    /// e.g. "1.27.1.7085"
    pub file_version: String,
}

pub fn game_info() -> GameInfo {
    // [size][build_name][file_version], the strings after it in the block.
    let Some(block) = read_out(256, |out, cap| unsafe { raw::game_info(out, cap) }) else {
        return GameInfo::default();
    };
    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct Raw {
        size: u32,
        build_name: u32,
        file_version: u32,
    }
    let raw: Raw = events::read_struct(&block);
    // The pointers are addresses in `block`, which is alive here.
    unsafe {
        GameInfo {
            build_name: events::text(raw.build_name).into_owned(),
            file_version: events::text(raw.file_version).into_owned(),
        }
    }
}

/// Shows `text` on the screen for `seconds` (in a game).
pub fn display_text(text: &str, seconds: f32) -> Result<()> {
    let (ptr, len) = str_arg(text);
    check(unsafe { raw::display_text(ptr, len, seconds) })
}

/// Asks the player to restart the game (a dialog; nothing happens unless
/// they agree).
pub fn restart_game() -> Result<()> {
    check(unsafe { raw::restart_game() })
}

// --- chat commands, settings, keybinds --------------------------------------

/// `/<name> args`: `f` gets the arguments.
pub fn register_command(name: &str, f: impl Fn(&str) + 'static) -> Result<()> {
    let (ptr, len) = str_arg(name);
    rt::register(Callback::Text(Rc::new(f)), |id| unsafe {
        raw::register_command(ptr, len, id)
    })
    .map(drop)
}

/// The settings (`tome.mod.json` "config"): the value, or `fallback`.
pub fn setting_bool(key: &str, fallback: bool) -> bool {
    let (ptr, len) = str_arg(key);
    unsafe { raw::setting_bool(ptr, len, fallback as i32) != 0 }
}

pub fn setting_int(key: &str, fallback: i32) -> i32 {
    let (ptr, len) = str_arg(key);
    unsafe { raw::setting_int(ptr, len, fallback) }
}

pub fn setting_number(key: &str, fallback: f64) -> f64 {
    let (ptr, len) = str_arg(key);
    unsafe { raw::setting_number(ptr, len, fallback) }
}

/// A string setting ("" if there's none).
pub fn setting_string(key: &str) -> String {
    let (ptr, len) = str_arg(key);
    read_text(|out, cap| unsafe { raw::setting_string(ptr, len, out, cap) })
}

pub fn set_setting_bool(key: &str, value: bool) -> Result<()> {
    let (ptr, len) = str_arg(key);
    check(unsafe { raw::set_setting_bool(ptr, len, value as i32) })
}

pub fn set_setting_int(key: &str, value: i32) -> Result<()> {
    let (ptr, len) = str_arg(key);
    check(unsafe { raw::set_setting_int(ptr, len, value) })
}

pub fn set_setting_number(key: &str, value: f64) -> Result<()> {
    let (ptr, len) = str_arg(key);
    check(unsafe { raw::set_setting_number(ptr, len, value) })
}

pub fn set_setting_string(key: &str, value: &str) -> Result<()> {
    let (ptr, len) = str_arg(key);
    let (vptr, vlen) = str_arg(value);
    check(unsafe { raw::set_setting_string(ptr, len, vptr, vlen) })
}

/// `f` gets the key of a setting that changed.
pub fn on_setting_changed(f: impl Fn(&str) + 'static) -> Result<()> {
    rt::register(Callback::Text(Rc::new(f)), |id| unsafe { raw::on_setting_changed(id) }).map(drop)
}

/// A keybind (`tome.mod.json` "keybinds"): `f` gets its id.
pub fn on_keybind(id: &str, f: impl Fn(&str) + 'static) -> Result<()> {
    let (ptr, len) = str_arg(id);
    rt::register(Callback::Text(Rc::new(f)), |cb| unsafe {
        raw::on_keybind(ptr, len, cb)
    })
    .map(drop)
}

/// The key a keybind is set to, e.g. "Ctrl+G".
pub fn keybind_name(id: &str) -> String {
    let (ptr, len) = str_arg(id);
    read_text(|out, cap| unsafe { raw::keybind_name(ptr, len, out, cap) })
}

// --- events -------------------------------------------------------------------

/// `f` runs for every `event` (on the main thread, in registration order).
pub fn on_event(event: Event, f: impl Fn(&EventInfo) + 'static) -> Result<()> {
    rt::register(Callback::Event(Rc::new(f)), |id| unsafe { raw::on_event(event.0, id) }).map(drop)
}

/// The units selected now (copies), and the index of the active one.
pub fn selection() -> (Vec<OwnedUnit>, Option<usize>) {
    let mut count = 0u32;
    let mut leader = -1i32;
    let Some(block) = read_out(4096, |out, cap| unsafe {
        raw::selection(out, cap, &mut count, &mut leader)
    }) else {
        return (Vec::new(), None);
    };
    let size = core::mem::size_of::<events::RawUnit>();
    let units = (0..count as usize)
        .filter_map(|i| block.get(i * size..(i + 1) * size))
        .map(|b| Unit::from_raw(events::read_struct(b)).to_owned())
        .collect();
    (units, usize::try_from(leader).ok())
}

/// A global of the map script that a function's local hides
/// (`tome_script_shadow`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ScriptShadow {
    pub name: String,
    pub local_type: String,
    pub global_type: String,
    pub line: i32,
}

/// The map script's shadowed globals (for `TOME_EVENT_MAP_LOAD`).
pub fn map_script_shadows() -> Vec<ScriptShadow> {
    let mut count = 0u32;
    let Some(block) = read_out(4096, |out, cap| unsafe {
        raw::map_script_shadows(out, cap, &mut count)
    }) else {
        return Vec::new();
    };
    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct Raw {
        size: u32,
        name: u32,
        local_type: u32,
        global_type: u32,
        line: i32,
    }
    const _: () = assert!(core::mem::size_of::<Raw>() == 20);
    (0..count as usize)
        .filter_map(|i| block.get(i * 20..(i + 1) * 20))
        .map(|b| {
            let raw: Raw = events::read_struct(b);
            unsafe {
                ScriptShadow {
                    name: events::text(raw.name).into_owned(),
                    local_type: events::text(raw.local_type).into_owned(),
                    global_type: events::text(raw.global_type).into_owned(),
                    line: raw.line,
                }
            }
        })
        .collect()
}

// --- screens and frames -----------------------------------------------------

/// A menu screen or the game's UI (`tome_ui_event`).
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct UiEvent(pub i32);

/// An anchor point (`tome_frame_point`).
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct FramePoint(pub i32);

/// `f` runs every time the screen is built.
pub fn on_ui(event: UiEvent, f: impl Fn(UiEvent) + 'static) -> Result<()> {
    rt::register(Callback::Ui(Rc::new(f)), |id| unsafe { raw::on_ui(event.0, id) }).map(drop)
}

/// A frame: a handle Tome looks up again (by name) on every use, so one to
/// a frame that's gone just fails.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct Frame(pub u32);

fn frame(handle: u32) -> Option<Frame> {
    (handle != 0).then_some(Frame(handle))
}

impl Frame {
    /// The root frame of a screen.
    pub fn root(event: UiEvent) -> Option<Frame> {
        frame(unsafe { raw::ui_root(event.0) })
    }

    /// The frame named `name` (FDF), with its `context`.
    pub fn get(name: &str, context: i32) -> Option<Frame> {
        let (ptr, len) = str_arg(name);
        frame(unsafe { raw::frame_get(ptr, len, context) })
    }

    /// A frame from a loaded FDF template (`load_fdf`).
    pub fn create(name: &str, parent: Frame, priority: i32, context: i32) -> Option<Frame> {
        let (ptr, len) = str_arg(name);
        frame(unsafe { raw::frame_create(ptr, len, parent.0, priority, context) })
    }

    pub fn set_point(
        self,
        point: FramePoint,
        relative: Frame,
        relative_point: FramePoint,
        x: f32,
        y: f32,
    ) -> Result<()> {
        check(unsafe { raw::frame_set_point(self.0, point.0, relative.0, relative_point.0, x, y) })
    }

    pub fn set_text(self, text: &str) -> Result<()> {
        let (ptr, len) = str_arg(text);
        check(unsafe { raw::frame_set_text(self.0, ptr, len) })
    }

    pub fn set_visible(self, visible: bool) -> Result<()> {
        check(unsafe { raw::frame_set_visible(self.0, visible as i32) })
    }

    /// A sprite's animation, by tags.
    pub fn set_animation(self, tags: &[i32]) -> Result<()> {
        let count = i32::try_from(tags.len()).map_err(|_| Error::INVALID_ARGUMENT)?;
        check(unsafe { raw::frame_set_animation(self.0, tags.as_ptr(), count) })
    }

    /// `f` runs when the button is clicked, with the button.
    pub fn on_click(self, f: impl Fn(Frame) + 'static) -> Result<()> {
        rt::register(Callback::Click(Rc::new(f)), |id| unsafe {
            raw::frame_on_click(self.0, id)
        })
        .map(drop)
    }
}

/// Loads an FDF file of the mod (a path in its folder).
pub fn load_fdf(path: &str) -> Result<()> {
    let (ptr, len) = str_arg(path);
    check(unsafe { raw::load_fdf(ptr, len) })
}

/// Loads a TOC of FDF files from the game's files.
pub fn load_toc(path: &str) -> Result<()> {
    let (ptr, len) = str_arg(path);
    check(unsafe { raw::load_toc(ptr, len) })
}

// --- popups -----------------------------------------------------------------

/// A dialog with buttons (`tome_popup_spec`).
pub struct Popup<'a> {
    pub title: &'a str,
    pub text: &'a str,
    pub options: &'a [&'a str],
    /// The button Enter picks.
    pub default_option: Option<usize>,
    /// The button Escape picks.
    pub cancel_option: Option<usize>,
}

impl<'a> Popup<'a> {
    pub fn new(title: &'a str, text: &'a str, options: &'a [&'a str]) -> Popup<'a> {
        Popup {
            title,
            text,
            options,
            default_option: None,
            cancel_option: None,
        }
    }

    pub fn default_option(mut self, index: usize) -> Self {
        self.default_option = Some(index);
        self
    }

    pub fn cancel_option(mut self, index: usize) -> Self {
        self.cancel_option = Some(index);
        self
    }
}

/// Shows a popup; `f` gets the button picked (`None` if it was closed
/// without one: `popup_close`). Returns the popup's id.
pub fn popup(spec: &Popup, f: impl Fn(Option<usize>) + 'static) -> Result<u32> {
    if spec.options.is_empty() || spec.options.len() > 32 {
        return Err(Error::INVALID_ARGUMENT);
    }
    let labels: Vec<u32> = spec
        .options
        .iter()
        .flat_map(|o| {
            let (ptr, len) = str_arg(o);
            [ptr as usize as u32, len]
        })
        .collect();
    let index = |i: Option<usize>| i.map_or(-1, |i| i as i32) as u32;
    let (title, title_len) = str_arg(spec.title);
    let (text, text_len) = str_arg(spec.text);
    let raw_spec: [u32; 9] = [
        title as usize as u32,
        title_len,
        text as usize as u32,
        text_len,
        labels.as_ptr() as usize as u32,
        spec.options.len() as u32,
        index(spec.default_option),
        index(spec.cancel_option),
        0,
    ];
    let mut id = 0u32;
    rt::register(Callback::Popup(Rc::new(move |_, choice| f(choice))), |cb| unsafe {
        raw::popup(raw_spec.as_ptr(), cb, &mut id)
    })?;
    Ok(id)
}

pub fn popup_close(id: u32) -> Result<()> {
    check(unsafe { raw::popup_close(id) })
}

// --- messages between mods --------------------------------------------------

/// `f` gets every message published on `topic` by other mods (topic, data).
pub fn subscribe(topic: &str, f: impl Fn(&str, &[u8]) + 'static) -> Result<()> {
    let (ptr, len) = str_arg(topic);
    rt::register(Callback::Message(Rc::new(f)), |id| unsafe {
        raw::subscribe(ptr, len, id)
    })
    .map(drop)
}

/// Sends `data` (up to 64 KiB, copied) to the other mods subscribed to
/// `topic`; how many got it.
pub fn publish(topic: &str, data: &[u8]) -> usize {
    let (ptr, len) = str_arg(topic);
    let size = u32::try_from(data.len()).unwrap_or(u32::MAX);
    usize::try_from(unsafe { raw::publish(ptr, len, data.as_ptr(), size) }).unwrap_or(0)
}

// --- files --------------------------------------------------------------------

/// A game file (through the archives and Tome's overlay), if there is one.
pub fn read_game_file(name: &str) -> Option<Vec<u8>> {
    let (ptr, len) = str_arg(name);
    // 0 for a file that isn't there (or is empty), u32::MAX for a bad name.
    read_out(64 * 1024, |out, cap| unsafe { raw::read_game_file(ptr, len, out, cap) }).filter(|b| !b.is_empty())
}

/// One of the mod's own files (in `tome/mods/config/<id>/`; plain names:
/// letters, digits, `.`, `_`, `-`), if it exists.
pub fn config_read(name: &str) -> Option<Vec<u8>> {
    let (ptr, len) = str_arg(name);
    read_out(4096, |out, cap| unsafe { raw::config_read(ptr, len, out, cap) })
}

/// Writes one of the mod's own files (up to 1 MiB).
pub fn config_write(name: &str, data: &[u8]) -> Result<()> {
    let (ptr, len) = str_arg(name);
    let size = u32::try_from(data.len()).map_err(|_| Error::INVALID_ARGUMENT)?;
    check(unsafe { raw::config_write(ptr, len, data.as_ptr(), size) })
}

// --- the lifecycle ----------------------------------------------------------

/// Exports the mod's lifecycle and the functions Tome calls back through
/// (`tome_buffer`, `tome_callback`, `tome_native`). `load` is required and
/// returns `()` or a `Result` (an error is logged and the mod doesn't
/// load); `enable`, `disable` and `unload` are optional:
///
/// ```ignore
/// tome_wasm::export_mod! { load: load, enable: enable, disable: disable }
/// ```
#[macro_export]
macro_rules! export_mod {
    (load: $load:path $(, enable: $enable:path)? $(, disable: $disable:path)? $(, unload: $unload:path)? $(,)?) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn tome_mod_load() -> i32 {
            $crate::__rt::load(|| $crate::LoadResult::into_code($load()))
        }
        $(
            #[unsafe(no_mangle)]
            pub extern "C" fn tome_mod_enable() {
                $crate::__rt::enable($enable)
            }
        )?
        // Always: the closures go when Tome revokes the registrations.
        #[unsafe(no_mangle)]
        pub extern "C" fn tome_mod_disable() {
            $crate::__rt::disable(|| { $($disable();)? })
        }
        $(
            #[unsafe(no_mangle)]
            pub extern "C" fn tome_mod_unload() {
                $crate::__rt::unload($unload)
            }
        )?
        #[unsafe(no_mangle)]
        pub extern "C" fn tome_buffer(size: u32) -> u32 {
            $crate::__rt::buffer(size) as u32
        }
        #[unsafe(no_mangle)]
        pub extern "C" fn tome_callback(id: u32, ptr: u32, len: u32) {
            $crate::__rt::callback(id, ptr as usize, len)
        }
        #[unsafe(no_mangle)]
        pub extern "C" fn tome_native(id: u32, args: u32, count: u32) -> u32 {
            $crate::__rt::native(id, args as usize, count) as u32
        }
    };
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn out_parameters_retry_with_the_full_length() {
        let text = b"a longer value than the first buffer";
        let mut calls = 0;
        let got = read_out(8, |out, cap| {
            calls += 1;
            let n = (cap as usize).min(text.len());
            unsafe { core::ptr::copy_nonoverlapping(text.as_ptr(), out, n) };
            text.len() as u32
        });
        assert_eq!(got.as_deref(), Some(&text[..]));
        assert_eq!(calls, 2);
        assert_eq!(read_out(8, |_, _| u32::MAX), None);
        assert_eq!(read_out(8, |_, _| 0), Some(Vec::new()));
    }

    #[test]
    fn errors_name_themselves() {
        assert_eq!(check(0), Ok(()));
        assert_eq!(check(2), Err(Error::NOT_DECLARED));
        assert_eq!(Error::NOT_DECLARED.to_string(), "not declared in tome.mod.json");
        assert_eq!(Error(99).to_string(), "error 99");
    }

    #[test]
    fn constants_come_from_tome_h() {
        assert_eq!(Event::MAP_LOAD.0, consts::TOME_EVENT_MAP_LOAD);
        assert_eq!(UiEvent::MAIN_MENU.0, 1);
        assert_eq!(FramePoint::CENTER.0, 4);
    }
}
