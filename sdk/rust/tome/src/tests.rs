// SPDX-License-Identifier: MIT-0
//! The safe layer against a mock API table (cargo test, under Wine).

use core::ffi::{CStr, c_char, c_void};
use core::mem::{offset_of, size_of, zeroed};
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::sync::Mutex;

use crate::*;

// The runtime's globals are per process: one test at a time.
static SERIAL: Mutex<()> = Mutex::new(());

thread_local! {
    static LOG: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    static EVENT_HANDLER: Cell<Option<(sys::tome_event_fn, *mut c_void)>> = const { Cell::new(None) };
}

unsafe extern "C" fn mock_log(_: *mut sys::tome_mod, _: sys::tome_log_level, message: *const c_char) {
    let line = unsafe { CStr::from_ptr(message) }.to_string_lossy().into_owned();
    LOG.with(|l| l.borrow_mut().push(line));
}

unsafe extern "C" fn mock_on_event(
    _: *mut sys::tome_mod,
    _: i32,
    f: sys::tome_event_fn,
    user: *mut c_void,
) -> sys::tome_result {
    EVENT_HANDLER.with(|h| h.set(Some((f, user))));
    sys::tome_result::TOME_OK
}

unsafe extern "C" fn mock_setting_string(
    _: *mut sys::tome_mod,
    key: *const c_char,
    buffer: *mut c_char,
    size: usize,
) -> usize {
    if unsafe { CStr::from_ptr(key) } != c"greeting" {
        return 0;
    }
    let value = "Γειά σου";
    if size > 0 {
        let n = value.len().min(size - 1);
        unsafe {
            core::ptr::copy_nonoverlapping(value.as_ptr(), buffer as *mut u8, n);
            *buffer.add(n) = 0;
        }
    }
    value.len()
}

unsafe extern "C" fn mock_string_create(text: *const c_char) -> u32 {
    unsafe { CStr::from_ptr(text) }.to_bytes().len() as u32 + 1000
}

thread_local! {
    static CLICK: Cell<Option<(sys::tome_click_fn, *mut c_void)>> = const { Cell::new(None) };
    static POPUP: Cell<Option<(sys::tome_popup_fn, *mut c_void)>> = const { Cell::new(None) };
}

unsafe extern "C" fn mock_on_click(
    _: *mut sys::tome_mod,
    _: *mut sys::tome_frame,
    f: sys::tome_click_fn,
    user: *mut c_void,
) -> sys::tome_result {
    CLICK.with(|c| c.set(Some((f, user))));
    sys::tome_result::TOME_OK
}

unsafe extern "C" fn mock_on_keybind(
    _: *mut sys::tome_mod,
    _: *const c_char,
    _: sys::tome_keybind_fn,
    _: *mut c_void,
) -> sys::tome_result {
    sys::tome_result::TOME_OK
}

unsafe extern "C" fn mock_register_command(
    _: *mut sys::tome_mod,
    name: *const c_char,
    _: sys::tome_command_fn,
    _: *mut c_void,
) -> sys::tome_result {
    if unsafe { CStr::from_ptr(name) } == c"taken" {
        sys::tome_result::TOME_ERR_NOT_OWNER
    } else {
        sys::tome_result::TOME_OK
    }
}

unsafe extern "C" fn mock_popup(
    _: *mut sys::tome_mod,
    _: *const sys::tome_popup,
    f: sys::tome_popup_fn,
    user: *mut c_void,
    _: *mut u32,
) -> sys::tome_result {
    POPUP.with(|p| p.set(Some((f, user))));
    sys::tome_result::TOME_OK
}

static mut MOD: u8 = 0;

/// A table of `version` with every function this test needs; the rest NULL.
fn api(version: u32, size: usize) -> &'static sys::tome_api {
    let mut api: sys::tome_api = unsafe { zeroed() };
    api.size = size as u32;
    api.api_version = version;
    api.tome_version = c"0.1.0".as_ptr();
    api.log = Some(mock_log);
    api.on_event = Some(mock_on_event);
    api.setting_string = Some(mock_setting_string);
    api.jass_string_create = Some(mock_string_create);
    api.frame_on_click = Some(mock_on_click);
    api.on_keybind = Some(mock_on_keybind);
    api.register_command = Some(mock_register_command);
    api.popup = Some(mock_popup);
    Box::leak(Box::new(api))
}

