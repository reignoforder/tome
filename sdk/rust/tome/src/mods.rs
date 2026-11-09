// SPDX-License-Identifier: MIT-0
//! Versions and the installed mods (API version 20): what a mod that
//! installs and updates others needs. Tome's own rules for versions, so a
//! mod picks releases the way Tome will check them.

use core::cmp::Ordering;
use core::ffi::c_char;
use core::mem::size_of;

use crate::util::{cstr, text};
use crate::{Error, Result, Tome, check, sys};

/// Which kind of version [`Tome::version_compare`] and [`Tome::version_match`]
/// read (`tome_version_kind`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VersionKind {
    /// Semantic versions: mods' `"version"` and `"depends"`, and Tome's own
    /// ([`Tome::version`]).
    Mod,
    /// Game versions: `"game"` in `tome.mod.json` (`"1.27.1.7085"`, `"1.27b"`).
    Game,
}

impl VersionKind {
    fn raw(self) -> i32 {
        match self {
            VersionKind::Mod => sys::tome_version_kind::TOME_VERSION_MOD.0 as i32,
            VersionKind::Game => sys::tome_version_kind::TOME_VERSION_GAME.0 as i32,
        }
    }
}

/// An installed mod's state (`tome_mod_state`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ModState {
    Active,
    /// Turned off by the player, or safe mode.
    Disabled,
    /// Its code waits for the player to allow it (the mod menu).
    Approval,
    /// Held off by a multiplayer game until it ends.
    Waiting,
    /// Can't run: see [`ModInfo::reason`].
    Error,
}

/// One installed mod, as [`Tome::list_mods`] reports it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ModInfo {
    /// "" if its `tome.mod.json` couldn't be read.
    pub id: String,
    pub name: String,
    /// What runs; "" if its `tome.mod.json` couldn't be read.
    pub version: String,
    /// Its entry in [`Tome::mods_dir`]: a folder's name or a package's file name.
    pub folder: String,
    /// Where its files are (for a package, where Tome unpacked it).
    pub dir: String,
    pub state: ModState,
    /// Why it isn't running, for people; "" if it is.
    pub reason: String,
    /// The player wants it on, and its code is allowed.
    pub enabled: bool,
    /// A `.tome` package file.
    pub package: bool,
    /// `"sync": "required"`.
    pub sync_required: bool,
    /// Changes to its assets wait for the game to restart.
    pub restart_pending: bool,
    /// The ids it stands in for (`"provides"`).
    pub provides: Vec<String>,
}

/// What [`Tome::check_metadata`] says of a `tome.mod.json`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MetadataCheck {
    /// It would run here (if what it `"depends"` on is installed).
    Runs,
    /// Valid, but not for this game, edition, Tome or build: why.
    NotHere(String),
    /// Not a valid `tome.mod.json`: why.
    Invalid(String),
}

impl Tome {
    /// Compares two versions by Tome's rules; [`Error::INVALID_ARGUMENT`]
    /// if either isn't one (a mod version needs all three numbers).
    pub fn version_compare(self, kind: VersionKind, a: &str, b: &str) -> Result<Ordering> {
        self.require(20)?;
        let f = self.api.version_compare.ok_or(Error::UNAVAILABLE)?;
        let (a, b) = (cstr(a)?, cstr(b)?);
        let mut order = 0i32;
        let r = unsafe { f(kind.raw(), a.as_ptr(), b.as_ptr(), &mut order) };
        check(r)?;
        Ok(order.cmp(&0))
    }

    /// Whether `version` is in `range`, as Tome checks `"depends"`, `"tome"`
    /// and `"game"`; `None` if either can't be parsed (or the running Tome
    /// is too old to say).
    pub fn version_match(self, kind: VersionKind, range: &str, version: &str) -> Option<bool> {
        self.require(20).ok()?;
        let f = self.api.version_match?;
        let (range, version) = (cstr(range).ok()?, cstr(version).ok()?);
        match unsafe { f(kind.raw(), range.as_ptr(), version.as_ptr()) } {
            1 => Some(true),
            0 => Some(false),
            _ => None,
        }
    }

