// SPDX-License-Identifier: MIT-0
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
    /// `TOME_EDITION_REIGN_OF_CHAOS`, `TOME_EDITION_FROZEN_THRONE`, or
    /// `TOME_EDITION_UNKNOWN` (also from a Tome older than API version 15).
    pub edition: i32,
}

pub fn game_info() -> GameInfo {
    // [size][build_name][file_version][edition], the strings after it in the block.
    let Some(block) = read_out(256, |out, cap| unsafe { raw::game_info(out, cap) }) else {
        return GameInfo { edition: consts::TOME_EDITION_UNKNOWN, ..GameInfo::default() };
    };
    #[repr(C)]
    #[derive(Clone, Copy, Default)]
    struct Raw {
        size: u32,
        build_name: u32,
        file_version: u32,
        edition: i32,
    }
    let mut raw: Raw = events::read_struct(&block);
    if (raw.size as usize) < core::mem::size_of::<Raw>() {
        raw.edition = consts::TOME_EDITION_UNKNOWN;
    }
    // The pointers are addresses in `block`, which is alive here.
    unsafe {
        GameInfo {
            build_name: events::text(raw.build_name).into_owned(),
            file_version: events::text(raw.file_version).into_owned(),
            edition: raw.edition,
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

// --- game objects (API version 16) -------------------------------------------
//
// Units, items, destructables and players are handles of this module (like
// `OwnedUnit::object`); properties are `consts::TOME_PROPERTY_*`, values
// f64. Changing the game in a multiplayer game needs a "sync": "required" mod.

/// What an object is now (`consts::TOME_OBJECT_*`); `TOME_OBJECT_NONE` if it's gone.
pub fn object_type(object: u32) -> i32 {
    unsafe { raw::object_type(object) }
}

/// The object a JASS handle id stands for and its type, if it's a unit,
/// item, destructable or player.
pub fn object_of(jass_handle: u32) -> Option<(u32, i32)> {
    let mut kind = consts::TOME_OBJECT_NONE;
    let object = unsafe { raw::object_of(jass_handle, &mut kind) };
    (object != 0).then_some((object, kind))
}

/// The JASS handle id the map script has for an object, or 0 (Tome never makes one).
pub fn handle_of(object: u32) -> u32 {
    unsafe { raw::handle_of(object) }
}

/// The player in a slot (0 = Player 1).
pub fn player_object(slot: i32) -> Option<u32> {
    let object = unsafe { raw::player_object(slot) };
    (object != 0).then_some(object)
}

/// The current game's host: its player slot (0 = Player 1), the same on
/// every player's computer; `None` outside a game.
pub fn host_player() -> Option<i32> {
    let slot = unsafe { raw::host_player() };
    (slot >= 0).then_some(slot)
}

/// Which kind of version [`version_compare`] and [`version_match`] read
/// (`tome_version_kind`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VersionKind {
    /// Semantic versions: mods' `"version"` and `"depends"`, and Tome's own.
    Mod,
    /// Game versions: `"game"` in `tome.mod.json` (`"1.27.1.7085"`, `"1.27b"`).
    Game,
}

fn version_kind(kind: VersionKind) -> i32 {
    match kind {
        VersionKind::Mod => consts::TOME_VERSION_MOD,
        VersionKind::Game => consts::TOME_VERSION_GAME,
    }
}

/// Compares two versions by Tome's own rules; an error if either isn't one.
pub fn version_compare(kind: VersionKind, a: &str, b: &str) -> Result<core::cmp::Ordering> {
    let (a_ptr, a_len) = str_arg(a);
    let (b_ptr, b_len) = str_arg(b);
    let mut order = 0i32;
    check(unsafe { raw::version_compare(version_kind(kind), a_ptr, a_len, b_ptr, b_len, &mut order) })?;
    Ok(order.cmp(&0))
}

/// What [`check_metadata`] says of a `tome.mod.json`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MetadataCheck {
    /// It would run here (if what it `"depends"` on is installed).
    Runs,
    /// Valid, but not for this game, edition, Tome or build: why.
    NotHere(String),
    /// Not a valid `tome.mod.json`: why.
    Invalid(String),
}

/// Checks a `tome.mod.json` (a release's, before installing it) as Tome
/// checks an installed one. Up to 64 KiB.
pub fn check_metadata(text: &str) -> MetadataCheck {
    let mut reason = vec![0u8; 1024];
    let r = unsafe {
        raw::check_metadata(
            text.as_ptr(),
            text.len() as u32,
            reason.as_mut_ptr(),
            reason.len() as u32,
        )
    };
    let end = reason.iter().position(|&b| b == 0).unwrap_or(reason.len());
    let why = String::from_utf8_lossy(&reason[..end]).into_owned();
    match r {
        1 => MetadataCheck::Runs,
        0 => MetadataCheck::NotHere(why),
        _ => MetadataCheck::Invalid(why),
    }
}

/// Whether `version` is in `range`, as Tome checks `"depends"`, `"tome"` and
/// `"game"`; `None` if either can't be parsed.
pub fn version_match(kind: VersionKind, range: &str, version: &str) -> Option<bool> {
    let (r_ptr, r_len) = str_arg(range);
    let (v_ptr, v_len) = str_arg(version);
    match unsafe { raw::version_match(version_kind(kind), r_ptr, r_len, v_ptr, v_len) } {
        1 => Some(true),
        0 => Some(false),
        _ => None,
    }
}

/// A unit as the selection describes it, if it's a live unit.
pub fn unit_info(unit: u32) -> Option<OwnedUnit> {
    let block = read_out(256, |out, cap| unsafe { raw::unit_info(unit, out, cap) })?;
    let raw = block.get(..core::mem::size_of::<events::RawUnit>())?;
    Some(Unit::from_raw(events::read_struct(raw)).to_owned())
}

/// A property's value (`consts::TOME_PROPERTY_*`).
pub fn get_property(object: u32, property: i32) -> Result<f64> {
    let mut value = 0.0;
    check(unsafe { raw::get_property(object, property, &mut value) }).map(|()| value)
}

/// Changes a property, as its JASS native would.
pub fn set_property(object: u32, property: i32, value: f64) -> Result<()> {
    check(unsafe { raw::set_property(object, property, value) })
}

/// A property that takes a key (an ability's type id; 1 for a hero's
/// attribute with bonuses).
pub fn get_keyed_property(object: u32, property: i32, key: u32) -> Result<f64> {
    let mut value = 0.0;
    check(unsafe { raw::get_keyed_property(object, property, key, &mut value) }).map(|()| value)
}

/// Changes a property that takes a key.
pub fn set_keyed_property(object: u32, property: i32, key: u32, value: f64) -> Result<()> {
    check(unsafe { raw::set_keyed_property(object, property, key, value) })
}

/// Every unit, item, destructable or player there is now
/// (`consts::TOME_OBJECT_*`), as handles; empty outside a game.
pub fn list_objects(kind: i32) -> Vec<u32> {
    let total = unsafe { raw::list_objects(kind, core::ptr::null_mut(), 0) };
    if total <= 0 {
        return Vec::new();
    }
    let mut out = vec![0u32; total as usize];
    let n = unsafe { raw::list_objects(kind, out.as_mut_ptr(), total) };
    out.truncate(n.clamp(0, total) as usize);
    out
}

/// One argument of `jass_call`, as the native's signature has it. Handles
/// are JASS handle ids (`handle_of`), not this module's object handles.
#[derive(Clone, Copy, Debug)]
pub enum JassArg<'a> {
    Int(i32),
    Bool(bool),
    Real(f32),
    Handle(u32),
    Code(u32),
    /// `None` is JASS null.
    Str(Option<&'a str>),
}

/// What a native returned: the word as the signature has it, and the text
/// of a string result.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct JassValue {
    pub word: u32,
    pub text: Option<String>,
}

