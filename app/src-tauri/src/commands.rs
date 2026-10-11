//! Tauri commands for the window. They only forward to the daemon, which owns the rules.

use std::sync::Arc;

use serde::Serialize;
use tauri::State;

use crate::daemon::config::{Config, Configs};
use crate::daemon::pairing::{Pairing, PairingState};
use crate::daemon::secrets::Secrets;
use crate::daemon::server::{DeviceInfo, Devices};

pub struct Daemon {
    pub devices: Devices,
    pub pairing: Arc<Pairing>,
    pub configs: Arc<Configs>,
    pub secrets: Arc<Secrets>,
}

#[tauri::command]
pub fn list_devices(daemon: State<'_, Daemon>) -> Vec<DeviceInfo> {
    daemon.devices.list()
}

/// Forgets a device and closes its live session.
#[tauri::command]
pub fn forget_device(daemon: State<'_, Daemon>, device: String) -> Result<(), String> {
    match daemon.devices.forget(&device) {
        Ok(true) => Ok(()),
        Ok(false) => Err(format!("{device} is not paired.")),
        Err(e) => Err(format!("Could not forget {device}: {e}")),
    }
}

/// Opens the pairing listener. Progress arrives as `pairing` events.
#[tauri::command]
pub async fn start_pairing(daemon: State<'_, Daemon>) -> Result<(), String> {
    match daemon.pairing.start().await {
        Ok(_) => Ok(()),
        Err(e) => Err(format!("Could not open the pairing port: {e}")),
    }
}

/// The current pairing state, for a window that missed a `pairing` event or reloaded.
#[tauri::command]
pub fn pairing_state(daemon: State<'_, Daemon>) -> PairingState {
    daemon.pairing.subscribe().borrow().clone()
}

/// Submits the code the device shows.
#[tauri::command]
pub fn confirm_pairing(daemon: State<'_, Daemon>, code: String) -> Result<(), String> {
    daemon.pairing.confirm(&code)
}

#[tauri::command]
pub async fn cancel_pairing(daemon: State<'_, Daemon>) -> Result<(), String> {
    daemon.pairing.cancel().await;
    Ok(())
}

#[derive(Serialize)]
pub struct ConfigView {
    /// The live config.
    config: Config,
    /// Pass back to `save_config`. A save from an older revision is refused.
    revision: String,
    /// Why `config.toml` on disk is not live, or empty when it is.
    errors: Vec<String>,
}

#[tauri::command]
pub fn get_config(daemon: State<'_, Daemon>) -> ConfigView {
    let status = daemon.configs.status().borrow().clone();
    ConfigView {
        config: (*daemon.configs.current()).clone(),
        revision: status.revision,
        errors: status.errors,
    }
}

/// Checks and writes the config. Returns the new revision, or every rule it breaks, in which
/// case nothing is written.
#[tauri::command]
pub fn save_config(
    daemon: State<'_, Daemon>,
    config: Config,
    revision: String,
) -> Result<String, Vec<String>> {
    daemon.configs.save(config, &revision)
}

/// Secret names. Values never leave the daemon.
#[tauri::command]
pub fn list_secrets(daemon: State<'_, Daemon>) -> Result<Vec<String>, String> {
    daemon.secrets.names().map_err(|e| e.to_string())
}

/// Adds or replaces a secret. There is no command that reads one back.
#[tauri::command]
pub fn set_secret(daemon: State<'_, Daemon>, name: String, value: String) -> Result<(), String> {
    daemon.secrets.set(&name, &value)?;
    // A config held back for want of this secret can go live now. reload logs its own outcome.
    let _ = daemon.configs.reload();
    Ok(())
}
