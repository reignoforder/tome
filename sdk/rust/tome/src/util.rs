use core::ffi::{CStr, c_char};
use std::borrow::Cow;
use std::ffi::CString;

use crate::{Error, Result};

/// A string for Tome; one with a NUL in it can't be passed.
pub(crate) fn cstr(s: &str) -> Result<CString> {
    CString::new(s).map_err(|_| Error::INVALID_ARGUMENT)
}

/// For log lines: a NUL would cut the line short, so it's shown instead.
pub(crate) fn cstring_lossy(s: &str) -> CString {
    CString::new(s).unwrap_or_else(|_| CString::new(s.replace('\0', "\\0")).unwrap_or_default())
}

/// Text from Tome (UTF-8; anything else is replaced), "" for NULL. The
/// caller picks the lifetime the documentation gives the pointer.
pub(crate) unsafe fn text<'a>(p: *const c_char) -> Cow<'a, str> {
    if p.is_null() {
        Cow::Borrowed("")
    } else {
        unsafe { CStr::from_ptr(p) }.to_string_lossy()
    }
}

/// Text Tome keeps for the whole process (game info, mapping names).
pub(crate) unsafe fn static_str(p: *const c_char) -> &'static str {
    if p.is_null() {
        ""
    } else {
        unsafe { CStr::from_ptr(p) }.to_str().unwrap_or("")
    }
}
