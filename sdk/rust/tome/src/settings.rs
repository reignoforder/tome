// SPDX-License-Identifier: MIT-0
//! Settings and keybinds (API version 7): the mod's config.schema.json and
//! tome.mod.json "keybinds"; see "Settings and keybinds" in tome.h.

use core::ffi::{c_char, c_void};

use crate::util::{cstr, text};
use crate::{Error, Result, Tome, check, guard, rt, sys};

impl Tome {
    /// A boolean setting, or `fallback` if there's none of that type.
    pub fn setting_bool(self, key: &str, fallback: bool) -> bool {
        self.setting(key, |api, m, k| unsafe {
            api.setting_bool.map(|f| f(m, k, fallback as i32) != 0)
        })
        .unwrap_or(fallback)
    }

    pub fn setting_int(self, key: &str, fallback: i32) -> i32 {
        self.setting(key, |api, m, k| unsafe { api.setting_int.map(|f| f(m, k, fallback)) })
            .unwrap_or(fallback)
    }

    pub fn setting_number(self, key: &str, fallback: f64) -> f64 {
        self.setting(key, |api, m, k| unsafe {
            api.setting_number.map(|f| f(m, k, fallback))
        })
        .unwrap_or(fallback)
    }

    /// A string setting, or None if there's no such string setting (or it's empty).
    pub fn setting_string(self, key: &str) -> Option<String> {
        self.setting(key, |api, m, k| {
            let get = api.setting_string?;
            copy_out(|buffer, size| unsafe { get(m, k, buffer, size) })
        })
    }

    fn setting<T>(
        self,
        key: &str,
        f: impl FnOnce(&sys::tome_api, *mut sys::tome_mod, *const c_char) -> Option<T>,
    ) -> Option<T> {
        self.require(7).ok()?;
        let key = cstr(key).ok()?;
        f(self.raw().0, self.ptr(), key.as_ptr())
    }

    /// Changes a setting as if the player had, and saves it.
    pub fn set_setting_bool(self, key: &str, value: bool) -> Result<()> {
        self.set(key, |api, m, k| unsafe {
            api.set_setting_bool.map(|f| f(m, k, value as i32))
        })
    }

    pub fn set_setting_int(self, key: &str, value: i32) -> Result<()> {
        self.set(key, |api, m, k| unsafe { api.set_setting_int.map(|f| f(m, k, value)) })
    }

    pub fn set_setting_number(self, key: &str, value: f64) -> Result<()> {
        self.set(key, |api, m, k| unsafe {
            api.set_setting_number.map(|f| f(m, k, value))
        })
    }

    pub fn set_setting_string(self, key: &str, value: &str) -> Result<()> {
        let value = cstr(value)?;
        self.set(key, |api, m, k| unsafe {
            api.set_setting_string.map(|f| f(m, k, value.as_ptr()))
        })
    }

    fn set(
        self,
        key: &str,
        f: impl FnOnce(&sys::tome_api, *mut sys::tome_mod, *const c_char) -> Option<sys::tome_result>,
    ) -> Result<()> {
        self.require(7)?;
        let key = cstr(key)?;
        check(f(self.raw().0, self.ptr(), key.as_ptr()).ok_or(Error::UNAVAILABLE)?)
    }

    /// Calls `f` with the key of each setting that changes (the mod menu,
    /// set_setting_*, or the settings file).
    pub fn on_setting_changed<F: Fn(&str) + 'static>(self, f: F) -> Result<()> {
        unsafe extern "C" fn call<F: Fn(&str)>(_: *mut sys::tome_mod, key: *const c_char, user: *mut c_void) {
            guard(|| unsafe { (*(user as *const F))(&text(key)) })
        }
        self.require(7)?;
        let on_changed = self.raw().0.on_setting_changed.ok_or(Error::UNAVAILABLE)?;
        rt::register(f, None, |user| unsafe { on_changed(self.ptr(), Some(call::<F>), user) })
    }

    /// Calls `f` when the player presses the key bound to `id` (an entry of
    /// tome.mod.json "keybinds").
    pub fn on_keybind<F: Fn() + 'static>(self, id: &str, f: F) -> Result<()> {
        unsafe extern "C" fn call<F: Fn()>(_: *mut sys::tome_mod, _: *const c_char, user: *mut c_void) {
            guard(|| unsafe { (*(user as *const F))() })
        }
        self.require(7)?;
        // Registering the same id again replaces the callback.
        let key = format!("key:{id}");
        let id = cstr(id)?;
        let on_keybind = self.raw().0.on_keybind.ok_or(Error::UNAVAILABLE)?;
        rt::register(f, Some(key), |user| unsafe {
            on_keybind(self.ptr(), id.as_ptr(), Some(call::<F>), user)
        })
    }

    /// The key bound to `id` now ("Ctrl+H", "None"), or None if the mod
    /// didn't declare it.
    pub fn keybind_name(self, id: &str) -> Option<String> {
        self.setting(id, |api, m, k| {
            let get = api.keybind_name?;
            copy_out(|buffer, size| unsafe { get(m, k, buffer, size) })
        })
    }
}

/// Calls a snprintf-style getter (returns the full length) for the whole text.
fn copy_out(get: impl Fn(*mut c_char, usize) -> usize) -> Option<String> {
    let len = get(core::ptr::null_mut(), 0);
    if len == 0 {
        return None;
    }
    let mut buffer = vec![0u8; len + 1];
    let len = get(buffer.as_mut_ptr() as *mut c_char, buffer.len()).min(len);
    buffer.truncate(len);
    Some(String::from_utf8_lossy(&buffer).into_owned())
}
