//! Tauri commands for the window. They only forward to the daemon, which owns the rules.

use std::sync::Arc;

use serde::Serialize;
use tauri::{AppHandle, State};
use tauri_plugin_dialog::DialogExt;

use crate::daemon::actions::ActionResult;
use crate::daemon::config::{Config, Configs};
use crate::daemon::pairing::{Pairing, PairingState};
use crate::daemon::secrets::Secrets;
use crate::daemon::server::{DeviceInfo, Devices};
use crate::daemon::tiles::TileView;

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
    /// The file text this view is about, to pass back to `save_config`. A save is refused if the
    /// file has changed since. When `errors` is not empty the file is not live, `config` is the
    /// last good one, and a save with this revision replaces the file with it.
    revision: String,
    /// Why `config.toml` on disk is not live, or empty when it is.
    errors: Vec<String>,
    /// False if no good config has loaded since the app started, so `config` is an empty grid.
    loaded: bool,
}

#[tauri::command]
pub fn get_config(daemon: State<'_, Daemon>) -> ConfigView {
    let status = daemon.configs.status().borrow().clone();
    ConfigView {
        config: daemon.configs.current().config.clone(),
        revision: status.revision,
        errors: status.errors,
        loaded: status.loaded,
    }
}

/// Checks and writes the config. Returns the new revision, or every rule it breaks, in which
/// case nothing is written.
#[tauri::command]
pub async fn save_config(
    daemon: State<'_, Daemon>,
    config: Config,
    revision: String,
) -> Result<String, Vec<String>> {
    let configs = daemon.configs.clone();
    blocking(move || configs.save(config, &revision))
        .await
        .map_err(|e| vec![e])?
}

/// The latest reading of each polled tile. Readings change as devices connect, so this is the
/// starting point for the `tile-values` event.
#[tauri::command]
pub fn tile_values(daemon: State<'_, Daemon>) -> Vec<TileView> {
    TileView::all(&daemon.devices.tile_values().borrow())
}

/// Secret names. Values never leave the daemon.
#[tauri::command]
pub fn list_secrets(daemon: State<'_, Daemon>) -> Result<Vec<String>, String> {
    daemon.secrets.names().map_err(|e| e.to_string())
}

/// Adds or replaces a secret. There is no command that reads one back.
#[tauri::command]
pub async fn set_secret(
    daemon: State<'_, Daemon>,
    name: String,
    value: String,
) -> Result<(), String> {
    let (configs, secrets) = (daemon.configs.clone(), daemon.secrets.clone());
    blocking(move || {
        secrets.set(&name, &value)?;
        // A config held back for want of this secret can go live now. reload logs its own outcome.
        let _ = configs.reload();
        Ok(())
    })
    .await?
}

/// Removes a secret, unless the live config still uses it. The error names the buttons.
#[tauri::command]
pub async fn delete_secret(daemon: State<'_, Daemon>, name: String) -> Result<(), String> {
    let configs = daemon.configs.clone();
    blocking(move || configs.delete_secret(&name)).await?
}

/// The files in `icons/` that a button's `icon` could name.
#[tauri::command]
pub async fn list_icons(daemon: State<'_, Daemon>) -> Result<Vec<String>, String> {
    let configs = daemon.configs.clone();
    blocking(move || configs.list_icons()).await?
}

/// Runs button `button`'s action from the live config, as a device press would, and returns what
/// the device would see. The window names only the button, never an action, and no device gets
/// the result.
#[tauri::command]
pub async fn test_action(
    daemon: State<'_, Daemon>,
    button: String,
) -> Result<ActionResult, String> {
    daemon.devices.test_press(&button).await
}

/// Asks the user for a PNG, JPEG or WebP in a native file dialog, copies it into the icons
/// directory and returns the file name for a button's `icon`, or `None` if they cancel. The
/// dialog is opened here, so the window never names a path, and the file must pass the same size
/// and decoding checks as a configured icon.
#[tauri::command]
pub async fn import_icon(
    app: AppHandle,
    daemon: State<'_, Daemon>,
) -> Result<Option<String>, String> {
    let (tx, rx) = tokio::sync::oneshot::channel();
    app.dialog()
        .file()
        .set_title("Import an icon")
        .add_filter("Images", &["png", "jpg", "jpeg", "webp"])
        .pick_file(move |picked| {
            // The receiver only goes away if the command was dropped.
            let _ = tx.send(picked);
        });
    let picked = rx
        .await
        .map_err(|_| "the file dialog closed unexpectedly".to_string())?;
    let Some(picked) = picked else {
        return Ok(None);
    };
    let path = picked
        .into_path()
        .map_err(|e| format!("not a local file: {e}"))?;
    let configs = daemon.configs.clone();
    blocking(move || configs.import_icon(&path))
        .await?
        .map(Some)
}

/// Runs `work` on the blocking pool, since it may decode images or wait on the config lock.
async fn blocking<T: Send + 'static>(
    work: impl FnOnce() -> T + Send + 'static,
) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(work)
        .await
        .map_err(|e| format!("background task failed: {e}"))
}
