//! Safe Rust API for [Tome](../../README.md) mods, on top of the raw
//! bindings in [`sys`] (`sdk/include/tome/tome.h`, which documents every
//! function in full).
//!
//! A mod is a `cdylib` built for `i686-pc-windows-gnu` that exports its
//! lifecycle with [`export_mod!`]:
//!
//! ```ignore
//! fn load(tome: tome::Tome) {
//!     tome::info!("loaded in Tome {}", tome.version());
//! }
//! tome::export_mod! { load: load }
//! ```
//!
//! - **Panics** are caught at every call from Tome, logged with where they
//!   happened, and reported to Tome as a crash: the mod is disabled with its
//!   dependents, as for a fault in a C mod. Threads a mod starts itself
//!   aren't guarded.
//! - **[`Tome`]** is the mod's handle, given to the lifecycle functions. It's
//!   `Copy` but not `Send`: Tome calls a mod on the game's main thread, and
//!   closures registered through it run there too, so they need not be
//!   `Send`. Capture it in closures to call Tome from them.
//! - **Callbacks** are closures (`Fn`; keep state in `Cell`s or `RefCell`s),
//!   except hooks, which run on any thread and are plain functions. Each is
//!   kept while Tome may call it (until `disable`, a registration that
//!   replaces it, or a popup's choice); register them in `enable`.
//! - **Versions:** every method checks that the running Tome has the
//!   function ([`Tome::has`]) and returns [`Error::UNAVAILABLE`] (or
//!   nothing) if not, so a mod built against a newer API still loads.

pub use tome_sys as sys;

mod rt;
#[doc(hidden)]
pub mod __rt {
    pub use crate::rt::{LoadResult, disable, enable, load, unload};
}

pub mod events;
pub mod hooks;
pub mod intermod;
pub mod jass;
pub mod popup;
mod settings;
pub mod ui;
mod util;

use core::ffi::c_void;
use core::fmt;
use core::marker::PhantomData;
use core::mem::offset_of;
use core::ptr::NonNull;
use std::borrow::Cow;

pub use events::{Event, EventInfo, Unit};
pub use hooks::{HookCall, Phase};
pub use jass::{Boolean, Code, Handle, Integer, JString, Real, RealRet, StringRet};
pub use popup::Popup;
pub use rt::{LoadResult, guard};

/// Keeps `value` (a closure for a raw callback) until Tome has revoked the
/// mod's callbacks after a disable, and returns its address, for `user`.
/// Main thread.
pub fn keep_until_disabled<T: 'static>(value: T) -> *mut c_void {
    rt::keep(value)
}
pub use ui::{Frame, FramePoint, UiEvent};
use util::{cstr, text};

/// The API version these bindings were made for (`TOME_API_VERSION`).
pub const API_VERSION: u32 = sys::TOME_API_VERSION;

/// A failure Tome reported (`tome_result`).
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub struct Error(pub u32);

impl Error {
    pub const INVALID_ARGUMENT: Error = Error(sys::tome_result::TOME_ERR_INVALID_ARGUMENT.0);
    pub const NOT_DECLARED: Error = Error(sys::tome_result::TOME_ERR_NOT_DECLARED.0);
    pub const NOT_OWNER: Error = Error(sys::tome_result::TOME_ERR_NOT_OWNER.0);
    pub const UNAVAILABLE: Error = Error(sys::tome_result::TOME_ERR_UNAVAILABLE.0);
    pub const WRONG_THREAD: Error = Error(sys::tome_result::TOME_ERR_WRONG_THREAD.0);
    pub const FAILED: Error = Error(sys::tome_result::TOME_ERR_FAILED.0);
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

pub(crate) fn check(result: sys::tome_result) -> Result<()> {
    if result == sys::tome_result::TOME_OK {
        Ok(())
    } else {
        Err(Error(result.0))
    }
}

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Level {
    Debug,
    Info,
    Warn,
    Error,
}

/// Writes a line to `tome.log`, tagged with the mod's id. Any thread; does
/// nothing before the mod is loaded. The [`info!`] family formats for it.
pub fn log(level: Level, message: &str) {
    let Some(log) = rt::api().and_then(|a| a.log) else {
        return;
    };
    let level = match level {
        Level::Debug => sys::tome_log_level::TOME_LOG_DEBUG,
        Level::Info => sys::tome_log_level::TOME_LOG_INFO,
        Level::Warn => sys::tome_log_level::TOME_LOG_WARN,
        Level::Error => sys::tome_log_level::TOME_LOG_ERROR,
    };
    let message = util::cstring_lossy(message);
    unsafe { log(rt::mod_ptr(), level, message.as_ptr()) };
}

#[macro_export]
macro_rules! debug { ($($arg:tt)*) => { $crate::log($crate::Level::Debug, &::std::format!($($arg)*)) }; }
#[macro_export]
macro_rules! info { ($($arg:tt)*) => { $crate::log($crate::Level::Info, &::std::format!($($arg)*)) }; }
#[macro_export]
macro_rules! warn { ($($arg:tt)*) => { $crate::log($crate::Level::Warn, &::std::format!($($arg)*)) }; }
#[macro_export]
macro_rules! error { ($($arg:tt)*) => { $crate::log($crate::Level::Error, &::std::format!($($arg)*)) }; }

/// The running game (`tome_game_info`).
#[derive(Clone, Copy, Debug)]
pub struct GameInfo {
    /// e.g. "1.27b"
    pub build_name: &'static str,
    /// e.g. "1.27.1.7085"
    pub file_version: &'static str,
    /// Where game.dll is loaded; add RVAs to it.
    pub game_base: usize,
}

/// An entry of the mapping database for the running build (`tome_symbol`).
#[derive(Clone, Copy, Debug)]
pub struct Symbol {
    pub name: &'static str,
    pub kind: SymbolKind,
    /// Functions, data, calls and imports: the address. Offsets and
    /// constants: the number.
    pub value: usize,
    pub rva: u32,
    /// "cdecl", "stdcall", "fastcall", "thiscall"; "" unless a function or import.
    pub convention: &'static str,
    /// C-style prototype, or "".
    pub signature: &'static str,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SymbolKind {
    Function,
    Data,
    Call,
    Offset,
    Constant,
    Import,
    Other(i32),
}

/// The mod's handle: see the crate documentation.
#[derive(Clone, Copy)]
pub struct Tome {
    api: &'static sys::tome_api,
    raw: NonNull<sys::tome_mod>,
    _main_thread: PhantomData<*const ()>,
}

/// The last function of each API version's table, to check `size` against.
macro_rules! version_end {
    ($field:ident) => {
        offset_of!(sys::tome_api, $field) + core::mem::size_of::<*const c_void>()
    };
}

impl Tome {
    pub(crate) fn new(api: &'static sys::tome_api, raw: NonNull<sys::tome_mod>) -> Tome {
        Tome {
            api,
            raw,
            _main_thread: PhantomData,
        }
    }