fn loaded(version: u32, size: usize) -> Tome {
    LOG.with(|l| l.borrow_mut().clear());
    let raw = &raw mut MOD as *mut sys::tome_mod;
    assert_eq!(unsafe { rt::load(api(version, size), raw, |_| ()) }, 0);
    rt_tome().unwrap()
}

fn log_lines() -> Vec<String> {
    LOG.with(|l| l.borrow().clone())
}

#[test]
fn versions_need_the_number_and_the_table() {
    let _serial = SERIAL.lock().unwrap();
    let full = size_of::<sys::tome_api>();
    assert!(loaded(13, full).has(13));
    let v7 = offset_of!(sys::tome_api, keybind_name) + 4;
    let tome = loaded(7, v7);
    assert!(tome.has(7) && tome.has(2) && !tome.has(8));
    // A version number without the table to go with it isn't enough.
    assert!(!loaded(13, v7).has(8));
    assert_eq!(loaded(4, v7).display_text("x", 1.0), Err(Error::UNAVAILABLE));
}

#[test]
fn load_reports_errors() {
    let _serial = SERIAL.lock().unwrap();
    let raw = &raw mut MOD as *mut sys::tome_mod;
    LOG.with(|l| l.borrow_mut().clear());
    let failed = unsafe { rt::load(api(13, size_of::<sys::tome_api>()), raw, |_| Err::<(), _>("no game")) };
    assert_eq!(failed, 1);
    assert_eq!(log_lines(), ["failed to load: no game"]);
    crate::info!("{} and {}", 1, "two\0three");
    assert_eq!(log_lines()[1], "1 and two\\0three");
}

fn event_info(size: usize) -> sys::tome_event_info {
    let mut info: sys::tome_event_info = unsafe { zeroed() };
    info.size = size as u32;
    info.event = Event::UNIT_DAMAGED.0;
    info.map_name = c"Dustwallow Keys".as_ptr();
    info.amount = 12.5;
    info.level = 3;
    info
}

fn fire(info: &sys::tome_event_info) {
    let (f, user) = EVENT_HANDLER.with(|h| h.get()).unwrap();
    unsafe { f.unwrap()(rt::mod_ptr(), info, user) };
}

#[test]
fn events_read_only_what_tome_filled() {
    let _serial = SERIAL.lock().unwrap();
    let tome = loaded(13, size_of::<sys::tome_api>());
    let seen = Rc::new(RefCell::new(Vec::new()));
    let s = seen.clone();
    tome.on_event(Event::UNIT_DAMAGED, move |info| {
        s.borrow_mut()
            .push((info.event(), info.map_name().into_owned(), info.amount(), info.level()))
    })
    .unwrap();
    fire(&event_info(size_of::<sys::tome_event_info>()));
    // An older Tome's info stops before `amount` (API version 12).
    fire(&event_info(offset_of!(sys::tome_event_info, amount)));
    let seen = seen.borrow();
    assert_eq!(seen[0], (Event::UNIT_DAMAGED, "Dustwallow Keys".into(), 12.5, 3));
    assert_eq!(seen[1], (Event::UNIT_DAMAGED, "Dustwallow Keys".into(), 0.0, 3));
}

