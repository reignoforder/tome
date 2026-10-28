//! The mod's side of the lifecycle: the API table Tome handed over, the
//! panic guard every callback runs under, and the closures registered with
//! Tome.

use core::ffi::c_void;
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicPtr, Ordering};
use std::any::Any;
use std::cell::{Cell, RefCell};
use std::panic::{self, AssertUnwindSafe};

use crate::{Level, Tome, sys};

// One mod per DLL. Set again by every tome_mod_load: after a crash Tome may
// load the same image again.
static API: AtomicPtr<sys::tome_api> = AtomicPtr::new(ptr::null_mut());
static MOD: AtomicPtr<sys::tome_mod> = AtomicPtr::new(ptr::null_mut());

pub(crate) fn api() -> Option<&'static sys::tome_api> {
    // Tome's table lives as long as the process.
    unsafe { API.load(Ordering::Acquire).as_ref() }
}

pub(crate) fn mod_ptr() -> *mut sys::tome_mod {
    MOD.load(Ordering::Acquire)
}

/// Runs `f`, turning a panic into a crash Tome's guard reports: the panic
/// hook has logged it, and TOME_EXCEPTION_MOD_FAILED makes Tome abandon the
/// call and disable the mod, as for a fault in C. An unwind must never
/// reach Tome or the game (Rust would abort the process there).
pub fn guard<R>(f: impl FnOnce() -> R) -> R {
    // Counts the mod's code running on this thread: closures that died
    // meanwhile are freed once it's done. A panic leaves the count up (Tome
    // unwinds past this); the next enable resets it.
    DEPTH.with(|d| d.set(d.get() + 1));
    let value = match panic::catch_unwind(AssertUnwindSafe(f)) {
        Ok(value) => value,
        Err(payload) => {
            drop(payload);
            fail()
        }
    };
    if DEPTH.with(|d| {
        d.set(d.get() - 1);
        d.get() == 0
    }) {
        collect();
    }
    value
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn RaiseException(code: u32, flags: u32, count: u32, args: *const usize);
}

const EXCEPTION_NONCONTINUABLE: u32 = 1;

fn fail() -> ! {
    unsafe { RaiseException(sys::TOME_EXCEPTION_MOD_FAILED, EXCEPTION_NONCONTINUABLE, 0, ptr::null()) };
    // Only without Tome's guard above (a thread the mod started itself).
    std::process::abort()
}

fn install_panic_hook() {
    panic::set_hook(Box::new(|info| {
        let message = info
            .payload()
            .downcast_ref::<&str>()
            .copied()
            .or_else(|| info.payload().downcast_ref::<String>().map(String::as_str))
            .unwrap_or("(no message)");
        let thread = std::thread::current();
        let place = info
            .location()
            .map(|l| format!(" at {}:{}", l.file(), l.line()))
            .unwrap_or_default();
        crate::log(
            Level::Error,
            &format!(
                "panicked{place}{}: {message}",
                thread.name().map(|n| format!(" (thread {n})")).unwrap_or_default()
            ),
        );
    }));
}

// Closures registered with Tome, owned here and passed to Tome as `user`.
// Only lifecycle and main-thread callbacks register them (Tome is !Send),
// so they live in the main thread's storage. One dies when Tome won't call
// it again: refused (the registration failed), replaced (on_click and
// on_keybind replace a button's or key's callback) or finished (a popup's
// runs once); it's freed once none of the mod's code runs on the thread.
// Tome revokes all of them right after tome_mod_disable: those retire and
// are freed at the next enable or at unload.
struct Entry {
    value: Box<dyn Any>,
    user: *mut c_void,
    key: Option<String>,
}

thread_local! {
    static LIVE: RefCell<Vec<Entry>> = const { RefCell::new(Vec::new()) };
    static DEAD: RefCell<Vec<Box<dyn Any>>> = const { RefCell::new(Vec::new()) };
    static RETIRED: RefCell<Vec<Box<dyn Any>>> = const { RefCell::new(Vec::new()) };
    static DEPTH: Cell<u32> = const { Cell::new(0) };
}

/// Keeps `value` until the mod is disabled; the pointer is its address.
pub(crate) fn keep<T: 'static>(value: T) -> *mut c_void {
    let boxed = Box::new(value);
    let user = &*boxed as *const T as *mut c_void;
    LIVE.with(|l| {
        l.borrow_mut().push(Entry {
            value: boxed,
            user,
            key: None,
        })
    });
    user
}

/// Registers `value` with Tome through `f` (which gets its address for
/// `user`): kept while Tome may call it. `key` names what a later
/// registration with the same key replaces.
pub(crate) fn register<T: 'static>(
    value: T,
    key: Option<String>,
    f: impl FnOnce(*mut c_void) -> sys::tome_result,
) -> crate::Result<()> {
    let user = keep(value);
    let result = f(user);
    let registered = result == sys::tome_result::TOME_OK;
    LIVE.with(|l| {
        let mut live = l.borrow_mut();
        let mut dead = Vec::new();
        if !registered {
            // Tome keeps no callback it refused.
            live.retain_mut(|e| e.user != user || keep_entry(&mut dead, e));
        } else if let Some(key) = key {
            live.retain_mut(|e| e.user == user || e.key.as_ref() != Some(&key) || keep_entry(&mut dead, e));
            if let Some(e) = live.iter_mut().find(|e| e.user == user) {
                e.key = Some(key);
            }
        }
        DEAD.with(|d| d.borrow_mut().append(&mut dead));
    });
    if DEPTH.with(Cell::get) == 0 {
        collect();
    }
    crate::check(result)
}

