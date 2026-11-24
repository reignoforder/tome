// SPDX-License-Identifier: MIT-0
//! Game events (API version 5; more in 8 and 12): see `tome_event` in
//! tome.h for when each one fires and which fields of [`EventInfo`] it fills.

use core::ffi::c_void;
use core::mem::{offset_of, size_of};
use std::borrow::Cow;

use crate::util::text;
use crate::{Error, Result, Tome, guard, rt, sys};

/// A `tome_event`. Shown by name in `{:?}`: `MAP_LOAD`.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
#[repr(transparent)]
pub struct Event(pub i32);

impl core::fmt::Debug for Event {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self.name() {
            Some(name) => f.write_str(name),
            None => write!(f, "Event({})", self.0),
        }
    }
}

impl Event {
    pub const MAP_LOAD: Event = Event(1);
    pub const GAME_START: Event = Event(2);
    pub const GAME_END: Event = Event(3);
    pub const TICK: Event = Event(4);
    pub const GAME_TICK: Event = Event(5);
    pub const SELECTION: Event = Event(6);
    pub const CHAT: Event = Event(7);
    pub const PAUSE: Event = Event(8);
    pub const RESUME: Event = Event(9);
    pub const UNIT_DEATH: Event = Event(10);
    pub const UNIT_TRAINED: Event = Event(11);
    pub const CONSTRUCTION_FINISHED: Event = Event(12);
    pub const UPGRADE_FINISHED: Event = Event(13);
    pub const RESEARCH_FINISHED: Event = Event(14);
    pub const HERO_LEVEL: Event = Event(15);
    pub const UNIT_SUMMONED: Event = Event(16);
    pub const ITEM_PICKUP: Event = Event(17);
    pub const ITEM_DROP: Event = Event(18);
    pub const ITEM_USE: Event = Event(19);
    pub const PLAYER_VICTORY: Event = Event(20);
    pub const PLAYER_DEFEAT: Event = Event(21);
    pub const PLAYER_LEFT: Event = Event(22);
    pub const GAME_SAVE: Event = Event(23);
    pub const GAME_LOADED: Event = Event(24);
    pub const UNIT_ATTACKED: Event = Event(25);
    pub const UNIT_RESCUED: Event = Event(26);
    pub const CONSTRUCTION_STARTED: Event = Event(27);
    pub const CONSTRUCTION_CANCELLED: Event = Event(28);
    pub const UPGRADE_STARTED: Event = Event(29);
    pub const UPGRADE_CANCELLED: Event = Event(30);
    pub const TRAINING_STARTED: Event = Event(31);
    pub const TRAINING_CANCELLED: Event = Event(32);
    pub const RESEARCH_STARTED: Event = Event(33);
    pub const RESEARCH_CANCELLED: Event = Event(34);
    pub const HERO_SKILL: Event = Event(35);
    pub const HERO_REVIVABLE: Event = Event(36);
    pub const HERO_REVIVE_STARTED: Event = Event(37);
    pub const HERO_REVIVE_CANCELLED: Event = Event(38);
    pub const HERO_REVIVED: Event = Event(39);
    pub const UNIT_ORDER: Event = Event(40);
    pub const UNIT_LOADED: Event = Event(41);
    pub const UNIT_SOLD: Event = Event(42);
    pub const ITEM_SOLD: Event = Event(43);
    pub const ITEM_PAWNED: Event = Event(44);
    pub const UNIT_OWNER_CHANGED: Event = Event(45);
    pub const SPELL_CHANNEL: Event = Event(46);
    pub const SPELL_CAST: Event = Event(47);
    pub const SPELL_EFFECT: Event = Event(48);
    pub const SPELL_FINISH: Event = Event(49);
    pub const SPELL_END_CAST: Event = Event(50);
    pub const UNIT_DAMAGED: Event = Event(51);
    pub const UNIT_ACQUIRED_TARGET: Event = Event(52);
    pub const UNIT_DECAY: Event = Event(53);
    pub const UNIT_DETECTED: Event = Event(54);
    pub const UNIT_HIDDEN: Event = Event(55);
    pub const ESC_PRESSED: Event = Event(56);
    pub const ARROW_KEY: Event = Event(57);
    pub const DESTRUCTABLE_DEATH: Event = Event(58);
    pub const ITEM_DEATH: Event = Event(59);
    pub const DIALOG_CLICK: Event = Event(60);
    pub const SKILL_MENU: Event = Event(61);
    pub const BUILD_MENU: Event = Event(62);
    pub const LEVEL_END: Event = Event(63);
    pub const UNIT_CREATED: Event = Event(64);
    pub const UNIT_REMOVED: Event = Event(65);
    pub const HERO_XP: Event = Event(66);
    pub const PLAYER_RESOURCE: Event = Event(67);
    pub const DAY: Event = Event(68);
    pub const NIGHT: Event = Event(69);
    /// The installed mods changed ([`crate::Tome::list_mods`]). No fields.
    pub const MODS_CHANGED: Event = Event(70);