impl JassValue {
    pub fn int(&self) -> i32 {
        self.word as i32
    }
    pub fn real(&self) -> f32 {
        f32::from_bits(self.word)
    }
    pub fn boolean(&self) -> bool {
        self.word != 0
    }
}

/// Runs a JASS native by name (Tome checks the arguments as for DLL mods).
pub fn jass_call(name: &str, args: &[JassArg]) -> Result<JassValue> {
    let mut texts = Vec::new();
    for arg in args {
        if let JassArg::Str(Some(s)) = arg {
            let mut bytes = s.as_bytes().to_vec();
            bytes.push(0);
            texts.push(bytes);
        }
    }
    let mut next_text = texts.iter();
    let words: Vec<u32> = args
        .iter()
        .map(|arg| match *arg {
            JassArg::Int(v) => v as u32,
            JassArg::Bool(v) => v as u32,
            JassArg::Real(v) => v.to_bits(),
            JassArg::Handle(v) | JassArg::Code(v) => v,
            JassArg::Str(None) => 0,
            JassArg::Str(Some(_)) => next_text.next().map_or(0, |t| t.as_ptr() as usize as u32),
        })
        .collect();
    let (ptr, len) = str_arg(name);
    let mut word = 0u32;
    let mut text = vec![0u8; 4096];
    let count = i32::try_from(words.len()).unwrap_or(i32::MAX);
    check(unsafe {
        raw::jass_call(ptr, len, words.as_ptr(), count, &mut word, text.as_mut_ptr(), text.len() as u32)
    })?;
    // A string result's word points at `text`.
    let text_result = (word != 0 && word as usize == text.as_ptr() as usize).then(|| {
        let end = text.iter().position(|&b| b == 0).unwrap_or(text.len());
        String::from_utf8_lossy(&text[..end]).into_owned()
    });
    Ok(JassValue { word, text: text_result })
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

// --- messages between players ----------------------------------------------

/// Sends `data` (up to `consts::TOME_NET_MAX_MESSAGE` bytes) to this mod on
/// every player's computer, this one included, at the same point of the
/// game. Only in a game from its first step (the first game tick, not yet at
/// game start); in multiplayer only for a "sync": "required" mod.
pub fn net_send(data: &[u8]) -> Result<()> {
    let size = u32::try_from(data.len()).unwrap_or(u32::MAX);
    check(unsafe { raw::net_send(data.as_ptr(), size) })
}

/// `f` gets every message this mod's copies send (the sender's player slot,
/// the data, whether a replay plays it back). Untrusted input from other
/// players: check what it says.
pub fn on_net_message(f: impl Fn(i32, &[u8], bool) + 'static) -> Result<()> {
    rt::register(Callback::Net(Rc::new(f)), |id| unsafe { raw::on_net_message(id) }).map(drop)
}

// --- timers and desync checks ------------------------------------------------

/// Which clock a timer runs on (`tome_clock`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Clock {
    /// Real time, checked once per frame drawn, in menus and games: for UI.
    Real,
    /// Game time: only in a game, follows the game speed, stops while it's
    /// paused, ends with the game; fires in the same game step on every
    /// player's computer if each started it in the same one.
    Game,
}

