// SPDX-License-Identifier: MIT-0
//! Raw bindings to Tome's mod API, generated from `sdk/include/tome/tome.h`
//! by `tools/gen-rust-bindings.sh`. The header is the documentation; the
//! `tome` crate wraps these in a safe API.
//!
//! The header's inline helpers (`tome_has_*`, `tome_resolve`, ...) aren't
//! here: check `tome_api::size` before using a field added after the API
//! version a mod needs, as they do.
#![no_std]
#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, clippy::all)]

include!("bindings.rs");