    /// Every installed mod as of Tome's last look, in the order Tome found
    /// them (this mod too). [`crate::Event::MODS_CHANGED`] says when it changed.
    pub fn list_mods(self) -> Result<Vec<ModInfo>> {
        self.require(20)?;
        let f = self.api.list_mods.ok_or(Error::UNAVAILABLE)?;
        let size = size_of::<sys::tome_mod_info>() as u32;
        loop {
            let count = unsafe { f(self.ptr(), core::ptr::null_mut(), size, 0) };
            if count < 0 {
                return Err(Error::FAILED);
            }
            // One spare place: if a mod was added in between, the count
            // says so and the list is asked again.
            let mut out: Vec<sys::tome_mod_info> = Vec::with_capacity(count as usize + 1);
            let max = count + 1;
            let filled = unsafe { f(self.ptr(), out.as_mut_ptr(), size, max) };
            if filled < 0 {
                return Err(Error::FAILED);
            }
            if filled > max {
                continue;
            }
            unsafe { out.set_len(filled as usize) };
            // Copied now: the strings last only until the next call.
            return Ok(out.iter().map(|m| unsafe { mod_info(m) }).collect());
        }
    }

    /// The folder mods are installed in (`tome/mods`). Put a new mod
    /// together elsewhere ([`Tome::config_dir`]) and move it in whole
    /// (`std::fs::rename`), so Tome never reads half of it. A mod with code
    /// waits for the player to allow it ([`ModState::Approval`]).
    pub fn mods_dir(self) -> String {
        if self.require(20).is_err() {
            return String::new();
        }
        self.api
            .mods_dir
            .map(|f| unsafe { text(f()).into_owned() })
            .unwrap_or_default()
    }

    /// Checks a `tome.mod.json` (a release's, before installing it) as Tome
    /// checks an installed one: this game, edition, Tome and the names it
    /// `"requires"`; not its `"depends"` (compare [`Tome::list_mods`] with
    /// [`Tome::version_match`]).
    pub fn check_metadata(self, json: &str) -> Result<MetadataCheck> {
        self.require(20)?;
        let f = self.api.check_metadata.ok_or(Error::UNAVAILABLE)?;
        let mut reason = vec![0u8; 2048];
        let r = unsafe {
            f(
                json.as_ptr() as *const c_char,
                json.len(),
                reason.as_mut_ptr() as *mut c_char,
                reason.len(),
            )
        };
        let end = reason.iter().position(|&b| b == 0).unwrap_or(reason.len());
        let why = String::from_utf8_lossy(&reason[..end]).into_owned();
        Ok(match r {
            1 => MetadataCheck::Runs,
            0 => MetadataCheck::NotHere(why),
            _ => MetadataCheck::Invalid(why),
        })
    }
}

unsafe fn mod_info(m: &sys::tome_mod_info) -> ModInfo {
    let state = match m.state {
        0 => ModState::Active,
        1 => ModState::Disabled,
        2 => ModState::Approval,
        3 => ModState::Waiting,
        _ => ModState::Error,
    };
    let provides = if m.provides.is_null() || m.provides_count <= 0 {
        Vec::new()
    } else {
        unsafe { core::slice::from_raw_parts(m.provides, m.provides_count as usize) }
            .iter()
            .map(|&p| unsafe { text(p).into_owned() })
            .collect()
    };
    unsafe {
        ModInfo {
            id: text(m.id).into_owned(),
            name: text(m.name).into_owned(),
            version: text(m.version).into_owned(),
            folder: text(m.folder).into_owned(),
            dir: text(m.dir).into_owned(),
            state,
            reason: text(m.reason).into_owned(),
            enabled: m.enabled != 0,
            package: m.package != 0,
            sync_required: m.sync_required != 0,
            restart_pending: m.restart_pending != 0,
            provides,
        }
    }
}
