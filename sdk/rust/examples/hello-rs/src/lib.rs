// SPDX-License-Identifier: MIT
//! The hello example in Rust: logs each lifecycle step, looks up a mapped
//! game function, provides JASS natives (numbers, strings and a callback),
//! adds a "Hello (Rust)" button to the main menu (from its own
//! ui/hello_rs.fdf), logs game events, hooks the game's CTextFrame::SetText,
//! adds a /hellors chat command, and has settings (config.schema.json), a
//! keybind (Greet, Ctrl+Shift+H) and a popup.
//!
//! `/hellors panic` panics on purpose: Tome logs where and disables the mod,
//! as for a crash in a C mod.

use std::cell::Cell;
use std::rc::Rc;
use std::sync::atomic::{AtomicU32, Ordering};

use tome::{Code, Event, FramePoint, Integer, JString, Phase, Popup, Real, RealRet, StringRet, Tome, UiEvent};

tome::native! {
    /// native HelloRsLog takes integer value returns nothing
    fn hello_log(value: Integer) {
        tome::info!("HelloRsLog({value}) called from JASS");
    }

    /// native HelloRsDouble takes real value returns real
    fn hello_double(value: Real) -> RealRet {
        (value.get() * 2.0).into()
    }

    /// native HelloRsGreet takes string name returns string
    fn hello_greet(name: JString) -> StringRet {
        let name = name.get().filter(|n| !n.is_empty()).unwrap_or("world".into());
        // Letters, not bytes: JASS's StringLength would count "Ζεύς" as 8.
        StringRet::new(&format!("Hello from Rust, {name}! ({} letters)", name.chars().count()))
    }

    /// native HelloRsCall takes code callback returns boolean
    fn hello_call(callback: Code) -> tome::Boolean {
        let result = callback.call();
        tome::info!("HelloRsCall: callback returned {result:?}");
        result.unwrap_or(false).into()
    }
}

// Hooks run on whichever thread calls the game's function: plain functions
// with atomics for state.
static SET_TEXT_CALLS: AtomicU32 = AtomicU32::new(0);

fn after_set_text(call: &mut tome::HookCall) {
    if SET_TEXT_CALLS.fetch_add(1, Ordering::Relaxed) == 0 {
        // const char* text, after `this`.
        let text = unsafe { call.arg_str(1) }.unwrap_or_default().into_owned();
        tome::info!("first CTextFrame::SetText after enable: {text:?}");
    }
}

fn greet(tome: Tome, who: &str, source: &str) {
    let greeting = tome
        .setting_string("greeting")
        .unwrap_or_else(|| "Hello from Rust".into());
    let seconds = tome.setting_number("seconds", 5.0);
    let text = format!("{greeting}, {}.", if who.is_empty() { "world" } else { who });
    tome::info!("{source}: {text} ({seconds:.1} s)");
    let _ = tome.display_text(&text, seconds as f32);
}

fn add_button(tome: Tome, clicks: Rc<Cell<u32>>) {
    let (Some(parent), Some(anchor)) = (tome.frame("ControlLayer", 0), tome.frame("MainMenuFrame", 0)) else {
        return;
    };
    if tome.load_fdf("ui/hello_rs.fdf").is_err() {
        return;
    }
    // After a disable and enable on the same screen the hidden button is
    // still there: a name and context can only be created once.
    let button = match tome.frame("HelloRsButton", 0) {
        Some(button) => {
            let _ = button.set_visible(true);
            button
        }
        None => match tome.create_frame("HelloRsButton", parent, 0, 0) {
            Some(button) => button,
            None => return,
        },
    };
    // Top left, out of the way of the menu's own buttons.
    let _ = button.set_point(FramePoint::TOPLEFT, anchor, FramePoint::TOPLEFT, 0.02, -0.02);
    let clicked = tome.on_click(button, move |_| {
        clicks.set(clicks.get() + 1);
        tome::info!("Hello (Rust) button clicked ({})", clicks.get());
        if let Some(text) = tome.frame("HelloRsButtonText", 0) {
            let _ = text.set_text(&format!("Clicked {}", clicks.get()));
        }
    });
    if clicked.is_ok() {
        tome::info!("added a Hello (Rust) button to the main menu");
    }
}

fn load(tome: Tome) -> Result<(), String> {
    let game = tome.game_info();
    tome::info!(
        "loaded (Tome {}, Warcraft III {}, game.dll at {:#010x})",
        tome.version(),
        game.build_name,
        game.game_base
    );
    // Listed in "requires", so Tome only loads this mod where it's mapped.
    let set_text = tome
        .resolve("CTextFrame::SetText")
        .ok_or("CTextFrame::SetText isn't mapped")?;
    tome::info!(
        "{} is a {} function at {:#010x} ({})",
        set_text.name,
        set_text.convention,
        set_text.value,
        set_text.signature
    );
    Ok(())
}

fn enable(tome: Tome) {
    // Errors only on an older Tome, which lacks the function.
    let _ = tome.register_native("HelloRsLog", hello_log);
    let _ = tome.register_native("HelloRsDouble", hello_double);
    let _ = tome.register_native("HelloRsGreet", hello_greet);
    let _ = tome.register_native("HelloRsCall", hello_call);

    let clicks = Rc::new(Cell::new(0));
    let _ = tome.on_ui(UiEvent::MAIN_MENU, move |_| add_button(tome, clicks.clone()));

    for event in [Event::MAP_LOAD, Event::GAME_START, Event::GAME_END] {
        let _ = tome.on_event(event, |info| {
            tome::info!("{:?}: {} ({})", info.event(), info.map_name(), info.map_path());
        });
    }
    let ticks = Cell::new(0u32);
    let _ = tome.on_event(Event::TICK, move |info| {
        ticks.set(ticks.get() + 1);
        if ticks.get() == 100 {
            tome::info!("100 ticks (frame {})", info.frame());
        }
    });
    let _ = tome.on_event(Event::UNIT_DEATH, |info| {
        if let Some(unit) = info.unit() {
            let killer = info.other().map(|k| k.to_string()).unwrap_or_else(|| "nobody".into());
            tome::info!("{unit} died, killed by {killer}");
        }
    });

    SET_TEXT_CALLS.store(0, Ordering::Relaxed);
    if let Err(e) = tome.hook("CTextFrame::SetText", Phase::AFTER, 0, after_set_text) {
        tome::warn!("couldn't hook CTextFrame::SetText: {e}");
    }

    let _ = tome.register_command("hellors", move |args| {
        if args == "panic" {
            panic!("/hellors panic");
        }
        greet(tome, args, "/hellors command");
    });
    let _ = tome.on_setting_changed(|key| tome::info!("setting changed: {key}"));
    let _ = tome.on_keybind("greet", move || {
        let popup = Popup::new(
            "Hello from Rust",
            "A popup from the |cffffcc00hello-rs|r example.",
            &["Greet", "Close"],
        )
        .default_option(0)
        .cancel_option(1);
        let shown = tome.popup(&popup, move |choice| {
            if choice == Some(0) {
                greet(tome, "", "popup");
            }
        });
        if shown.is_err() {
            greet(tome, "", "Greet key");
        }
    });
    tome::info!("enabled");
}

fn disable(_: Tome) {
    // Tome revokes the natives, callbacks, hook and command itself and hides
    // the button; the natives return 0 until enabled again.
    tome::info!("disabled");
}

fn unload(_: Tome) {
    tome::info!("unloading");
}

tome::export_mod! { load: load, enable: enable, disable: disable, unload: unload }
