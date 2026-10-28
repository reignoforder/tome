//! Screens and frames (API version 3; every screen since 5): the game's UI
//! objects, the ones 1.31's Blz*Frame* natives use. A [`Frame`] stays valid
//! while its screen exists: look frames up again in each [`Tome::on_ui`]
//! callback instead of keeping them. Main thread only.

use core::ffi::c_void;
use core::ptr::NonNull;

use crate::util::cstr;
use crate::{Error, Result, Tome, check, guard, rt, sys};

/// A screen (`tome_ui_event`).
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
#[repr(transparent)]
pub struct UiEvent(pub i32);

impl UiEvent {
    pub const MAIN_MENU: UiEvent = UiEvent(1);
    pub const SINGLE_PLAYER: UiEvent = UiEvent(2);
    pub const CUSTOM_GAME: UiEvent = UiEvent(3);
    pub const OPTIONS: UiEvent = UiEvent(4);
    pub const CAMPAIGN: UiEvent = UiEvent(5);
    pub const CUSTOM_CAMPAIGN: UiEvent = UiEvent(6);
    pub const LOAD_SAVED_GAME: UiEvent = UiEvent(7);
    pub const VIEW_REPLAY: UiEvent = UiEvent(8);
    pub const LAN: UiEvent = UiEvent(9);
    pub const BATTLE_NET: UiEvent = UiEvent(10);
    pub const LOADING: UiEvent = UiEvent(11);
    pub const SCORE_SCREEN: UiEvent = UiEvent(12);
    /// The in-game UI, once loading has finished.
    pub const GAME: UiEvent = UiEvent(13);
}

/// An anchor point, numbered like 1.31's FRAMEPOINT_*.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
#[repr(transparent)]
pub struct FramePoint(pub i32);

impl FramePoint {
    pub const TOPLEFT: FramePoint = FramePoint(0);
    pub const TOP: FramePoint = FramePoint(1);
    pub const TOPRIGHT: FramePoint = FramePoint(2);
    pub const LEFT: FramePoint = FramePoint(3);
    pub const CENTER: FramePoint = FramePoint(4);
    pub const RIGHT: FramePoint = FramePoint(5);
    pub const BOTTOMLEFT: FramePoint = FramePoint(6);
    pub const BOTTOM: FramePoint = FramePoint(7);
    pub const BOTTOMRIGHT: FramePoint = FramePoint(8);
}

/// A frame (the game's CFrame*).
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct Frame(NonNull<sys::tome_frame>);

fn frames() -> Result<&'static sys::tome_api> {
    let api = rt::api().ok_or(Error::UNAVAILABLE)?;
    crate::rt_tome().ok_or(Error::UNAVAILABLE)?.require(3)?;
    Ok(api)
}

impl Frame {
    pub fn from_raw(raw: *mut sys::tome_frame) -> Option<Frame> {
        NonNull::new(raw).map(Frame)
    }

    pub fn as_raw(self) -> *mut sys::tome_frame {
        self.0.as_ptr()
    }

    /// Anchors `point` to `relative_point` of `relative`, offset by x, y
    /// (0.8 x 0.6 screen units, y up).
    pub fn set_point(
        self,
        point: FramePoint,
        relative: Frame,
        relative_point: FramePoint,
        x: f32,
        y: f32,
    ) -> Result<()> {
        let f = frames()?.frame_set_point.ok_or(Error::UNAVAILABLE)?;
        check(unsafe { f(self.as_raw(), point.0, relative.as_raw(), relative_point.0, x, y) })
    }

    /// TEXT frames only; for a text button, set its text child.
    pub fn set_text(self, text: &str) -> Result<()> {
        let text = cstr(text)?;
        let f = frames()?.frame_set_text.ok_or(Error::UNAVAILABLE)?;
        check(unsafe { f(self.as_raw(), text.as_ptr()) })
    }

    /// Showing a frame also shows its children.
    pub fn set_visible(self, visible: bool) -> Result<()> {
        let f = frames()?.frame_set_visible.ok_or(Error::UNAVAILABLE)?;
        check(unsafe { f(self.as_raw(), visible as i32) })
    }