    /// The constant's name ("MAP_LOAD"), or None for one this version doesn't know.
    pub fn name(self) -> Option<&'static str> {
        Some(match self.0 {
            1 => "MAP_LOAD",
            2 => "GAME_START",
            3 => "GAME_END",
            4 => "TICK",
            5 => "GAME_TICK",
            6 => "SELECTION",
            7 => "CHAT",
            8 => "PAUSE",
            9 => "RESUME",
            10 => "UNIT_DEATH",
            11 => "UNIT_TRAINED",
            12 => "CONSTRUCTION_FINISHED",
            13 => "UPGRADE_FINISHED",
            14 => "RESEARCH_FINISHED",
            15 => "HERO_LEVEL",
            16 => "UNIT_SUMMONED",
            17 => "ITEM_PICKUP",
            18 => "ITEM_DROP",
            19 => "ITEM_USE",
            20 => "PLAYER_VICTORY",
            21 => "PLAYER_DEFEAT",
            22 => "PLAYER_LEFT",
            23 => "GAME_SAVE",
            24 => "GAME_LOADED",
            25 => "UNIT_ATTACKED",
            26 => "UNIT_RESCUED",
            27 => "CONSTRUCTION_STARTED",
            28 => "CONSTRUCTION_CANCELLED",
            29 => "UPGRADE_STARTED",
            30 => "UPGRADE_CANCELLED",
            31 => "TRAINING_STARTED",
            32 => "TRAINING_CANCELLED",
            33 => "RESEARCH_STARTED",
            34 => "RESEARCH_CANCELLED",
            35 => "HERO_SKILL",
            36 => "HERO_REVIVABLE",
            37 => "HERO_REVIVE_STARTED",
            38 => "HERO_REVIVE_CANCELLED",
            39 => "HERO_REVIVED",
            40 => "UNIT_ORDER",
            41 => "UNIT_LOADED",
            42 => "UNIT_SOLD",
            43 => "ITEM_SOLD",
            44 => "ITEM_PAWNED",
            45 => "UNIT_OWNER_CHANGED",
            46 => "SPELL_CHANNEL",
            47 => "SPELL_CAST",
            48 => "SPELL_EFFECT",
            49 => "SPELL_FINISH",
            50 => "SPELL_END_CAST",
            51 => "UNIT_DAMAGED",
            52 => "UNIT_ACQUIRED_TARGET",
            53 => "UNIT_DECAY",
            54 => "UNIT_DETECTED",
            55 => "UNIT_HIDDEN",
            56 => "ESC_PRESSED",
            57 => "ARROW_KEY",
            58 => "DESTRUCTABLE_DEATH",
            59 => "ITEM_DEATH",
            60 => "DIALOG_CLICK",
            61 => "SKILL_MENU",
            62 => "BUILD_MENU",
            63 => "LEVEL_END",
            64 => "UNIT_CREATED",
            65 => "UNIT_REMOVED",
            66 => "HERO_XP",
            67 => "PLAYER_RESOURCE",
            68 => "DAY",
            69 => "NIGHT",
            70 => "MODS_CHANGED",
            _ => return None,
        })
    }

    /// The API version that introduced the event.
    pub fn api_version(self) -> u32 {
        match self.0 {
            i32::MIN..=5 => 5,
            6..=45 => 8,
            46..=69 => 12,
            _ => 20,
        }
    }
}

/// What an event is about, valid during the handler. Fields the running
/// Tome doesn't fill (an older version) read as empty, 0 or None.
#[derive(Clone, Copy)]
pub struct EventInfo<'a>(&'a sys::tome_event_info);

