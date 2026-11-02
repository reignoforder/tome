// SPDX-License-Identifier: MIT-0
//! JASS values for natives, in the engine's ABI (tome.h, "JASS native ABI"):
//! one 32-bit argument per JASS parameter, results in eax.
//!
//! | JASS      | argument        | result      |
//! |-----------|-----------------|-------------|
//! | integer   | [`Integer`]     | [`Integer`] |
//! | boolean   | [`Boolean`]     | [`Boolean`] |
//! | real      | [`Real`]        | [`RealRet`] |
//! | string    | [`JString`]     | [`StringRet`] |
//! | code      | [`Code`]        |             |
//! | handle    | [`Handle`]      | [`Handle`]  |
//! | nothing   |                 | (none)      |
//!
//! Write natives with [`native!`](crate::native), which wraps the body in
//! the panic guard, and attach them with
//! [`Tome::register_native`](crate::Tome::register_native).

use core::ffi::c_void;
use std::borrow::Cow;

use crate::util::{cstr, text};
use crate::{Error, Result, check, rt};

pub type Integer = i32;

/// A JASS boolean: 0 or 1.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Boolean(pub i32);

impl Boolean {
    pub fn get(self) -> bool {
        self.0 != 0
    }
}

impl From<bool> for Boolean {
    fn from(value: bool) -> Self {
        Boolean(value as i32)
    }
}

impl From<Boolean> for bool {
    fn from(value: Boolean) -> Self {
        value.get()
    }
}

/// A real argument: the engine passes a pointer to the value.
#[repr(transparent)]
#[derive(Clone, Copy, Debug)]
pub struct Real(*const f32);

impl Real {
    pub fn get(self) -> f32 {
        if self.0.is_null() { 0.0 } else { unsafe { *self.0 } }
    }
}

/// A real result: the float's bits in eax.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct RealRet(pub u32);

impl From<f32> for RealRet {
    fn from(value: f32) -> Self {
        RealRet(value.to_bits())
    }
}

/// A string argument (the engine's string object; null for a null string).
#[repr(transparent)]
#[derive(Debug)]
pub struct JString(*const c_void);

impl JString {
    /// The text, or None for a null string. It belongs to the game and may
    /// go away once the native returns: keep a copy if needed.
    pub fn get(&self) -> Option<Cow<'_, str>> {
        let api = rt::api()?;
        if !crate::rt_tome()?.has(6) {
            return None;
        }
        let p = unsafe { api.jass_string?(self.0) };
        if p.is_null() { None } else { Some(unsafe { text(p) }) }
    }
}

/// A string result, made from inside the native that returns it.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct StringRet(pub u32);

impl StringRet {
    pub const NULL: StringRet = StringRet(0);

    /// The result for `text` (invalid UTF-8 can't happen here; a NUL cuts
    /// it short). Null outside a native.
    pub fn new(text: &str) -> StringRet {
        let Some(api) = rt::api() else { return StringRet::NULL };
        if !crate::rt_tome().is_some_and(|t| t.has(6)) {
            return StringRet::NULL;
        }
        let text = text.split('\0').next().unwrap_or("");
        let Ok(text) = cstr(text) else { return StringRet::NULL };
        match api.jass_string_create {
            Some(create) => StringRet(unsafe { create(text.as_ptr()) }),
            None => StringRet::NULL,
        }
    }
}

impl From<&str> for StringRet {
    fn from(text: &str) -> Self {
        StringRet::new(text)
    }
}

/// A `code` argument: a function of the current map script. It can be kept
/// and run later (from events, clicks, commands) until that map ends.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, Default)]
pub struct Code(pub u32);

impl Code {
    pub fn is_null(self) -> bool {
        self.0 == 0
    }

    /// Runs it now, like TriggerEvaluate (it can't wait), and returns the
    /// boolean it returns (false for a function that returns nothing).
    pub fn call(self) -> Result<bool> {
        let api = rt::api().ok_or(Error::UNAVAILABLE)?;
        crate::rt_tome().ok_or(Error::UNAVAILABLE)?.require(6)?;
        let mut result = 0;
        check(unsafe { api.jass_code_call.ok_or(Error::UNAVAILABLE)?(self.0, &mut result) })?;
        Ok(result != 0)
    }

    /// Runs it on a JASS thread of its own, like TriggerExecute: it may
    /// wait, and this returns at its first wait.
    pub fn execute(self) -> Result<()> {
        let api = rt::api().ok_or(Error::UNAVAILABLE)?;
        crate::rt_tome().ok_or(Error::UNAVAILABLE)?.require(6)?;
        check(unsafe { api.jass_code_execute.ok_or(Error::UNAVAILABLE)?(self.0) })
    }
}

