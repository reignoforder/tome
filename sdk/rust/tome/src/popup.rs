// SPDX-License-Identifier: MIT-0
//! Popups (API version 10): a title, a message and buttons over any screen.

use core::ffi::{c_char, c_void};
use std::ffi::CString;

use crate::util::cstr;
use crate::{Error, Result, Tome, check, guard, rt, sys};

/// A popup to show with [`Tome::popup`]. Strings are UTF-8 and may use the
/// game's color codes (|cffRRGGBB ... |r).
#[derive(Clone, Debug, Default)]
pub struct Popup<'a> {
    pub title: &'a str,
    pub text: &'a str,
    pub options: &'a [&'a str],
    /// Enter picks it.
    pub default_option: Option<usize>,
    /// Esc picks it.
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

fn index(option: Option<usize>) -> i32 {
    option.map_or(-1, |i| i as i32)
}

impl Tome {
    /// Shows a popup, or queues it behind others. `f` gets the chosen
    /// option, or None if the popup was closed with [`Tome::popup_close`].
    /// Returns its id.
    pub fn popup<F: Fn(Option<usize>) + 'static>(self, popup: &Popup, f: F) -> Result<u32> {
        unsafe extern "C" fn call<F: Fn(Option<usize>)>(_: *mut sys::tome_mod, _: u32, choice: i32, user: *mut c_void) {
            guard(|| {
                unsafe { (*(user as *const F))(usize::try_from(choice).ok()) };
                // It runs once; freed when the guard has finished.
                rt::finished(user);
            })
        }
        self.require(10)?;
        let show = self.raw().0.popup.ok_or(Error::UNAVAILABLE)?;
        // Tome copies the strings.
        let title = cstr(popup.title)?;
        let text = cstr(popup.text)?;
        let options = popup
            .options
            .iter()
            .map(|o| cstr(o))
            .collect::<Result<Vec<CString>>>()?;
        let pointers: Vec<*const c_char> = options.iter().map(|o| o.as_ptr()).collect();
        let raw = sys::tome_popup {
            size: core::mem::size_of::<sys::tome_popup>() as u32,
            title: title.as_ptr(),
            text: text.as_ptr(),
            options: pointers.as_ptr(),
            option_count: pointers.len() as i32,
            default_option: index(popup.default_option),
            cancel_option: index(popup.cancel_option),
        };
        let mut id = 0;
        rt::register(f, None, |user| unsafe {
            show(self.ptr(), &raw, Some(call::<F>), user, &mut id)
        })?;
        Ok(id)
    }

    /// Takes one of the mod's popups away; its callback gets None.
    pub fn popup_close(self, id: u32) -> Result<()> {
        self.require(10)?;
        check(unsafe { self.raw().0.popup_close.ok_or(Error::UNAVAILABLE)?(self.ptr(), id) })
    }
}
