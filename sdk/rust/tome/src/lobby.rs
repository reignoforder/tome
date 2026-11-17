// SPDX-License-Identifier: MIT-0
//! The lobby (API version 21): a required mod's messages to its copies on
//! other players' computers before a game starts, who's in the lobby, and
//! a say in when the game starts. See "Lobby messages" in tome.h.

use core::ffi::{c_char, c_void};

use crate::{Error, Result, Tome, check, guard, rt, sys};

/// Most bytes a lobby message holds.
pub const MAX_MESSAGE: usize = sys::TOME_LOBBY_MAX_MESSAGE as usize;
/// [`Tome::lobby_send`]'s `to`: every player who runs Tome (host only).
pub const EVERYONE: i32 = sys::TOME_LOBBY_EVERYONE;

/// One person in the lobby (`tome_lobby_player`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LobbyPlayer {
    /// The lobby's id for them: [`Tome::lobby_send`]'s `to`.
    pub id: i32,
    pub name: String,
    pub is_host: bool,
    pub is_local: bool,
    /// The handshake heard from their Tome (always for the local player).
    pub runs_tome: bool,
}

/// A lobby message as it arrives.
#[derive(Clone, Copy, Debug)]
pub struct LobbyMessage<'a> {
    /// The sender's lobby id.
    pub player: i32,
    /// The sender is the game's host.
    pub from_host: bool,
    pub data: &'a [u8],
}

/// What [`Tome::on_lobby_start`]'s callback says.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum StartVerdict {
    Allow,
    /// Keep the game from starting; the reason (may be empty) goes in the
    /// lobby's chat.
    Refuse(String),
}

impl Tome {
    /// Sends `data` to this mod on lobby player `to`'s computer (or, from
    /// the host, [`EVERYONE`] who runs Tome). Main thread.
    pub fn lobby_send(self, to: i32, data: &[u8]) -> Result<()> {
        self.require(21)?;
        let f = self.raw().0.lobby_send.ok_or(Error::UNAVAILABLE)?;
        let size = u32::try_from(data.len()).map_err(|_| Error::INVALID_ARGUMENT)?;
        check(unsafe { f(self.ptr(), to, data.as_ptr().cast(), size) })
    }

    /// Calls `f` for each lobby message this mod's copies send it.
    pub fn on_lobby_message<F: Fn(LobbyMessage<'_>) + 'static>(self, f: F) -> Result<()> {
        unsafe extern "C" fn call<F: Fn(LobbyMessage<'_>)>(
            _: *mut sys::tome_mod,
            m: *const sys::tome_lobby_message,
            user: *mut c_void,
        ) {
            guard(|| {
                let Some(m) = (unsafe { m.as_ref() }) else { return };
                let data = if m.data.is_null() || m.data_size == 0 {
                    &[][..]
                } else {
                    unsafe { core::slice::from_raw_parts(m.data.cast::<u8>(), m.data_size as usize) }
                };
                unsafe {
                    (*(user as *const F))(LobbyMessage {
                        player: m.player,
                        from_host: m.from_host != 0,
                        data,
                    })
                }
            })
        }
        self.require(21)?;
        let f_on = self.raw().0.on_lobby_message.ok_or(Error::UNAVAILABLE)?;
        rt::register(f, None, |user| unsafe { f_on(self.ptr(), Some(call::<F>), user) })
    }

    /// The people in the lobby, this computer included; `None` outside a
    /// game's lobby. Main thread.
    pub fn lobby_players(self) -> Option<Vec<LobbyPlayer>> {
        self.require(21).ok()?;
        let f = self.raw().0.lobby_players?;
        let mut out = vec![unsafe { core::mem::zeroed::<sys::tome_lobby_player>() }; 16];
        let mut n = unsafe { f(out.as_mut_ptr(), out.len() as i32) };
        if n < 0 {
            return None;
        }
        if n as usize > out.len() {
            out.resize(n as usize, unsafe { core::mem::zeroed() });
            n = unsafe { f(out.as_mut_ptr(), out.len() as i32) };
            if n < 0 {
                return None;
            }
        }
        Some(
            out[..(n as usize).min(out.len())]
                .iter()
                .map(|p| {
                    let bytes: Vec<u8> = p.name.iter().take_while(|&&c| c != 0).map(|&c| c as u8).collect();
                    LobbyPlayer {
                        id: p.id,
                        name: String::from_utf8_lossy(&bytes).into_owned(),
                        is_host: p.is_host != 0,
                        is_local: p.is_local != 0,
                        runs_tome: p.runs_tome != 0,
                    }
                })
                .collect(),
        )
    }

    /// Calls `f` when the host presses Start, once Tome's checks pass.
    pub fn on_lobby_start<F: Fn() -> StartVerdict + 'static>(self, f: F) -> Result<()> {
        unsafe extern "C" fn call<F: Fn() -> StartVerdict>(
            _: *mut sys::tome_mod,
            reason: *mut c_char,
            reason_size: u32,
            user: *mut c_void,
        ) -> i32 {
            // A panic refuses (as a crash does in Tome).
            let mut verdict = sys::TOME_START_REFUSE as i32;
            guard(|| {
                let answer = unsafe { (*(user as *const F))() };
                if answer == StartVerdict::Allow {
                    verdict = sys::TOME_START_ALLOW as i32;
                }
                if let StartVerdict::Refuse(text) = answer {
                    if !reason.is_null() && reason_size > 0 {
                        let mut n = text.len().min(reason_size as usize - 1);
                        while !text.is_char_boundary(n) {
                            n -= 1;
                        }
                        unsafe {
                            core::ptr::copy_nonoverlapping(text.as_ptr(), reason.cast::<u8>(), n);
                            *reason.add(n) = 0;
                        }
                    }
                }
            });
            verdict
        }
        self.require(21)?;
        let f_on = self.raw().0.on_lobby_start.ok_or(Error::UNAVAILABLE)?;
        rt::register(f, None, |user| unsafe { f_on(self.ptr(), Some(call::<F>), user) })
    }

    /// Shows `text` in this computer's lobby chat (this player only).
    pub fn lobby_show(self, text: &str) -> Result<()> {
        self.require(21)?;
        let f = self.raw().0.lobby_show.ok_or(Error::UNAVAILABLE)?;
        let text = crate::util::cstr(text)?;
        check(unsafe { f(self.ptr(), text.as_ptr()) })
    }

    /// Presses the host's Start for this mod: the checks run again
    /// (on_lobby_start too, which should now allow). Main thread.
    pub fn lobby_start(self) -> Result<()> {
        self.require(21)?;
        let f = self.raw().0.lobby_start.ok_or(Error::UNAVAILABLE)?;
        check(unsafe { f(self.ptr()) })
    }
}