/// Calls `f` (with the timer's id) `delay` seconds from now, then every
/// `interval` seconds until `timer_stop` (0: only once). Returns its id.
pub fn timer_start(clock: Clock, delay: f64, interval: f64, f: impl Fn(u32) + 'static) -> Result<u32> {
    let clock = match clock {
        Clock::Real => consts::TOME_CLOCK_REAL,
        Clock::Game => consts::TOME_CLOCK_GAME,
    };
    let mut timer = 0u32;
    let id = rt::register(Callback::Timer(Rc::new(f)), |cb| unsafe {
        raw::timer_start(clock, delay, interval, cb, &mut timer)
    })?;
    rt::note_timer(timer, id);
    Ok(timer)
}

/// Stops one of the mod's timers.
pub fn timer_stop(timer: u32) -> Result<()> {
    check(unsafe { raw::timer_stop(timer) })?;
    rt::forget_timer(timer);
    Ok(())
}

/// Gives Tome the mod's checksum for desync checks (tome.h, `on_checksum`):
/// `f` returns a hash of the mod's game state ([`Hash`] makes one), which
/// must be the same on every player's computer. For a "sync": "required"
/// mod, called every few seconds of a multiplayer game.
pub fn on_checksum(f: impl Fn() -> u64 + 'static) -> Result<()> {
    let id = rt::register(Callback::Checksum(Rc::new(f)), |id| unsafe { raw::on_checksum(id) })?;
    rt::replace_checksum(Some(id));
    Ok(())
}

/// Removes the mod's checksum.
pub fn clear_checksum() -> Result<()> {
    check(unsafe { raw::on_checksum(0) })?;
    rt::replace_checksum(None);
    Ok(())
}

/// A hash for desync checks, as tome.h's `tome_hash_*` (FNV-1a, 64 bits):
/// `Hash::new().u32(wave).f64(boss_life).value()`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Hash(pub u64);

impl Default for Hash {
    fn default() -> Self {
        Self::new()
    }
}

impl Hash {
    /// The starting value (`TOME_HASH_INIT`).
    pub const fn new() -> Self {
        Hash(0xcbf2_9ce4_8422_2325)
    }
    /// Adds bytes.
    pub fn bytes(mut self, data: &[u8]) -> Self {
        for &b in data {
            self.0 ^= u64::from(b);
            self.0 = self.0.wrapping_mul(0x0000_0100_0000_01b3);
        }
        self
    }
    /// Adds a number, little-endian (as `tome_hash_u32`).
    pub fn u32(self, value: u32) -> Self {
        self.bytes(&value.to_le_bytes())
    }
    /// Adds a number, little-endian.
    pub fn i32(self, value: i32) -> Self {
        self.bytes(&value.to_le_bytes())
    }
    /// Adds a number, little-endian (as `tome_hash_u64`).
    pub fn u64(self, value: u64) -> Self {
        self.bytes(&value.to_le_bytes())
    }
    /// A number's bits (0.0 and -0.0 differ).
    pub fn f64(self, value: f64) -> Self {
        self.u64(value.to_bits())
    }
    /// The hash.
    pub fn value(self) -> u64 {
        self.0
    }
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
    #[test]
    fn hash_matches_fnv1a() {
        // FNV-1a 64 of "a" is af63dc4c8601ec8c; of nothing, the start value.
        assert_eq!(super::Hash::new().value(), 0xcbf2_9ce4_8422_2325);
        assert_eq!(super::Hash::new().bytes(b"a").value(), 0xaf63_dc4c_8601_ec8c);
        assert_eq!(super::Hash::new().u32(1).value(), super::Hash::new().bytes(&[1, 0, 0, 0]).value());
    }

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
