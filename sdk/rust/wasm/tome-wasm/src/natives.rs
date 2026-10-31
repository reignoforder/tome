//! JASS natives (`tome.mod.json` "natives", [`register_native`]): Tome
//! converts the arguments before the module sees them and checks the
//! result (`docs/wasm-mods.md`, "Natives").

use std::borrow::Cow;

use crate::rt::{self, Callback};
use crate::{Result, raw, str_arg};

/// A JASS handle id (a unit, a timer...), as a native gets it. A native may
/// return only one of its own arguments of that type: Tome makes any other
/// handle null.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, Default)]
pub struct Handle(pub u32);

/// A JASS `code` value (a function of the map script), for
/// [`jass_code_call`] and [`jass_code_execute`].
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, Default)]
pub struct Code(pub u32);

impl Code {
    pub fn is_null(self) -> bool {
        self.0 == 0
    }
}

/// A native's arguments, one word each, in the order `tome.mod.json`
/// declares them. An index past the last reads as 0.
pub struct Args<'a> {
    // Tome's block: the words, then the strings' copies.
    block: &'a [u8],
    count: usize,
}

impl<'a> Args<'a> {
    /// `block` starts with `count` words; None if it's shorter.
    pub(crate) fn new(block: &'a [u8], count: usize) -> Option<Args<'a>> {
        if count.checked_mul(4)? > block.len() {
            return None;
        }
        Some(Args { block, count })
    }

    pub fn len(&self) -> usize {
        self.count
    }

    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// The raw word.
    pub fn word(&self, index: usize) -> u32 {
        if index >= self.count {
            return 0;
        }
        let w = &self.block[index * 4..index * 4 + 4];
        u32::from_le_bytes([w[0], w[1], w[2], w[3]])
    }

    pub fn integer(&self, index: usize) -> i32 {
        self.word(index) as i32
    }

    pub fn boolean(&self, index: usize) -> bool {
        self.word(index) != 0
    }

    pub fn real(&self, index: usize) -> f32 {
        f32::from_bits(self.word(index))
    }

    pub fn handle(&self, index: usize) -> Handle {
        Handle(self.word(index))
    }

    pub fn code(&self, index: usize) -> Code {
        Code(self.word(index))
    }

    /// A string argument (`None` for a null string): Tome's copy, valid
    /// during the call. Only Tome's copies are read: a word that doesn't
    /// point at one (an argument of another type) is `None` too.
    pub fn string(&self, index: usize) -> Option<Cow<'a, str>> {
        let addr = self.word(index);
        if addr == 0 {
            return None;
        }
        // As a wasm32 address (the block's own, truncated in host tests).
        let at = addr.wrapping_sub(self.block.as_ptr() as usize as u32) as usize;
        if at < self.count * 4 || at >= self.block.len() {
            return None;
        }
        let text = &self.block[at..];
        let end = text.iter().position(|&b| b == 0)?;
        Some(String::from_utf8_lossy(&text[..end]))
    }
}

pub(crate) enum RetKind {
    Word(u32),
    Text(Option<String>),
}

/// What a native returns: `()`, an integer, a `bool`, an `f32` (a real), a
/// [`Handle`], or a string (`&str`, `String`, `Option<String>` for null;
/// cut at a NUL byte).
pub struct Ret(pub(crate) RetKind);

macro_rules! words {
    ($($ty:ty => |$v:ident| $e:expr),* $(,)?) => {
        $(impl From<$ty> for Ret {
            fn from($v: $ty) -> Ret {
                Ret(RetKind::Word($e))
            }
        })*
    };
}

words! {
    () => |_v| 0,
    i32 => |v| v as u32,
    u32 => |v| v,
    bool => |v| v as u32,
    f32 => |v| v.to_bits(),
    Handle => |v| v.0,
}

impl From<&str> for Ret {
    fn from(v: &str) -> Ret {
        Ret(RetKind::Text(Some(v.to_owned())))
    }
}

impl From<String> for Ret {
    fn from(v: String) -> Ret {
        Ret(RetKind::Text(Some(v)))
    }
}

impl From<Option<String>> for Ret {
    fn from(v: Option<String>) -> Ret {
        Ret(RetKind::Text(v))
    }
}

/// Implements the native `name` (declared in `tome.mod.json` "natives";
/// natives returning `code` can't be registered). Register in `enable`.
/// The closure may run map code ([`jass_code_call`]) that calls natives
/// again, this one included.
pub fn register_native<R: Into<Ret>>(name: &str, f: impl Fn(&Args) -> R + 'static) -> Result<()> {
    let (ptr, len) = str_arg(name);
    rt::register(
        Callback::Native(std::rc::Rc::new(move |args: &Args| f(args).into())),
        |id| unsafe { raw::register_native(ptr, len, id) },
    )
    .map(drop)
}

/// Runs a `code` value (a function of the map script) and returns what it
/// returned. On the main thread, in a native or a game event.
pub fn jass_code_call(code: Code) -> Result<i32> {
    let mut result = 0;
    crate::check(unsafe { raw::jass_code_call(code.0, &mut result) })?;
    Ok(result)
}

/// Runs a `code` value in a new JASS thread (it may wait, as `ExecuteFunc`).
pub fn jass_code_execute(code: Code) -> Result<()> {
    crate::check(unsafe { raw::jass_code_execute(code.0) })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn arguments_read_as_their_types() {
        let words = [(-5i32) as u32, 1, 2.5f32.to_bits(), 0x100007];
        let block: Vec<u8> = words.iter().flat_map(|w| w.to_le_bytes()).collect();
        let args = Args::new(&block, 4).unwrap();
        assert_eq!(args.len(), 4);
        assert_eq!(args.integer(0), -5);
        assert!(args.boolean(1));
        assert_eq!(args.real(2), 2.5);
        assert_eq!(args.handle(3), Handle(0x100007));
        assert_eq!(args.word(9), 0);
        assert!(args.string(9).is_none());
        // Words that aren't Tome's string copies don't read as strings.
        assert!(args.string(0).is_none());
        assert!(args.string(3).is_none());
        assert!(Args::new(&block, 5).is_none());
    }

    #[test]
    fn strings_are_read_from_the_block_only() {
        // Three words: a string, another word pointing at the words
        // themselves, and one pointing at the end (no NUL after it).
        let mut block = vec![0u8; 12];
        block.extend_from_slice(b"hi\0");
        block.extend_from_slice(b"x");
        let base = block.as_ptr() as usize as u32;
        for (i, w) in [base.wrapping_add(12), base.wrapping_add(4), base.wrapping_add(15)].iter().enumerate() {
            block[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
        }
        // The words changed after `base` was taken: same allocation.
        let args = Args::new(&block, 3).unwrap();
        assert_eq!(args.string(0).as_deref(), Some("hi"));
        assert!(args.string(1).is_none());
        assert!(args.string(2).is_none());
    }

    #[test]
    fn results_are_words() {
        let word = |r: Ret| match r.0 {
            RetKind::Word(w) => w,
            RetKind::Text(_) => panic!("text"),
        };
        assert_eq!(word(Ret::from(-1)), u32::MAX);
        assert_eq!(word(Ret::from(true)), 1);
        assert_eq!(word(Ret::from(0.5f32)), 0.5f32.to_bits());
        assert_eq!(word(Ret::from(())), 0);
        assert!(matches!(Ret::from(None).0, RetKind::Text(None)));
    }
}
