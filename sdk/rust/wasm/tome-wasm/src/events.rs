// SPDX-License-Identifier: MIT-0
//! Game events (`on_event`) and units, as Tome writes them into the
//! module's memory (`tome_wasm_event`, `tome_wasm_unit` in
//! `sdk/include/tome/tome_wasm.h`; `loader/src/core/wasm_layout.cpp`).
//! Units, items and destructables are handles: ids that tell objects apart,
//! never pointers.

use std::borrow::Cow;
use std::ffi::CStr;

/// A game event (`tome_event`; the values are in [`crate::consts`]).
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct Event(pub i32);

// The layouts Tome writes, for wasm32: pointers are u32 addresses in the
// module's memory, doubles 8-aligned (as clang lays out tome_wasm.h's).
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub(crate) struct RawUnit {
    size: u32,
    object: u32,
    handle: u32,
    type_id: u32,
    name: u32,
    owner: i32,
    flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub(crate) struct RawEvent {
    size: u32,
    event: i32,
    map_name: u32,
    map_path: u32,
    delta: f64,
    frame: u32,
    game_time: f64,
    map_description: u32,
    units: u32,
    unit_count: u32,
    leader: i32,
    player: i32,
    player_name: u32,
    text: u32,
    recipient: i32,
    unit: u32,
    other: u32,
    item: u32,
    object_id: u32,
    object_name: u32,
    level: i32,
    save_name: u32,
    third: u32,
    order_id: u32,
    order_type: i32,
    x: f64,
    y: f64,
    amount: f64,
    destructable: u32,
    item_handle: u32,
    destructable_handle: u32,
    key: i32,
    pressed: i32,
    dialog: u32,
    button: u32,
    resource: i32,
    previous: f64,
    script_shadows: u32,
}

// tome_wasm.h's asserts: Tome writes these offsets.
const _: () = {
    use core::mem::{offset_of, size_of};
    assert!(offset_of!(RawUnit, name) == 16);
    assert!(offset_of!(RawUnit, flags) == 24);
    assert!(size_of::<RawUnit>() == 28);
    assert!(offset_of!(RawEvent, delta) == 16);
    assert!(offset_of!(RawEvent, game_time) == 32);
    assert!(offset_of!(RawEvent, units) == 44);
    assert!(offset_of!(RawEvent, player) == 56);
    assert!(offset_of!(RawEvent, unit) == 72);
    assert!(offset_of!(RawEvent, item) == 80);
    assert!(offset_of!(RawEvent, third) == 100);
    assert!(offset_of!(RawEvent, x) == 112);
    assert!(offset_of!(RawEvent, amount) == 128);
    assert!(offset_of!(RawEvent, destructable) == 136);
    assert!(offset_of!(RawEvent, previous) == 168);
    assert!(offset_of!(RawEvent, script_shadows) == 176);
    assert!(size_of::<RawEvent>() == 184);
};

/// Reads a `T` from the first bytes of `data`, zeros for what's missing
/// (an older, shorter struct).
pub(crate) fn read_struct<T: Copy + Default>(data: &[u8]) -> T {
    let mut value = T::default();
    let n = data.len().min(core::mem::size_of::<T>());
    // T is plain old data (u32, i32 and f64 fields).
    unsafe { core::ptr::copy_nonoverlapping(data.as_ptr(), (&raw mut value).cast::<u8>(), n) };
    value
}

/// The NUL-terminated string at `addr` in the module's memory, if any.
///
/// # Safety
/// `addr` is 0 or a string Tome wrote that's still alive.
pub(crate) unsafe fn text<'a>(addr: u32) -> Cow<'a, str> {
    if addr == 0 {
        return Cow::Borrowed("");
    }
    unsafe { CStr::from_ptr(addr as usize as *const core::ffi::c_char) }.to_string_lossy()
}

/// An event's details (`tome_event_info`: which fields an event sets is
/// documented with the event in `tome.h`; the rest are 0 or empty). Valid
/// during the callback.
pub struct EventInfo<'a> {
    raw: RawEvent,
    _block: core::marker::PhantomData<&'a [u8]>,
}

macro_rules! fields {
    ($($name:ident: $ty:ty),* $(,)?) => {
        $(pub fn $name(&self) -> $ty { self.raw.$name as $ty })*
    };
}

