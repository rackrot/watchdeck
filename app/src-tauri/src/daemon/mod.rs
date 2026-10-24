//! The device-facing daemon. Nothing in here imports `tauri`, so tests run headless.

pub mod actions;
pub mod alerts;
pub mod config;
pub mod feed;
mod fonts_gen;
pub mod gpu;
pub mod hoststats;
pub mod hwmon;
pub mod icons;
pub mod integrations;
pub mod mdns;
pub mod pairing;
pub mod protocol;
pub mod screens;
pub mod secrets;
pub mod server;
pub mod store;
pub mod tiles;
pub mod tls;
pub mod units;
