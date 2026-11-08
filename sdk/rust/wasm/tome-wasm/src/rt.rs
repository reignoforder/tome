// SPDX-License-Identifier: MIT-0
//! The module's side of Tome's calls (`docs/wasm-mods.md`, "The module's
//! contract"): the buffers Tome copies data into (`tome_buffer`), the
//! callback table behind every registration (`tome_callback`,
//! `tome_native`) and the lifecycle. `export_mod!` exports these.
//!
//! A module runs on one thread, so the state is thread-local. Callbacks can
//! nest (a native that runs map code which calls another native, a message
//! a callback publishes): nothing here stays borrowed while a mod's closure
//! runs.

use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::CString;
use std::rc::Rc;

use crate::events::EventInfo;
use crate::natives::{Args, Ret, RetKind};
use crate::{Error, Frame, Level, Result, UiEvent};

type MessageFn = Rc<dyn Fn(&str, &[u8])>;

#[derive(Clone)]
pub(crate) enum Callback {
    Text(Rc<dyn Fn(&str)>),
    Event(Rc<dyn Fn(&EventInfo)>),
    Ui(Rc<dyn Fn(UiEvent)>),
    Click(Rc<dyn Fn(Frame)>),
    Popup(Rc<dyn Fn(u32, Option<usize>)>),
    Message(MessageFn),
    Net(Rc<dyn Fn(i32, &[u8], bool)>),
    Native(Rc<dyn Fn(&Args) -> Ret>),
    Timer(Rc<dyn Fn(u32)>),
    Checksum(Rc<dyn Fn() -> u64>),
}

// A block from tome_buffer: 8-aligned (events hold doubles), with a byte
// after `size` for a NUL.
struct Buffer {
    words: Vec<u64>,
}

impl Buffer {
    fn addr(&self) -> usize {
        self.words.as_ptr() as usize
    }
}

#[derive(Default)]
struct State {
    callbacks: HashMap<u32, Callback>,
    next_id: u32,
    // The block of the last tome_buffer, until the callback it's for takes
    // it (Tome calls back right after; a new tome_buffer replaces it).
    pending: Option<Buffer>,
    // A native's string result, until the next native returns (Tome
    // copies it as the native returns).
    result: Option<CString>,
    // Running timers' callback ids, by timer id (timer_stop forgets one).
    timers: HashMap<u32, u32>,
    // A checksum's block, with the answer in it: Tome reads it after the
    // callback returns, so it lives until the next callback.
    answer: Option<Buffer>,
    // The checksum Tome calls (one at a time: registering again replaces it).
    checksum: Option<u32>,
}

thread_local! {
    static STATE: RefCell<State> = RefCell::new(State::default());
}

// Callbacks registered and not yet revoked: more means a mod registering in
// a loop (Tome revokes them all at disable).
const MAX_CALLBACKS: usize = 65536;
// Largest block tome_buffer hands out (Tome's largest is a 64 KiB message
// or a native's strings).
const MAX_BUFFER: u32 = 16 << 20;

/// Registers `callback` and calls `register` with its id: Ok, or the
/// registration is dropped again.
pub(crate) fn register(callback: Callback, register: impl FnOnce(u32) -> i32) -> Result<u32> {
    let id = STATE.with(|s| {
        let mut s = s.borrow_mut();
        if s.callbacks.len() >= MAX_CALLBACKS {
            return None;
        }
        // Ids are never reused: a callback Tome still had can't reach
        // another one.
        s.next_id = s.next_id.checked_add(1)?;
        let id = s.next_id;
        s.callbacks.insert(id, callback);
        Some(id)
    });
    let Some(id) = id else {
        return Err(Error::INVALID_ARGUMENT);
    };
    match crate::check(register(id)) {
        Ok(()) => Ok(id),
        Err(e) => {
            forget(id);
            Err(e)
        }
    }
}

pub(crate) fn forget(id: u32) {
    STATE.with(|s| s.borrow_mut().callbacks.remove(&id));
}

/// The checksum is now callback `id` (None: removed): the one before goes.
pub(crate) fn replace_checksum(id: Option<u32>) {
    let old = STATE.with(|s| core::mem::replace(&mut s.borrow_mut().checksum, id));
    if let Some(old) = old.filter(|&old| Some(old) != id) {
        forget(old);
    }
}