// For retain_mut: moves the entry's value out to `dead` and drops the entry.
fn keep_entry(dead: &mut Vec<Box<dyn Any>>, e: &mut Entry) -> bool {
    dead.push(core::mem::replace(&mut e.value, Box::new(())));
    false
}

/// A callback that runs once (a popup's) is done.
pub(crate) fn finished(user: *mut c_void) {
    LIVE.with(|l| {
        let mut live = l.borrow_mut();
        let mut dead = Vec::new();
        live.retain_mut(|e| e.user != user || keep_entry(&mut dead, e));
        DEAD.with(|d| d.borrow_mut().append(&mut dead));
    });
}

fn collect() {
    // Dropped outside the borrow, in case a closure's drop registers again.
    let dead = DEAD.with(|d| core::mem::take(&mut *d.borrow_mut()));
    drop(dead);
}

fn retire() {
    let live = LIVE.with(|l| core::mem::take(&mut *l.borrow_mut()));
    RETIRED.with(|r| r.borrow_mut().extend(live.into_iter().map(|e| e.value)));
}

fn free_retired() {
    DEPTH.with(|d| d.set(0));
    collect();
    let retired = RETIRED.with(|r| core::mem::take(&mut *r.borrow_mut()));
    drop(retired);
}

/// The value a mod's load function returns: `()`, or a Result whose error
/// is logged before the mod is left unloaded.
pub trait LoadResult {
    fn into_result(self) -> Result<(), String>;
}

impl LoadResult for () {
    fn into_result(self) -> Result<(), String> {
        Ok(())
    }
}

impl<E: core::fmt::Display> LoadResult for Result<(), E> {
    fn into_result(self) -> Result<(), String> {
        self.map_err(|e| e.to_string())
    }
}

fn tome(raw: *mut sys::tome_mod) -> Option<Tome> {
    Some(Tome::new(api()?, NonNull::new(raw)?))
}

// The lifecycle functions export_mod! generates call these.

#[doc(hidden)]
pub unsafe fn load<R: LoadResult>(api: *const sys::tome_api, raw: *mut sys::tome_mod, f: fn(Tome) -> R) -> i32 {
    let Some(table) = (unsafe { api.as_ref() }) else {
        return 1;
    };
    if table.log.is_none() || raw.is_null() {
        return 1;
    }
    API.store(api as *mut _, Ordering::Release);
    MOD.store(raw, Ordering::Release);
    install_panic_hook();
    let Some(tome) = tome(raw) else { return 1 };
    match guard(|| f(tome).into_result()) {
        Ok(()) => 0,
        Err(e) => {
            crate::log(Level::Error, &format!("failed to load: {e}"));
            1
        }
    }
}

#[doc(hidden)]
pub unsafe fn enable(raw: *mut sys::tome_mod, f: Option<fn(Tome)>) {
    free_retired();
    if let (Some(f), Some(tome)) = (f, tome(raw)) {
        guard(|| f(tome));
    }
}

#[doc(hidden)]
pub unsafe fn disable(raw: *mut sys::tome_mod, f: Option<fn(Tome)>) {
    // Retired first: if `f` panics, Tome still revokes everything.
    retire();
    if let (Some(f), Some(tome)) = (f, tome(raw)) {
        guard(|| f(tome));
    }
}

#[doc(hidden)]
pub unsafe fn unload(raw: *mut sys::tome_mod, f: Option<fn(Tome)>) {
    if let (Some(f), Some(tome)) = (f, tome(raw)) {
        guard(|| f(tome));
    }
    retire();
    free_retired();
}

/// Exports the lifecycle functions Tome looks for. `load` is required and
/// returns `()` or a `Result`; `enable`, `disable` and `unload` are
/// optional, in this order, and take a [`Tome`]:
///
/// ```ignore
/// tome::export_mod! { load: load, enable: enable }
/// ```
///
/// Register callbacks in `enable`: Tome revokes them after `disable`.
#[macro_export]
macro_rules! export_mod {
    (load: $load:expr $(, enable: $enable:expr)? $(, disable: $disable:expr)? $(, unload: $unload:expr)? $(,)?) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn tome_mod_load(
            api: *const $crate::sys::tome_api,
            raw: *mut $crate::sys::tome_mod,
        ) -> i32 {
            unsafe { $crate::__rt::load(api, raw, $load) }
        }

        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn tome_mod_enable(raw: *mut $crate::sys::tome_mod) {
            #[allow(unused_mut, unused_assignments)]
            let mut f: Option<fn($crate::Tome)> = None;
            $(f = Some($enable);)?
            unsafe { $crate::__rt::enable(raw, f) }
        }

        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn tome_mod_disable(raw: *mut $crate::sys::tome_mod) {
            #[allow(unused_mut, unused_assignments)]
            let mut f: Option<fn($crate::Tome)> = None;
            $(f = Some($disable);)?
            unsafe { $crate::__rt::disable(raw, f) }
        }

        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn tome_mod_unload(raw: *mut $crate::sys::tome_mod) {
            #[allow(unused_mut, unused_assignments)]
            let mut f: Option<fn($crate::Tome)> = None;
            $(f = Some($unload);)?
            unsafe { $crate::__rt::unload(raw, f) }
        }

        // Keeps MinGW's CRT from switching the game's new threads to 64-bit
        // x87 precision (sdk/support/fpreset.c has the details).
        #[unsafe(no_mangle)]
        pub extern "C" fn _fpreset() {}
    };
}
