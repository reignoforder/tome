//! The WebAssembly example in Rust (the tome-wasm crate): it runs
//! sandboxed, so it can only do what Tome's imports let it, and Tome
//! doesn't ask players to trust it with their computer. Every line it logs
//! is tagged [hello-wasm-rs] in tome/tome.log.

use std::cell::Cell;

use tome_wasm::{Args, Event, EventInfo, Frame, Handle, UiEvent, info};

thread_local! {
    static TICKS: Cell<u32> = const { Cell::new(0) };
    static SUBSCRIBED: Cell<bool> = const { Cell::new(false) };
}

fn load() {
    let game = tome_wasm::game_info();
    info!("loaded, Warcraft III {}", game.build_name);
}

/// /hellowasmrs <name>: the greeting setting, `repeat` times.
fn greet(args: &str) {
    let name = if args.is_empty() { "world" } else { args };
    let line = format!("{}, {name}.", tome_wasm::setting_string("greeting"));
    for _ in 0..tome_wasm::setting_int("repeat", 1) {
        info!("/hellowasmrs command: {line}");
    }
    tome_wasm::display_text(&line, 5.0).ok();
}

fn on_event(event: &EventInfo) {
    match event.event() {
        Event::MAP_LOAD => info!("map load: {}", event.map_path()),
        Event::GAME_START => info!("game start: {}", event.map_name()),
        Event::GAME_END => info!("game end: {}", event.map_name()),
        Event::TICK if TICKS.with(|t| t.replace(t.get() + 1)) == 99 => info!("100 ticks"),
        _ => {}
    }
}

fn enable() {
    tome_wasm::register_command("hellowasmrs", greet).ok();
    tome_wasm::on_setting_changed(|key| info!("setting changed: {key}")).ok();
    tome_wasm::on_keybind("greet", |id| info!("key pressed: {}", tome_wasm::keybind_name(id))).ok();
    for event in [Event::MAP_LOAD, Event::GAME_START, Event::GAME_END, Event::TICK] {
        tome_wasm::on_event(event, on_event).ok();
    }
    tome_wasm::on_ui(UiEvent::MAIN_MENU, |_| {
        // Frames are handles: looked up by name each time they're used.
        let found = Frame::get("SinglePlayerButton", 0).is_some();
        info!(
            "main menu: SinglePlayerButton {}",
            if found { "found" } else { "missing" }
        );
        // The C example (hello-wasm) publishes its greetings here. Here, not
        // in enable: subscribe works on the game's main thread, and a mod's
        // first enable runs before the game does.
        if !SUBSCRIBED.replace(true) {
            tome_wasm::subscribe("hello-wasm.greeted", |topic, data| {
                info!("{topic}: {}", String::from_utf8_lossy(data));
            })
            .ok();
        }
    })
    .ok();

    // Natives (tome.mod.json "natives"): the arguments as declared.
    tome_wasm::register_native("HelloWasmRsAdd", |a: &Args| a.integer(0).wrapping_add(a.integer(1))).ok();
    tome_wasm::register_native("HelloWasmRsHalf", |a: &Args| a.real(0) / 2.0).ok();
    tome_wasm::register_native("HelloWasmRsGreet", |a: &Args| match a.string(0) {
        Some(name) => format!("Hello, {name}"),
        None => "Hello, nobody".to_owned(),
    })
    .ok();
    // A handle result must be one of the call's own arguments of that type.
    tome_wasm::register_native("HelloWasmRsSame", |a: &Args| -> Handle { a.handle(0) }).ok();
    info!("enabled");
}

fn disable() {
    SUBSCRIBED.set(false);
    info!("disabled");
}

tome_wasm::export_mod! { load: load, enable: enable, disable: disable }