macro_rules! fields {
    ($($(#[$meta:meta])* $name:ident: $ty:ty;)*) => {
        $(
            $(#[$meta])*
            pub fn $name(&self) -> $ty {
                if self.has(offset_of!(sys::tome_event_info, $name), size_of::<$ty>()) {
                    self.0.$name as $ty
                } else {
                    Default::default()
                }
            }
        )*
    };
}

macro_rules! strings {
    ($($(#[$meta:meta])* $name:ident;)*) => {
        $(
            $(#[$meta])*
            pub fn $name(&self) -> Cow<'a, str> {
                if self.has(offset_of!(sys::tome_event_info, $name), size_of::<*const u8>()) {
                    unsafe { text(self.0.$name) }
                } else {
                    Cow::Borrowed("")
                }
            }
        )*
    };
}

macro_rules! units {
    ($($(#[$meta:meta])* $name:ident;)*) => {
        $(
            $(#[$meta])*
            pub fn $name(&self) -> Option<Unit<'a>> {
                if !self.has(offset_of!(sys::tome_event_info, $name), size_of::<*const u8>()) {
                    return None;
                }
                unsafe { self.0.$name.as_ref() }.map(Unit::from_raw)
            }
        )*
    };
}

impl<'a> EventInfo<'a> {
    /// # Safety
    /// `raw` must be what Tome passed to an event handler, used during the call.
    pub unsafe fn from_raw(raw: &'a sys::tome_event_info) -> EventInfo<'a> {
        EventInfo(raw)
    }

    pub fn raw(&self) -> &'a sys::tome_event_info {
        self.0
    }

    fn has(&self, offset: usize, len: usize) -> bool {
        self.0.size as usize >= offset + len
    }

    pub fn event(&self) -> Event {
        Event(self.0.event)
    }

    fields! {
        /// TICK: real seconds since the previous tick. GAME_TICK: the game
        /// time this step advances.
        delta: f64;
        /// TICK: frames so far, from 1.
        frame: u32;
        /// GAME_TICK, GAME_END: seconds of game time since the game started.
        game_time: f64;
        /// CHAT, PAUSE, RESUME, players' events: the player slot (0 = Player
        /// 1), -1 for a trigger or the game.
        player: i32;
        /// CHAT: a `tome_chat_recipient`.
        recipient: i32;
        /// The type an event is about ('Rhme', 'pinv'), else 0.
        object_id: u32;
        /// HERO_LEVEL, HERO_XP, HERO_SKILL.
        level: i32;
        /// UNIT_ORDER: the order id.
        order_id: u32;
        /// UNIT_ORDER: a `tome_order_type`.
        order_type: i32;
        /// A target point.
        x: f64;
        y: f64;
        /// UNIT_DAMAGED: the damage. HERO_XP: the experience. PLAYER_RESOURCE:
        /// the new value. DAY, NIGHT: the time of day.
        amount: f64;
        /// JASS handle ids of `item` and `destructable`, if the map has one.
        item_handle: u32;
        destructable_handle: u32;
        /// ARROW_KEY: a `tome_arrow_key`.
        key: i32;
        /// DIALOG_CLICK: the dialog's and the button's handle ids.
        dialog: u32;
        button: u32;
        /// PLAYER_RESOURCE: a `tome_resource`, and its value before.
        resource: i32;
        previous: f64;
        /// MAP_LOAD, GAME_START, GAME_END: locals in the map script that
        /// shadow a global with another type (the memhack pattern).
        script_shadows: u32;
    }

    strings! {
        /// MAP_LOAD, GAME_START, GAME_END: the map's name, path, description.
        map_name;
        map_path;
        map_description;
        player_name;
        /// CHAT: the message.
        text;
        object_name;
        /// GAME_SAVE, GAME_LOADED: the saved game.
        save_name;
    }

    units! {
        /// Unit events: the unit.
        unit;
        /// The other unit involved (killer, trainer, attacker...).
        other;
        /// UNIT_SOLD: the buyer.
        third;
    }

    /// ARROW_KEY: pressed (true) or released.
    pub fn pressed(&self) -> bool {
        self.has(offset_of!(sys::tome_event_info, pressed), 4) && self.0.pressed != 0
    }

    /// The item (the game's object) an event is about, else None.
    pub fn item(&self) -> Option<*mut c_void> {
        if !self.has(offset_of!(sys::tome_event_info, item), 4) {
            return None;
        }
        Some(self.0.item).filter(|p| !p.is_null())
    }

    /// The destructable (the game's object) an event is about, else None.
    pub fn destructable(&self) -> Option<*mut c_void> {
        if !self.has(offset_of!(sys::tome_event_info, destructable), 4) {
            return None;
        }
        Some(self.0.destructable).filter(|p| !p.is_null())
    }

    /// SELECTION: the selected units.
    pub fn units(&self) -> impl Iterator<Item = Unit<'a>> + 'a {
        let ok = self.has(offset_of!(sys::tome_event_info, leader), 4) && !self.0.units.is_null();
        let mut count = if ok { self.0.unit_count as usize } else { 0 };
        let base = self.0.units as *const u8;
        // Tome's tome_unit may be bigger than this one: step by its size. A
        // smaller one (an older Tome) can't be read as this one.
        let stride = if count > 0 {
            unsafe { (*self.0.units).size as usize }
        } else {
            0
        };
        if stride < size_of::<sys::tome_unit>() {
            count = 0;
        }
        (0..count).map(move |i| Unit::from_raw(unsafe { &*(base.add(i * stride) as *const sys::tome_unit) }))
    }

    /// SELECTION: the index of the active unit in `units`.
    pub fn leader(&self) -> Option<usize> {
        if !self.has(offset_of!(sys::tome_event_info, leader), 4) {
            return None;
        }
        usize::try_from(self.0.leader)
            .ok()
            .filter(|&l| l < self.0.unit_count as usize)
    }
}

/// A unit as the game has it during the call (`tome_unit`).
#[derive(Clone, Copy)]
pub struct Unit<'a>(&'a sys::tome_unit);

impl<'a> Unit<'a> {
    pub(crate) fn from_raw(raw: &'a sys::tome_unit) -> Unit<'a> {
        Unit(raw)
    }

    pub fn raw(&self) -> &'a sys::tome_unit {
        self.0
    }

    /// The game's unit object: the same while the unit exists. Don't keep it.
    pub fn object(&self) -> *mut c_void {
        self.0.object
    }

    /// Its JASS handle id if the map script has one, else 0.
    pub fn handle(&self) -> u32 {
        self.0.handle
    }

    /// The unit type, e.g. 'hpea'; [`crate::type_code`] makes "hpea".
    pub fn type_id(&self) -> u32 {
        self.0.type_id
    }

    /// The type's name in the game's language.
    pub fn name(&self) -> Cow<'a, str> {
        unsafe { text(self.0.name) }
    }

    /// The owner's player slot (0 = Player 1).
    pub fn owner(&self) -> i32 {
        self.0.owner
    }

    pub fn is_structure(&self) -> bool {
        self.0.flags & sys::tome_unit_flag::TOME_UNIT_STRUCTURE.0 != 0
    }

    pub fn is_hero(&self) -> bool {
        self.0.flags & sys::tome_unit_flag::TOME_UNIT_HERO.0 != 0
    }

    pub fn is_dead(&self) -> bool {
        self.0.flags & sys::tome_unit_flag::TOME_UNIT_DEAD.0 != 0
    }

    pub fn to_owned(&self) -> OwnedUnit {
        OwnedUnit {
            object: self.object(),
            handle: self.handle(),
            type_id: self.type_id(),
            name: self.name().into_owned(),
            owner: self.owner(),
            flags: self.0.flags,
        }
    }
}

impl core::fmt::Display for Unit<'_> {
    /// "Footman (hfoo) of player 1"
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(
            f,
            "{} ({}) of player {}",
            self.name(),
            crate::type_code(self.type_id()),
            self.owner() + 1
        )
    }
}

/// A copy of a unit's fields, from [`Tome::selection`].
#[derive(Clone, Debug)]
pub struct OwnedUnit {
    pub object: *mut c_void,
    pub handle: u32,
    pub type_id: u32,
    pub name: String,
    pub owner: i32,
    pub flags: u32,
}

impl Tome {
    /// Calls `f` whenever `event` happens (on the main thread).
    pub fn on_event<F: Fn(&EventInfo) + 'static>(self, event: Event, f: F) -> Result<()> {
        unsafe extern "C" fn call<F: Fn(&EventInfo)>(
            _: *mut sys::tome_mod,
            info: *const sys::tome_event_info,
            user: *mut c_void,
        ) {
            guard(|| {
                if let Some(info) = unsafe { info.as_ref() } {
                    unsafe { (*(user as *const F))(&EventInfo(info)) }
                }
            })
        }
        self.require(event.api_version())?;
        let on_event = self.raw().0.on_event.ok_or(Error::UNAVAILABLE)?;
        rt::register(f, None, |user| unsafe {
            on_event(self.ptr(), event.0, Some(call::<F>), user)
        })
    }
}