/// Timer `timer` calls back through callback `id`.
pub(crate) fn note_timer(timer: u32, id: u32) {
    STATE.with(|s| s.borrow_mut().timers.insert(timer, id));
}

/// Timer `timer` won't call back any more: its closure goes.
pub(crate) fn forget_timer(timer: u32) {
    let id = STATE.with(|s| s.borrow_mut().timers.remove(&timer));
    if let Some(id) = id {
        forget(id);
    }
}

fn lookup(id: u32) -> Option<Callback> {
    STATE.with(|s| s.borrow().callbacks.get(&id).cloned())
}

// Takes the pending block if it's the one at `addr`: it lives until the
// callback using it returns.
fn take_pending(addr: usize) -> Option<Buffer> {
    STATE.with(|s| {
        let mut s = s.borrow_mut();
        if s.pending.as_ref().is_some_and(|b| b.addr() == addr) {
            s.pending.take()
        } else {
            None
        }
    })
}

/// # Safety
/// `addr`..`addr + len` is in the module's memory (Tome's own block).
unsafe fn bytes<'a>(addr: usize, len: u32) -> &'a [u8] {
    if len == 0 {
        return &[];
    }
    unsafe { core::slice::from_raw_parts(addr as *const u8, len as usize) }
}

fn word(data: &[u8], at: usize) -> Option<u32> {
    let w = data.get(at..at + 4)?;
    Some(u32::from_le_bytes([w[0], w[1], w[2], w[3]]))
}

// --- the exports (export_mod!) ---------------------------------------------
// Addresses are usize here (u32 in the module, where they're the same; the
// crate's unit tests run on 64-bit hosts).

pub fn buffer(size: u32) -> usize {
    if size > MAX_BUFFER {
        return 0;
    }
    let block = Buffer {
        words: vec![0u64; (size as usize + 1).div_ceil(8)],
    };
    let addr = block.addr();
    STATE.with(|s| s.borrow_mut().pending = Some(block));
    addr
}

pub fn callback(id: u32, addr: usize, len: u32) {
    let mut block = take_pending(addr);
    STATE.with(|s| s.borrow_mut().answer = None);
    let Some(callback) = lookup(id) else { return };
    // Tome's copy, alive until this returns (block, or Tome's own memory).
    let data = unsafe { bytes(addr, len) };
    match callback {
        Callback::Text(f) => f(&String::from_utf8_lossy(data)),
        Callback::Event(f) => f(&unsafe { EventInfo::from_block(data) }),
        Callback::Ui(f) => {
            if let Some(w) = word(data, 0) {
                f(UiEvent(w as i32))
            }
        }
        Callback::Click(f) => {
            if let Some(w) = word(data, 0) {
                f(Frame(w))
            }
        }
        Callback::Popup(f) => {
            // A popup calls back once.
            forget(id);
            if let (Some(popup), Some(choice)) = (word(data, 0), word(data, 4)) {
                f(popup, usize::try_from(choice as i32).ok())
            }
        }
        Callback::Message(f) => {
            // [topic length][topic][data]
            let Some(n) = word(data, 0) else { return };
            let n = n as usize;
            if n > data.len() - 4 {
                return;
            }
            f(&String::from_utf8_lossy(&data[4..4 + n]), &data[4 + n..])
        }
        Callback::Net(f) => {
            // [player][replay][data]
            if let (Some(player), Some(replay)) = (word(data, 0), word(data, 4)) {
                f(player as i32, &data[8..], replay != 0)
            }
        }
        Callback::Native(_) => {}
        Callback::Timer(f) => {
            // [timer][state]: 0 it fires again, 1 for the last time, 2 it
            // ended without firing (its game ended).
            let (Some(timer), Some(state)) = (word(data, 0), word(data, 4)) else { return };
            if state != 0 {
                forget_timer(timer);
            }
            if state != 2 {
                f(timer)
            }
        }
        Callback::Checksum(f) => {
            // The hash goes back in the block (Tome reads it after this returns).
            if len < 8 {
                return;
            }
            let hash = f().to_le_bytes();
            unsafe { core::ptr::copy_nonoverlapping(hash.as_ptr(), addr as *mut u8, 8) };
            let kept = block.take();
            STATE.with(|s| s.borrow_mut().answer = kept);
        }
    }
}