    /// The raw table and mod handle, for calls this crate doesn't wrap
    /// (yet): check [`Tome::has`] first, run callbacks under [`guard`], and
    /// pass closures with [`keep_until_disabled`].
    pub fn raw(self) -> (&'static sys::tome_api, *mut sys::tome_mod) {
        (self.api, self.raw.as_ptr())
    }

    /// The API version of the running Tome.
    pub fn api_version(self) -> u32 {
        self.api.api_version
    }

    /// True if the running Tome has the functions of API version `version`.
    pub fn has(self, version: u32) -> bool {
        let end = match version {
            0 | 1 => version_end!(register_native),
            2 => version_end!(resolve),
            3 => version_end!(frame_on_click),
            4 => version_end!(hook_call_next),
            5 => version_end!(ui_root),
            6 => version_end!(jass_code_execute),
            7 => version_end!(keybind_name),
            8 | 9 => version_end!(selection),
            10 => version_end!(restart_game),
            11 | 12 => version_end!(publish),
            _ => version_end!(map_script_shadows),
        };
        self.api.api_version >= version && self.api.size as usize >= end
    }

    pub(crate) fn require(self, version: u32) -> Result<()> {
        if self.has(version) {
            Ok(())
        } else {
            Err(Error::UNAVAILABLE)
        }
    }

    pub(crate) fn ptr(self) -> *mut sys::tome_mod {
        self.raw.as_ptr()
    }

    /// Tome's version, e.g. "0.1.0".
    pub fn version(self) -> Cow<'static, str> {
        unsafe { text(self.api.tome_version) }
    }

    pub fn game_info(self) -> GameInfo {
        let info = self
            .api
            .game_info
            .map(|f| unsafe { f() })
            .and_then(|p| unsafe { p.as_ref() });
        match info {
            Some(info) => unsafe {
                GameInfo {
                    build_name: util::static_str(info.build_name),
                    file_version: util::static_str(info.file_version),
                    game_base: info.game_base,
                }
            },
            None => GameInfo {
                build_name: "",
                file_version: "",
                game_base: 0,
            },
        }
    }

    /// The mod's id from tome.mod.json.
    pub fn id(self) -> String {
        self.string(self.api.mod_id)
    }

    /// The mod's version from tome.mod.json.
    pub fn mod_version(self) -> String {
        self.string(self.api.mod_version)
    }

    /// The mod's folder.
    pub fn dir(self) -> String {
        self.string(self.api.mod_dir)
    }

    /// `tome/mods/config/<id>`, created on demand: for files that survive
    /// updates of the mod.
    pub fn config_dir(self) -> String {
        self.string(self.api.config_dir)
    }

    fn string(self, f: Option<unsafe extern "C" fn(*mut sys::tome_mod) -> *const core::ffi::c_char>) -> String {
        f.map(|f| unsafe { text(f(self.ptr())).into_owned() })
            .unwrap_or_default()
    }