#[test]
fn selection_steps_by_tomes_unit_size() {
    let _serial = SERIAL.lock().unwrap();
    // Units as a newer Tome might lay them out: bigger than tome_unit.
    #[repr(C)]
    #[derive(Clone, Copy)]
    struct Bigger {
        unit: sys::tome_unit,
        extra: u32,
    }
    let mut units = [Bigger {
        unit: unsafe { zeroed() },
        extra: 0,
    }; 2];
    for (i, u) in units.iter_mut().enumerate() {
        u.unit.size = size_of::<Bigger>() as u32;
        u.unit.type_id = u32::from_be_bytes(*b"hfoo") + i as u32;
        u.unit.name = c"Footman".as_ptr();
        u.unit.flags = sys::tome_unit_flag::TOME_UNIT_HERO.0;
    }
    let mut raw = event_info(size_of::<sys::tome_event_info>());
    raw.units = &units[0].unit;
    raw.unit_count = 2;
    raw.leader = 1;
    let info = unsafe { EventInfo::from_raw(&raw) };
    let codes: Vec<String> = info.units().map(|u| type_code(u.type_id())).collect();
    assert_eq!(codes, ["hfoo", "hfop"]);
    assert_eq!(info.leader(), Some(1));
    assert_eq!(info.units().next().unwrap().to_string(), "Footman (hfoo) of player 1");
    assert!(info.units().all(|u| u.is_hero() && !u.is_dead()));
}

struct Counted(Rc<Cell<u32>>);
impl Drop for Counted {
    fn drop(&mut self) {
        self.0.set(self.0.get() + 1);
    }
}

#[test]
fn callbacks_live_until_the_next_enable() {
    let _serial = SERIAL.lock().unwrap();
    let tome = loaded(13, size_of::<sys::tome_api>());
    let raw = rt::mod_ptr();
    unsafe { rt::enable(raw, None) };
    let drops = Rc::new(Cell::new(0));
    let counted = Counted(drops.clone());
    tome.on_event(Event::TICK, move |_| {
        let _ = &counted;
    })
    .unwrap();
    // Tome revokes it after disable, but it may still be on the stack.
    unsafe { rt::disable(raw, None) };
    assert_eq!(drops.get(), 0);
    unsafe { rt::enable(raw, None) };
    assert_eq!(drops.get(), 1);
}

#[test]
fn settings_copy_whole_strings() {
    let _serial = SERIAL.lock().unwrap();
    let tome = loaded(13, size_of::<sys::tome_api>());
    assert_eq!(tome.setting_string("greeting").as_deref(), Some("Γειά σου"));
    assert_eq!(tome.setting_string("missing"), None);
    // No setting_bool in the mock table: the fallback.
    assert!(tome.setting_bool("shout", true));
}

crate::native! {
    fn double(value: Real) -> RealRet { (value.get() * 2.0).into() }
    fn nothing(value: Integer) { let _ = value; }
    fn greet(flag: Boolean) -> StringRet { StringRet::new(if flag.get() { "yes" } else { "no\0pe" }) }
}

#[test]
fn natives_use_the_engine_abi() {
    let _serial = SERIAL.lock().unwrap();
    loaded(13, size_of::<sys::tome_api>());
    let x = 1.25f32;
    let real: Real = unsafe { core::mem::transmute(&x as *const f32) };
    assert_eq!(f32::from_bits(double(real).0), 2.5);
    assert_eq!(nothing(7), 0);
    // The mock's id is 1000 + the length: a NUL cuts the text short.
    assert_eq!(greet(true.into()), StringRet(1003));
    assert_eq!(greet(false.into()), StringRet(1002));
    assert_eq!(jass::NativeFn::as_ptr(double), double as *mut c_void);
}

#[test]
fn errors_read_well() {
    assert_eq!(Error::NOT_DECLARED.to_string(), "not declared in tome.mod.json");
    assert_eq!(Error(42).to_string(), "error 42");
    assert_eq!(type_code(u32::from_be_bytes(*b"Hamg")), "Hamg");
    assert_eq!(format!("{:?} {:?}", Event::MAP_LOAD, Event(999)), "MAP_LOAD Event(999)");
    assert_eq!(Event::NIGHT.name(), Some("NIGHT"));
}

