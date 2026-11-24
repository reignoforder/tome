// SPDX-License-Identifier: GPL-3.0-or-later
//! A WebAssembly mod for tests/host/test_wasm.cpp: the Rust runtime's lobby
//! messages, start answers and typed hooks, on functions the test's own mapping
//! file puts in the fake game. Every line it logs is tagged [rs-probe].

use tome_wasm::{HookPhase, LobbyStart, info};

fn load() {}

fn enable() {
    tome_wasm::on_lobby_message(|m| {
        info!(
            "lobby message from player {}{}: {}",
            m.player,
            if m.from_host { " (host)" } else { "" },
            String::from_utf8_lossy(m.data)
        );
    })
    .ok();
    // The first allows, the second refuses with a reason.
    tome_wasm::on_lobby_start(|| LobbyStart::Allow).ok();
    tome_wasm::on_lobby_start(|| LobbyStart::Refuse("rs-probe is not ready".into())).ok();
    let players = tome_wasm::lobby_players().map(|p| p.len());
    let sent = tome_wasm::lobby_send(tome_wasm::consts::TOME_LOBBY_EVERYONE, b"x");
    info!("lobby outside a game: players {players:?}, send ok {}", sent.is_ok());

    tome_wasm::hook("rs-probe/Scale", HookPhase::Before, 0, |call| call.set_float(0, 10.0)).ok();
    tome_wasm::hook("rs-probe/Skip", HookPhase::Before, 0, |call| {
        call.set_result_int(99);
        call.skip();
    })
    .ok();
    tome_wasm::hook("rs-probe/Twice", HookPhase::After, 0, |call| {
        call.set_result_int(call.result_int() + 1000)
    })
    .ok();
    // An int argument a handler may only read: the change is refused.
    tome_wasm::hook("rs-probe/Plus", HookPhase::Before, 0, |call| call.set_int(0, 100)).ok();
    info!("enabled");
}

fn disable() {
    info!("disabled");
}

tome_wasm::export_mod! { load: load, enable: enable, disable: disable }