    /// Looks a name up in the mapping database ("CTextFrame::SetText"). List
    /// the names a mod can't do without in tome.mod.json "requires".
    pub fn resolve(self, name: &str) -> Option<Symbol> {
        self.require(2).ok()?;
        let name = cstr(name).ok()?;
        let sym = unsafe { self.api.resolve?(name.as_ptr()).as_ref()? };
        unsafe {
            Some(Symbol {
                name: util::static_str(sym.name),
                kind: match sym.kind {
                    1 => SymbolKind::Function,
                    2 => SymbolKind::Data,
                    3 => SymbolKind::Call,
                    4 => SymbolKind::Offset,
                    5 => SymbolKind::Constant,
                    6 => SymbolKind::Import,
                    other => SymbolKind::Other(other),
                },
                value: sym.value as usize,
                rva: sym.rva,
                convention: util::static_str(sym.convention),
                signature: util::static_str(sym.signature),
            })
        }
    }

    /// Attaches a JASS native declared in tome.mod.json "natives"; write
    /// it with [`native!`]. Call from `enable`.
    pub fn register_native<F: jass::NativeFn>(self, name: &str, f: F) -> Result<()> {
        let name = cstr(name)?;
        let register = self.api.register_native.ok_or(Error::UNAVAILABLE)?;
        check(unsafe { register(self.ptr(), name.as_ptr(), f.as_ptr()) })
    }

    /// Shows a message to the local player in a game, for `seconds`.
    pub fn display_text(self, text: &str, seconds: f32) -> Result<()> {
        self.require(5)?;
        let text = cstr(text)?;
        check(unsafe { self.api.display_text.ok_or(Error::UNAVAILABLE)?(text.as_ptr(), seconds) })
    }

    /// Adds the chat command `/name`; `f` gets the rest of the line, trimmed.
    pub fn register_command<F: Fn(&str) + 'static>(self, name: &str, f: F) -> Result<()> {
        unsafe extern "C" fn call<F: Fn(&str)>(
            _: *mut sys::tome_mod,
            args: *const core::ffi::c_char,
            user: *mut c_void,
        ) {
            guard(|| unsafe { (*(user as *const F))(&text(args)) })
        }
        self.require(5)?;
        let name = cstr(name)?;
        let register = self.api.register_command.ok_or(Error::UNAVAILABLE)?;
        rt::register(f, None, |user| unsafe {
            register(self.ptr(), name.as_ptr(), Some(call::<F>), user)
        })
    }

    /// Restarts the game (ask the player first).
    pub fn restart_game(self) -> Result<()> {
        self.require(10)?;
        check(unsafe { self.api.restart_game.ok_or(Error::UNAVAILABLE)?(self.ptr()) })
    }

    /// The local player's selection, and the index of the active unit.
    /// Empty outside a game.
    pub fn selection(self) -> (Vec<events::OwnedUnit>, Option<usize>) {
        if !self.has(8) {
            return (Vec::new(), None);
        }
        let Some(selection) = self.api.selection else {
            return (Vec::new(), None);
        };
        let mut units: Vec<sys::tome_unit> = vec![unsafe { core::mem::zeroed() }; 12];
        let mut leader = -1;
        let size = core::mem::size_of::<sys::tome_unit>() as u32;
        let count = unsafe { selection(units.as_mut_ptr(), size, units.len() as i32, &mut leader) };
        let count = (count.max(0) as usize).min(units.len());
        let owned = units[..count].iter().map(|u| Unit::from_raw(u).to_owned()).collect();
        (owned, usize::try_from(leader).ok().filter(|&l| l < count))
    }

    /// The current map script's locals that shadow a global with another
    /// type (the memhack pattern Tome blocks).
    pub fn map_script_shadows(self) -> Vec<ScriptShadow> {
        if !self.has(13) {
            return Vec::new();
        }
        let Some(get) = self.api.map_script_shadows else {
            return Vec::new();
        };
        let size = core::mem::size_of::<sys::tome_script_shadow>() as u32;
        let count = unsafe { get(core::ptr::null_mut(), size, 0) }.max(0) as usize;
        let mut items: Vec<sys::tome_script_shadow> = vec![unsafe { core::mem::zeroed() }; count];
        let filled = unsafe { get(items.as_mut_ptr(), size, count as i32) }.max(0) as usize;
        items.truncate(filled.min(count));
        items
            .iter()
            .map(|s| unsafe {
                ScriptShadow {
                    name: text(s.name).into_owned(),
                    local_type: text(s.local_type).into_owned(),
                    global_type: text(s.global_type).into_owned(),
                    line: s.line,
                }
            })
            .collect()
    }
}

/// The mod's handle from the globals, for wrappers that don't get one
/// (JASS values, frames, hook calls).
pub(crate) fn rt_tome() -> Option<Tome> {
    Some(Tome::new(rt::api()?, NonNull::new(rt::mod_ptr())?))
}

/// A local in the map script with a global's name and another type.
#[derive(Clone, Debug)]
pub struct ScriptShadow {
    pub name: String,
    pub local_type: String,
    pub global_type: String,
    /// Its line in war3map.j, from 1.
    pub line: i32,
}

/// A unit or item type id as its four characters: 'hpea' is "hpea".
pub fn type_code(id: u32) -> String {
    id.to_be_bytes().iter().map(|&b| b as char).collect()
}

#[cfg(test)]
mod tests;