macro_rules! strings {
    ($($name:ident),* $(,)?) => {
        $(pub fn $name(&self) -> Cow<'a, str> { unsafe { text(self.raw.$name) } })*
    };
}

macro_rules! units {
    ($($name:ident),* $(,)?) => {
        $(pub fn $name(&self) -> Option<Unit<'a>> { unsafe { Unit::at(self.raw.$name) } })*
    };
}

impl<'a> EventInfo<'a> {
    /// # Safety
    /// `data` is the block Tome wrote for an event callback.
    pub(crate) unsafe fn from_block(data: &'a [u8]) -> EventInfo<'a> {
        EventInfo {
            raw: read_struct(data),
            _block: core::marker::PhantomData,
        }
    }

    pub fn event(&self) -> Event {
        Event(self.raw.event)
    }

    fields! {
        delta: f64,
        frame: u32,
        game_time: f64,
        player: i32,
        recipient: i32,
        item: u32,
        object_id: u32,
        level: i32,
        order_id: u32,
        order_type: i32,
        x: f64,
        y: f64,
        amount: f64,
        destructable: u32,
        item_handle: u32,
        destructable_handle: u32,
        key: i32,
        dialog: u32,
        button: u32,
        resource: i32,
        previous: f64,
        script_shadows: u32,
    }

    strings! { map_name, map_path, map_description, player_name, text, object_name, save_name }

    units! { unit, other, third }

    /// Whether the key was pressed (`TOME_EVENT_ARROW_KEY`), not released.
    pub fn pressed(&self) -> bool {
        self.raw.pressed != 0
    }

    /// The units of a selection event.
    pub fn units(&self) -> impl Iterator<Item = Unit<'a>> + 'a {
        let (base, count) = (self.raw.units, self.raw.unit_count);
        let size = core::mem::size_of::<RawUnit>() as u32;
        (0..if base == 0 { 0 } else { count }).filter_map(move |i| unsafe { Unit::at(base.wrapping_add(i * size)) })
    }

    /// The index of the active unit in [`units`](Self::units), if there is one.
    pub fn leader(&self) -> Option<usize> {
        usize::try_from(self.raw.leader).ok()
    }
}

/// A unit (`tome_wasm_unit`).
#[derive(Clone, Copy)]
pub struct Unit<'a> {
    raw: RawUnit,
    _block: core::marker::PhantomData<&'a [u8]>,
}

impl<'a> Unit<'a> {
    /// # Safety
    /// `addr` is 0 or a unit Tome wrote that's still alive.
    unsafe fn at(addr: u32) -> Option<Unit<'a>> {
        if addr == 0 {
            return None;
        }
        let raw = unsafe { (addr as usize as *const RawUnit).read_unaligned() };
        Some(Unit {
            raw,
            _block: core::marker::PhantomData,
        })
    }

    pub(crate) fn from_raw(raw: RawUnit) -> Unit<'a> {
        Unit {
            raw,
            _block: core::marker::PhantomData,
        }
    }

    /// Tells units apart: the same unit has the same one while it exists.
    pub fn object(&self) -> u32 {
        self.raw.object
    }

    /// Its JASS handle id, or 0.
    pub fn handle(&self) -> u32 {
        self.raw.handle
    }

    /// Its type ('hfoo' is 0x68666F6F).
    pub fn type_id(&self) -> u32 {
        self.raw.type_id
    }

    pub fn name(&self) -> Cow<'a, str> {
        unsafe { text(self.raw.name) }
    }

    /// The owning player's number.
    pub fn owner(&self) -> i32 {
        self.raw.owner
    }

    pub fn is_structure(&self) -> bool {
        self.raw.flags & crate::consts::TOME_UNIT_STRUCTURE as u32 != 0
    }

    pub fn is_hero(&self) -> bool {
        self.raw.flags & crate::consts::TOME_UNIT_HERO as u32 != 0
    }

    pub fn is_dead(&self) -> bool {
        self.raw.flags & crate::consts::TOME_UNIT_DEAD as u32 != 0
    }

    /// A copy that outlives the callback.
    pub fn to_owned(&self) -> OwnedUnit {
        OwnedUnit {
            object: self.object(),
            handle: self.handle(),
            type_id: self.type_id(),
            name: self.name().into_owned(),
            owner: self.owner(),
            flags: self.raw.flags,
        }
    }
}

/// A [`Unit`] that's kept.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct OwnedUnit {
    pub object: u32,
    pub handle: u32,
    pub type_id: u32,
    pub name: String,
    pub owner: i32,
    pub flags: u32,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_event_reads_its_fields() {
        let mut block = vec![0u8; 184];
        block[4..8].copy_from_slice(&7i32.to_le_bytes());
        block[16..24].copy_from_slice(&0.5f64.to_le_bytes());
        block[112..120].copy_from_slice(&(-3.25f64).to_le_bytes());
        block[56..60].copy_from_slice(&2i32.to_le_bytes());
        let info = unsafe { EventInfo::from_block(&block) };
        assert_eq!(info.event(), Event(7));
        assert_eq!(info.delta(), 0.5);
        assert_eq!(info.x(), -3.25);
        assert_eq!(info.player(), 2);
        assert_eq!(info.map_name(), "");
        assert!(info.unit().is_none());
        assert_eq!(info.units().count(), 0);
        assert_eq!(info.leader(), Some(0));
    }

    #[test]
    fn a_short_block_reads_as_zeros() {
        let info = unsafe { EventInfo::from_block(&[1, 0, 0, 0, 9, 0]) };
        assert_eq!(info.event(), Event(9));
        assert_eq!(info.previous(), 0.0);
    }
}
