//! Hooks on mapped game functions (API version 4; imports since 9): see
//! "Hooks" in tome.h. Handlers run on whatever thread called the function,
//! so they're plain `fn`s: keep state in atomics or locks, and log with
//! [`info!`](crate::info) and friends, which work on any thread.

use core::ffi::{c_char, c_void};
use std::borrow::Cow;

use crate::util::{cstr, text};
use crate::{Error, Result, Tome, check, guard, rt, sys};

/// A `tome_hook_phase`.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
#[repr(transparent)]
pub struct Phase(pub i32);

impl Phase {
    /// May change the arguments, or skip the function with a result.
    pub const BEFORE: Phase = Phase(1);
    /// Runs in place of the function; [`HookCall::call_next`] runs it.
    pub const REPLACE: Phase = Phase(2);
    /// May read and change the result.
    pub const AFTER: Phase = Phase(3);
}

pub type HookFn = fn(&mut HookCall);

/// A call of a hooked function (`tome_hook_call`).
#[repr(transparent)]
pub struct HookCall(sys::tome_hook_call);

impl HookCall {
    pub fn raw(&mut self) -> &mut sys::tome_hook_call {
        &mut self.0
    }

    /// The hooked function, e.g. "CTextFrame::SetText".
    pub fn name(&self) -> Cow<'_, str> {
        unsafe { text(self.0.name) }
    }

    /// The fixed arguments as 32-bit words, in the mapping signature's
    /// order (`this` first for thiscall; a double or 64-bit value takes two).
    pub fn args(&self) -> &[u32] {
        self.words(0, self.0.arg_count)
    }

    pub fn args_mut(&mut self) -> &mut [u32] {
        let count = self.0.arg_count.max(0) as usize;
        if self.0.args.is_null() {
            &mut []
        } else {
            unsafe { core::slice::from_raw_parts_mut(self.0.args, count) }
        }
    }

    /// For variadic functions: the caller's next stack words after the fixed ones.
    pub fn extra_args(&self) -> &[u32] {
        self.words(self.0.arg_count.max(0) as usize, self.0.extra_count)
    }

    fn words(&self, start: usize, count: i32) -> &[u32] {
        if self.0.args.is_null() || count <= 0 {
            return &[];
        }
        unsafe { core::slice::from_raw_parts(self.0.args.add(start), count as usize) }
    }

    pub fn arg(&self, index: usize) -> u32 {
        self.args()[index]
    }

    pub fn set_arg(&mut self, index: usize, value: u32) {
        self.args_mut()[index] = value;
    }

    pub fn arg_f32(&self, index: usize) -> f32 {
        f32::from_bits(self.arg(index))
    }

    pub fn set_arg_f32(&mut self, index: usize, value: f32) {
        self.set_arg(index, value.to_bits());
    }

    /// A double or 64-bit argument: two words from `index`, low half first.
    pub fn arg_u64(&self, index: usize) -> u64 {
        self.arg(index) as u64 | (self.arg(index + 1) as u64) << 32
    }

    pub fn set_arg_u64(&mut self, index: usize, value: u64) {
        self.set_arg(index, value as u32);
        self.set_arg(index + 1, (value >> 32) as u32);
    }

    pub fn arg_f64(&self, index: usize) -> f64 {
        f64::from_bits(self.arg_u64(index))
    }

    pub fn set_arg_f64(&mut self, index: usize, value: f64) {
        self.set_arg_u64(index, value.to_bits());
    }

    /// A string argument (a `const char*`), if not NULL.
    ///
    /// # Safety
    /// The argument must be a NUL-terminated string the game keeps during the call.
    pub unsafe fn arg_str(&self, index: usize) -> Option<Cow<'_, str>> {
        let p = self.arg(index) as usize as *const c_char;
        if p.is_null() { None } else { Some(unsafe { text(p) }) }
    }

    /// eax: the return value (and edx, the upper half of a 64-bit one).
    pub fn result(&self) -> u32 {
        self.0.result
    }

    pub fn set_result(&mut self, value: u32) {
        self.0.result = value;
    }

    pub fn result_u64(&self) -> u64 {
        self.0.result as u64 | (self.0.result_high as u64) << 32
    }

    pub fn set_result_u64(&mut self, value: u64) {
        self.0.result = value as u32;
        self.0.result_high = (value >> 32) as u32;
    }

    /// A float or double result (st(0)).
    pub fn result_f64(&self) -> f64 {
        self.0.result_float
    }

    pub fn set_result_f64(&mut self, value: f64) {
        self.0.result_float = value;
    }

    /// From a BEFORE handler: skip the REPLACE handlers and the function,
    /// returning `result` (or the float result) instead.
    pub fn skip_original(&mut self) {
        self.0.skip_original = 1;
    }

    pub fn skips_original(&self) -> bool {
        self.0.skip_original != 0
    }

    /// Where the function returns to.
    pub fn return_address(&self) -> usize {
        self.0.return_address
    }

    /// From a REPLACE handler: runs the rest of the chain with the current
    /// arguments and stores its result. Not calling it skips them.
    pub fn call_next(&mut self) {
        if let Some(next) = rt::api().and_then(|a| a.hook_call_next) {
            unsafe { next(&mut self.0) };
        }
    }
}

unsafe extern "C" fn call(_: *mut sys::tome_mod, call: *mut sys::tome_hook_call, user: *mut c_void) {
    guard(|| {
        let f: HookFn = unsafe { core::mem::transmute::<*mut c_void, HookFn>(user) };
        if let Some(call) = unsafe { (call as *mut HookCall).as_mut() } {
            f(call)
        }
    })
}

impl Tome {
    /// Adds `f` to the chain of the mapped function or import `name`, in
    /// `phase`; higher priorities run first. List `name` in tome.mod.json
    /// "requires".
    pub fn hook(self, name: &str, phase: Phase, priority: i32, f: HookFn) -> Result<()> {
        self.require(4)?;
        let name = cstr(name)?;
        let hook = self.raw().0.hook.ok_or(Error::UNAVAILABLE)?;
        check(unsafe {
            hook(
                self.ptr(),
                name.as_ptr(),
                phase.0,
                priority,
                Some(call),
                f as *mut c_void,
            )
        })
    }

    /// Removes a handler added with the same name, phase and function.
    pub fn unhook(self, name: &str, phase: Phase, f: HookFn) -> Result<()> {
        self.require(4)?;
        let name = cstr(name)?;
        let unhook = self.raw().0.unhook.ok_or(Error::UNAVAILABLE)?;
        check(unsafe { unhook(self.ptr(), name.as_ptr(), phase.0, Some(call), f as *mut c_void) })
    }
}