pub fn native(id: u32, addr: usize, count: u32) -> usize {
    let block = take_pending(addr);
    let Some(Callback::Native(f)) = lookup(id) else {
        return 0;
    };
    // One word per argument; strings point at copies after them, in the
    // block (alive until this returns). Not from tome_buffer: the words only.
    let len = match &block {
        Some(b) => b.words.len() * 8,
        None => count as usize * 4,
    };
    let Some(args) = Args::new(unsafe { bytes(addr, len as u32) }, count as usize) else {
        return 0;
    };
    let ret = f(&args);
    match ret.0 {
        RetKind::Word(w) => w as usize,
        RetKind::Text(None) => 0,
        RetKind::Text(Some(text)) => {
            // Cut at a NUL: JASS strings end there.
            let text = CString::new(text).unwrap_or_else(|e| {
                let n = e.nul_position();
                CString::new(&e.into_vec()[..n]).unwrap_or_default()
            });
            STATE.with(|s| {
                let mut s = s.borrow_mut();
                let addr = text.as_ptr() as usize;
                s.result = Some(text);
                addr
            })
        }
    }
}

// --- the lifecycle ---------------------------------------------------------

/// What `load` may return: nothing, or a `Result` whose error is logged
/// (the mod then fails to load).
pub trait LoadResult {
    fn into_code(self) -> i32;
}

impl LoadResult for () {
    fn into_code(self) -> i32 {
        0
    }
}

impl<E: core::fmt::Display> LoadResult for core::result::Result<(), E> {
    fn into_code(self) -> i32 {
        match self {
            Ok(()) => 0,
            Err(e) => {
                crate::log(Level::Error, &format!("load failed: {e}"));
                1
            }
        }
    }
}

pub fn load(f: impl FnOnce() -> i32) -> i32 {
    // A panic is a trap, which disables the mod: log what it was first.
    std::panic::set_hook(Box::new(|info| crate::log(Level::Error, &format!("panicked: {info}"))));
    f()
}

pub fn enable(f: impl FnOnce()) {
    f()
}

/// Tome revokes the mod's registrations when it's disabled: their closures
/// go too.
pub fn disable(f: impl FnOnce()) {
    f();
    STATE.with(|s| {
        let mut s = s.borrow_mut();
        s.callbacks.clear();
        s.timers.clear();
        s.checksum = None;
        s.result = None;
    });
}

