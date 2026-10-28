//! Services and messages between mods (API version 11): see tome.h. A
//! service is a table of functions with a C layout, so mods in C and Rust
//! can use each other's:
//!
//! ```ignore
//! #[repr(C)]
//! pub struct CounterApi {
//!     pub size: u32,
//!     pub add: extern "C" fn(n: i32) -> i32,
//! }
//! static API: CounterApi = CounterApi { size: size_of::<CounterApi>() as u32, add };
//! tome.service_register("counter.api", 1, &API)?;
//! ```

use core::ffi::{c_char, c_void};

use crate::util::{cstr, text};
use crate::{Error, Result, Tome, check, guard, rt, sys};

impl Tome {
    /// Offers `table` to other mods under `name` ("<mod id>.<what>") at a
    /// compatibility `version`. Withdrawn on disable.
    pub fn service_register<T: Sync + 'static>(self, name: &str, version: u32, table: &'static T) -> Result<()> {
        self.require(11)?;
        let name = cstr(name)?;
        let register = self.raw().0.service_register.ok_or(Error::UNAVAILABLE)?;
        check(unsafe { register(self.ptr(), name.as_ptr(), version, table as *const T as *const c_void) })
    }

    pub fn service_unregister(self, name: &str) -> Result<()> {
        self.require(11)?;
        let name = cstr(name)?;
        check(unsafe { self.raw().0.service_unregister.ok_or(Error::UNAVAILABLE)?(self.ptr(), name.as_ptr()) })
    }

    /// The table another mod offers under `name` at `version` (0 for any).
    /// Keep it only until [`Tome::on_service`] says it's gone, unless the
    /// provider is in tome.mod.json "depends".
    ///
    /// # Safety
    /// `T` must be the layout the provider documents for that version.
    pub unsafe fn service_get<T>(self, name: &str, version: u32) -> Option<&'static T> {
        self.require(11).ok()?;
        let name = cstr(name).ok()?;
        let table = unsafe { self.raw().0.service_get?(self.ptr(), name.as_ptr(), version) };
        unsafe { (table as *const T).as_ref() }
    }

    /// Calls `f` with true when the service `name` appears and false when
    /// it goes away; at once if it exists already.
    pub fn on_service<F: Fn(bool) + 'static>(self, name: &str, f: F) -> Result<()> {
        unsafe extern "C" fn call<F: Fn(bool)>(
            _: *mut sys::tome_mod,
            _: *const c_char,
            available: i32,
            user: *mut c_void,
        ) {
            guard(|| unsafe { (*(user as *const F))(available != 0) })
        }
        self.require(11)?;
        let name = cstr(name)?;
        let on_service = self.raw().0.on_service.ok_or(Error::UNAVAILABLE)?;
        rt::register(f, None, |user| unsafe {
            on_service(self.ptr(), name.as_ptr(), Some(call::<F>), user)
        })
    }

    /// Calls `f` for every message other mods publish on `topic`.
    pub fn subscribe<F: Fn(&str, &[u8]) + 'static>(self, topic: &str, f: F) -> Result<()> {
        unsafe extern "C" fn call<F: Fn(&str, &[u8])>(
            _: *mut sys::tome_mod,
            topic: *const c_char,
            data: *const c_void,
            size: usize,
            user: *mut c_void,
        ) {
            guard(|| {
                let bytes = if data.is_null() || size == 0 {
                    &[][..]
                } else {
                    unsafe { core::slice::from_raw_parts(data as *const u8, size) }
                };
                unsafe { (*(user as *const F))(&text(topic), bytes) }
            })
        }
        self.require(11)?;
        let topic = cstr(topic)?;
        let subscribe = self.raw().0.subscribe.ok_or(Error::UNAVAILABLE)?;
        rt::register(f, None, |user| unsafe {
            subscribe(self.ptr(), topic.as_ptr(), Some(call::<F>), user)
        })
    }

    /// Sends `data` to the other mods' subscribers of `topic` before
    /// returning; returns how many it reached.
    pub fn publish(self, topic: &str, data: &[u8]) -> usize {
        if self.require(11).is_err() {
            return 0;
        }
        let (Ok(topic), Some(publish)) = (cstr(topic), self.raw().0.publish) else {
            return 0;
        };
        let n = unsafe { publish(self.ptr(), topic.as_ptr(), data.as_ptr() as *const c_void, data.len()) };
        n.max(0) as usize
    }
}
