//! Tauri commands for the window. They only forward to the daemon, which owns the rules.

use std::sync::Arc;

use tauri::State;

use crate::daemon::pairing::{Pairing, PairingState};
use crate::daemon::server::{DeviceInfo, Devices};

pub struct Daemon {
    pub devices: Devices,
    pub pairing: Arc<Pairing>,
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