/// A handle id (unit, player, trigger...).
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, Default)]
pub struct Handle(pub u32);

/// Types a native can take.
pub trait NativeArg: sealed::Sealed {}
/// Types a native can return; `Nothing` is a native returning nothing.
pub trait NativeRet: sealed::Sealed {}

/// What a native returning nothing returns: 0.
pub type Nothing = i32;

mod sealed {
    pub trait Sealed {}
}

macro_rules! jass_types {
    (args: $($arg:ty),*; rets: $($ret:ty),*) => {
        $(impl sealed::Sealed for $arg {} impl NativeArg for $arg {})*
        $(impl NativeRet for $ret {})*
    };
}

impl sealed::Sealed for RealRet {}
impl sealed::Sealed for StringRet {}
jass_types!(args: Integer, Boolean, Real, JString, Code, Handle; rets: Integer, Boolean, RealRet, StringRet, Handle);

/// A function that can be a native: `extern "C" fn` of [`NativeArg`]s
/// returning a [`NativeRet`].
pub trait NativeFn: Copy {
    fn as_ptr(self) -> *mut c_void;
}

macro_rules! native_fn {
    ($($arg:ident),*) => {
        impl<R: NativeRet, $($arg: NativeArg),*> NativeFn for extern "C" fn($($arg),*) -> R {
            fn as_ptr(self) -> *mut c_void {
                self as *mut c_void
            }
        }
    };
}

native_fn!();
native_fn!(A);
native_fn!(A, B);
native_fn!(A, B, C);
native_fn!(A, B, C, D);
native_fn!(A, B, C, D, E);
native_fn!(A, B, C, D, E, F);
native_fn!(A, B, C, D, E, F, G);
native_fn!(A, B, C, D, E, F, G, H);
native_fn!(A, B, C, D, E, F, G, H, I);
native_fn!(A, B, C, D, E, F, G, H, I, J);
native_fn!(A, B, C, D, E, F, G, H, I, J, K);
native_fn!(A, B, C, D, E, F, G, H, I, J, K, L);

/// The result of a native's body: its value, or 0 for one returning nothing.
#[doc(hidden)]
pub trait IntoNative {
    type Out;
    fn into_native(self) -> Self::Out;
}

impl IntoNative for () {
    type Out = Nothing;
    fn into_native(self) -> Nothing {
        0
    }
}

macro_rules! into_native_self {
    ($($t:ty),*) => { $(impl IntoNative for $t { type Out = $t; fn into_native(self) -> $t { self } })* };
}
into_native_self!(Integer, Boolean, RealRet, StringRet, Handle);

#[doc(hidden)]
#[macro_export]
macro_rules! __native_ret {
    () => {
        ()
    };
    ($ret:ty) => {
        $ret
    };
}

/// Defines JASS natives in the engine's ABI, each body under the panic
/// guard. A native without a result type returns nothing. Each name is an
/// `extern "C" fn` pointer to pass to
/// [`Tome::register_native`](crate::Tome::register_native); declare the
/// natives in tome.mod.json "natives" too.
///
/// ```ignore
/// tome::native! {
///     /// native MyDouble takes real value returns real
///     fn my_double(value: Real) -> RealRet { (value.get() * 2.0).into() }
///     /// native MyLog takes integer value returns nothing
///     fn my_log(value: Integer) { tome::info!("{value}") }
/// }
/// // in enable:
/// tome.register_native("MyDouble", my_double)?;
/// ```
#[macro_export]
macro_rules! native {
    ($( $(#[$meta:meta])* $vis:vis fn $name:ident ( $($arg:ident : $ty:ty),* $(,)? ) $(-> $ret:ty)? $body:block )*) => {
        $(
            $(#[$meta])*
            #[allow(non_upper_case_globals)]
            $vis const $name: extern "C" fn($($ty),*)
                -> <$crate::__native_ret!($($ret)?) as $crate::jass::IntoNative>::Out = {
                #[allow(unused_braces)]
                extern "C" fn $name($($arg: $ty),*)
                    -> <$crate::__native_ret!($($ret)?) as $crate::jass::IntoNative>::Out
                {
                    $crate::jass::IntoNative::into_native(
                        $crate::guard(move || -> $crate::__native_ret!($($ret)?) { $body }))
                }
                $name
            };
        )*
    };
}
