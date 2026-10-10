//! The device-facing daemon. Nothing in here imports `tauri`, so tests run headless.

pub mod actions;
pub mod mdns;
pub mod protocol;
pub mod server;
