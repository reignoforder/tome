//! A Tome mod in Rust: a JASS native, a game event and a chat command. The
//! `tome` crate's documentation and sdk/rust/README.md explain the API; more
//! of it in use: sdk/rust/examples/hello-rs.

use tome::{Event, Integer, Tome};

tome::native! {
    /// native MyRustModAdd takes integer a, integer b returns integer
    fn my_add(a: Integer, b: Integer) -> Integer {
        a.wrapping_add(b)
    }
}

fn load(tome: Tome) {
    tome::info!("loaded (Tome {})", tome.version());
}

// Register everything here: Tome revokes it when the mod is disabled.
fn enable(tome: Tome) {
    let _ = tome.register_native("MyRustModAdd", my_add);
    let _ = tome.on_event(Event::GAME_START, |info| tome::info!("game started on {}", info.map_name()));
    // /myrustmod <text> in the in-game chat.
    let _ = tome.register_command("myrustmod", move |args| {
        let _ = tome.display_text(if args.is_empty() { "Hello from my-rust-mod" } else { args }, 5.0);
    });
}

tome::export_mod! { load: load, enable: enable }