pub fn unload(f: impl FnOnce()) {
    f()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;

    #[test]
    fn a_text_callback_gets_its_block() {
        let got = Rc::new(RefCell::new(String::new()));
        let g = got.clone();
        let id = register(Callback::Text(Rc::new(move |t| *g.borrow_mut() = t.to_owned())), |_| 0).unwrap();
        let addr = buffer(5);
        unsafe { core::ptr::copy_nonoverlapping(b"hello".as_ptr(), addr as *mut u8, 5) };
        callback(id, addr, 5);
        assert_eq!(*got.borrow(), "hello");
        assert!(STATE.with(|s| s.borrow().pending.is_none()));
    }

    #[test]
    fn a_failed_registration_is_dropped() {
        let before = STATE.with(|s| s.borrow().callbacks.len());
        assert_eq!(register(Callback::Ui(Rc::new(|_| {})), |_| 4), Err(Error::UNAVAILABLE));
        assert_eq!(STATE.with(|s| s.borrow().callbacks.len()), before);
    }

    #[test]
    fn a_message_splits_topic_and_data() {
        let got = Rc::new(RefCell::new((String::new(), Vec::new())));
        let g = got.clone();
        let f = move |t: &str, d: &[u8]| *g.borrow_mut() = (t.to_owned(), d.to_vec());
        let id = register(Callback::Message(Rc::new(f)), |_| 0).unwrap();
        let block = [3, 0, 0, 0, b'a', b'.', b'b', 1, 2];
        callback(id, block.as_ptr() as usize, block.len() as u32);
        assert_eq!(*got.borrow(), ("a.b".to_owned(), vec![1, 2]));
        // A topic longer than the block: nothing.
        let calls = Rc::new(Cell::new(0));
        let c = calls.clone();
        let id = register(Callback::Message(Rc::new(move |_, _| c.set(c.get() + 1))), |_| 0).unwrap();
        let bad = [9, 0, 0, 0, b'a'];
        callback(id, bad.as_ptr() as usize, bad.len() as u32);
        callback(id, bad.as_ptr() as usize, 2);
        assert_eq!(calls.get(), 0);
    }

    #[test]
    fn a_popup_calls_back_once() {
        let calls = Rc::new(Cell::new(None));
        let c = calls.clone();
        let id = register(Callback::Popup(Rc::new(move |_, choice| c.set(Some(choice)))), |_| 0).unwrap();
        let closed: [u8; 8] = [7, 0, 0, 0, 0xff, 0xff, 0xff, 0xff];
        callback(id, closed.as_ptr() as usize, 8);
        assert_eq!(calls.get(), Some(None));
        calls.set(None);
        callback(id, closed.as_ptr() as usize, 8);
        assert_eq!(calls.get(), None);
    }

    #[test]
    fn a_timer_is_forgotten_after_its_last_call() {
        let calls = Rc::new(Cell::new(0));
        let c = calls.clone();
        let id = register(Callback::Timer(Rc::new(move |_| c.set(c.get() + 1))), |_| 0).unwrap();
        note_timer(9, id);
        let again: [u8; 8] = [9, 0, 0, 0, 0, 0, 0, 0];
        let last: [u8; 8] = [9, 0, 0, 0, 1, 0, 0, 0];
        callback(id, again.as_ptr() as usize, 8);
        callback(id, last.as_ptr() as usize, 8);
        callback(id, again.as_ptr() as usize, 8);
        assert_eq!(calls.get(), 2);
        // Ended with its game: forgotten, not called.
        let c = calls.clone();
        let id = register(Callback::Timer(Rc::new(move |_| c.set(c.get() + 1))), |_| 0).unwrap();
        note_timer(10, id);
        let ended: [u8; 8] = [10, 0, 0, 0, 2, 0, 0, 0];
        callback(id, ended.as_ptr() as usize, 8);
        assert_eq!(calls.get(), 2);
        assert!(lookup(id).is_none());
    }

    #[test]
    fn a_checksum_answers_in_its_block() {
        let id = register(Callback::Checksum(Rc::new(|| 0x0102_0304_0506_0708)), |_| 0).unwrap();
        let addr = buffer(8);
        callback(id, addr, 8);
        let got = unsafe { bytes(addr, 8) };
        assert_eq!(got, &[8, 7, 6, 5, 4, 3, 2, 1]);
    }

    #[test]
    fn a_new_checksum_frees_the_one_before() {
        let first = register(Callback::Checksum(Rc::new(|| 1)), |_| 0).unwrap();
        replace_checksum(Some(first));
        let second = register(Callback::Checksum(Rc::new(|| 2)), |_| 0).unwrap();
        replace_checksum(Some(second));
        assert!(lookup(first).is_none());
        assert!(lookup(second).is_some());
        replace_checksum(None);
        assert!(lookup(second).is_none());
    }

    #[test]
    fn natives_return_words_and_strings() {
        let id = register(
            Callback::Native(Rc::new(|a: &Args| Ret::from(a.integer(0) + a.integer(1)))),
            |_| 0,
        )
        .unwrap();
        let words = [40u32, 2];
        assert_eq!(native(id, words.as_ptr() as usize, 2), 42);
        let id = register(Callback::Native(Rc::new(|_: &Args| Ret::from("a\0b"))), |_| 0).unwrap();
        let addr = native(id, words.as_ptr() as usize, 0);
        let text = unsafe { std::ffi::CStr::from_ptr(addr as *const core::ffi::c_char) };
        assert_eq!(text.to_str(), Ok("a"));
        // Not a native: 0.
        let text_id = register(Callback::Text(Rc::new(|_| {})), |_| 0).unwrap();
        assert_eq!(native(text_id, words.as_ptr() as usize, 2), 0);
    }
}