    /// Plays a sprite's sequence matching animation tags (resolve
    /// "AnimTag.<name>" for their ids).
    pub fn set_animation(self, tags: &[i32]) -> Result<()> {
        let f = frames()?.frame_set_animation.ok_or(Error::UNAVAILABLE)?;
        check(unsafe { f(self.as_raw(), tags.as_ptr(), tags.len() as i32) })
    }
}

impl Tome {
    /// Calls `f` every time `screen` is built, and now if it's up already.
    /// Create the mod's frames for that screen there.
    pub fn on_ui<F: Fn(UiEvent) + 'static>(self, screen: UiEvent, f: F) -> Result<()> {
        unsafe extern "C" fn call<F: Fn(UiEvent)>(_: *mut sys::tome_mod, event: i32, user: *mut c_void) {
            guard(|| unsafe { (*(user as *const F))(UiEvent(event)) })
        }
        self.require(if screen == UiEvent::MAIN_MENU { 3 } else { 5 })?;
        let on_ui = self.raw().0.on_ui.ok_or(Error::UNAVAILABLE)?;
        rt::register(f, None, |user| unsafe {
            on_ui(self.ptr(), screen.0, Some(call::<F>), user)
        })
    }

    /// Loads an FDF file from the mod's folder ("ui/mymod.fdf").
    pub fn load_fdf(self, path: &str) -> Result<()> {
        self.require(3)?;
        let path = cstr(path)?;
        check(unsafe { self.raw().0.load_fdf.ok_or(Error::UNAVAILABLE)?(self.ptr(), path.as_ptr()) })
    }

    /// Loads a TOC file from the game's archives.
    pub fn load_toc(self, path: &str) -> Result<()> {
        self.require(3)?;
        let path = cstr(path)?;
        check(unsafe { self.raw().0.load_toc.ok_or(Error::UNAVAILABLE)?(path.as_ptr()) })
    }

    /// The frame with this FDF name and create context.
    pub fn frame(self, name: &str, context: i32) -> Option<Frame> {
        self.require(3).ok()?;
        let name = cstr(name).ok()?;
        Frame::from_raw(unsafe { self.raw().0.frame_get?(name.as_ptr(), context) })
    }

    /// Creates a frame from an FDF definition under `parent`. None if the
    /// definition doesn't exist or that name and context exist already (get
    /// it with [`Tome::frame`] then).
    pub fn create_frame(self, name: &str, parent: Frame, priority: i32, context: i32) -> Option<Frame> {
        self.require(3).ok()?;
        let name = cstr(name).ok()?;
        let create = self.raw().0.frame_create?;
        Frame::from_raw(unsafe { create(self.ptr(), name.as_ptr(), parent.as_raw(), priority, context) })
    }

    /// The root frame of a screen while it shows.
    pub fn ui_root(self, screen: UiEvent) -> Option<Frame> {
        self.require(5).ok()?;
        Frame::from_raw(unsafe { self.raw().0.ui_root?(screen.0) })
    }

    /// Calls `f` when a button is clicked. Register again in each on_ui
    /// callback: a screen's buttons are new each time.
    pub fn on_click<F: Fn(Frame) + 'static>(self, button: Frame, f: F) -> Result<()> {
        unsafe extern "C" fn call<F: Fn(Frame)>(
            _: *mut sys::tome_mod,
            button: *mut sys::tome_frame,
            user: *mut c_void,
        ) {
            guard(|| {
                if let Some(button) = Frame::from_raw(button) {
                    unsafe { (*(user as *const F))(button) }
                }
            })
        }
        self.require(3)?;
        let on_click = self.raw().0.frame_on_click.ok_or(Error::UNAVAILABLE)?;
        // Registering the same button again replaces its callback.
        let key = format!("click:{:p}", button.as_raw());
        rt::register(f, Some(key), |user| unsafe {
            on_click(self.ptr(), button.as_raw(), Some(call::<F>), user)
        })
    }
}