#[test]
fn dead_callbacks_are_freed() {
    let _serial = SERIAL.lock().unwrap();
    let tome = loaded(13, size_of::<sys::tome_api>());
    let raw = rt::mod_ptr();
    unsafe { rt::enable(raw, None) };
    let drops = Rc::new(Cell::new(0));
    let counted = || Counted(drops.clone());
    // Refused: never registered, freed at once.
    let c = counted();
    assert_eq!(
        tome.register_command("taken", move |_| {
            let _ = &c;
        }),
        Err(Error::NOT_OWNER)
    );
    assert_eq!(drops.get(), 1);
    // The same button or key again replaces the callback; another one doesn't.
    let button = Frame::from_raw(0x10 as *mut sys::tome_frame).unwrap();
    let other = Frame::from_raw(0x20 as *mut sys::tome_frame).unwrap();
    for frame in [button, other, button] {
        let c = counted();
        tome.on_click(frame, move |_| {
            let _ = &c;
        })
        .unwrap();
    }
    assert_eq!(drops.get(), 2);
    for _ in 0..2 {
        let c = counted();
        tome.on_keybind("greet", move || {
            let _ = &c;
        })
        .unwrap();
    }
    assert_eq!(drops.get(), 3);
    // A popup's callback runs once.
    let chosen = Rc::new(Cell::new(None));
    let (c, seen) = (counted(), chosen.clone());
    tome.popup(&Popup::new("t", "x", &["OK"]), move |choice| {
        let _ = &c;
        seen.set(choice);
    })
    .unwrap();
    let (f, user) = POPUP.with(|p| p.get()).unwrap();
    unsafe { f.unwrap()(raw, 1, 0, user) };
    assert_eq!(chosen.get(), Some(0));
    assert_eq!(drops.get(), 4);
    // A callback that replaces itself stays alive until it returns.
    let (c, inside, seen) = (counted(), Rc::new(Cell::new(0)), drops.clone());
    let inside_seen = inside.clone();
    tome.on_click(button, move |b| {
        let _ = &c;
        tome.on_click(b, |_| {}).unwrap();
        inside_seen.set(seen.get());
    })
    .unwrap();
    assert_eq!(drops.get(), 5); // the one before it
    let (f, user) = CLICK.with(|c| c.get()).unwrap();
    unsafe { f.unwrap()(raw, button.as_raw(), user) };
    assert_eq!(inside.get(), 5);
    assert_eq!(drops.get(), 6);
    unsafe { rt::disable(raw, None) };
    unsafe { rt::unload(raw, None) };
    assert_eq!(drops.get(), 8); // the other button's and the key's
}

// Tome runs a mod's first load and enable on its bootstrap thread, which
// ends once the game is running; the callbacks registered there must outlive
// it (thread-local storage is dropped when its thread ends).
#[test]
fn callbacks_outlive_the_thread_that_registered_them() {
    use std::sync::Arc;
    use std::sync::atomic::{AtomicUsize, Ordering::SeqCst};
    struct Dropped(Arc<AtomicUsize>);
    impl Drop for Dropped {
        fn drop(&mut self) {
            self.0.fetch_add(1, SeqCst);
        }
    }
    struct Sendable(Tome, *mut sys::tome_mod);
    unsafe impl Send for Sendable {}

    let _serial = SERIAL.lock().unwrap();
    let tome = loaded(13, size_of::<sys::tome_api>());
    let drops = Arc::new(AtomicUsize::new(0));
    let on_thread = Sendable(tome, rt::mod_ptr());
    let counted = drops.clone();
    std::thread::spawn(move || {
        let on_thread = on_thread;
        unsafe { rt::enable(on_thread.1, None) };
        let dropped = Dropped(counted);
        on_thread.0.register_command("hello", move |_| {
            let _ = &dropped;
        })
        .unwrap();
    })
    .join()
    .unwrap();
    assert_eq!(drops.load(SeqCst), 0, "the bootstrap thread's end freed a callback Tome still holds");
    // Freed at the next enable, as callbacks registered on the main thread are.
    unsafe { rt::disable(rt::mod_ptr(), None) };
    unsafe { rt::enable(rt::mod_ptr(), None) };
    assert_eq!(drops.load(SeqCst), 1);
}
