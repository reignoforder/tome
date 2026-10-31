//! A Tome mod in Rust as WebAssembly: a JASS native, a game event and a
//! chat command. The `tome-wasm` crate's documentation and
//! sdk/rust/README.md explain the API; more of it in use:
//! sdk/rust/wasm/examples/hello-wasm-rs.

use tome_wasm::{Args, Event};

fn load() {
    tome_wasm::info!("loaded");
}

// Register everything here: Tome revokes it when the mod is disabled.
fn enable() {
    let _ = tome_wasm::register_native("MyRustWasmModAdd", |a: &Args| {
        a.integer(0).wrapping_add(a.integer(1))
    });
    let _ = tome_wasm::on_event(Event::GAME_START, |info| {
        tome_wasm::info!("game started on {}", info.map_name())
    });
    // /myrustwasmmod <text> in the in-game chat.
    let _ = tome_wasm::register_command("myrustwasmmod", |args| {
        let _ = tome_wasm::display_text(
            if args.is_empty() {
                "Hello from my-rust-wasm-mod"
            } else {
                args
            },
            5.0,
        );
    });
}

tome_wasm::export_mod! { load: load, enable: enable }
