//! Tauri commands for the window. They only forward to the daemon, which owns the rules.

use std::path::{Path, PathBuf};
use std::sync::Arc;

use serde::Serialize;
use tauri::{AppHandle, State};
use tauri_plugin_dialog::DialogExt;

use crate::daemon::actions::ActionResult;
use crate::daemon::config::{Config, Configs, StatusBarConfig, StatusBarDefaults};
use crate::daemon::gpu::GpuList;
use crate::daemon::pairing::{Pairing, PairingState};
use crate::daemon::protocol::Feed;
use crate::daemon::secrets::Secrets;
use crate::daemon::server::{DeviceInfo, Devices};
use crate::daemon::tiles::TileView;

pub struct Daemon {
    pub dir: PathBuf,
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
    /// The steps a status bar metric without a key gets, for each temperature unit, so the editor
    /// holds no copy.
    status_bar_defaults: StatusBarDefaults,
}

#[tauri::command]
pub fn get_config(daemon: State<'_, Daemon>) -> ConfigView {
    let status = daemon.configs.status().borrow().clone();
    ConfigView {
        config: daemon.configs.current().config.clone(),
        revision: status.revision,
        errors: status.errors,
        loaded: status.loaded,
        status_bar_defaults: StatusBarDefaults::default(),
    }
}

/// `bar` with its units changed to `temp_unit` and `net_unit` and its steps converted to match,
/// for the status bar form. Nothing is saved.
#[tauri::command]
pub fn convert_status_bar(
    bar: StatusBarConfig,
    temp_unit: String,
    net_unit: String,
) -> Result<StatusBarConfig, String> {
    bar.convert(&temp_unit, &net_unit)
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

/// The -1 page's current snapshot, which is also what the boards get. It is the starting point
/// for the `feed-changed` event.
#[tauri::command]
pub fn feed_snapshot(daemon: State<'_, Daemon>) -> Feed {
    (*daemon.devices.feed().snapshot()).clone()
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

/// The folder that holds `config.toml` and `icons/`, for the window to build icon URLs from.
#[tauri::command]
pub fn config_dir(daemon: State<'_, Daemon>) -> String {
    daemon.dir.display().to_string()
}

/// The files in `icons/` that a button's `icon` could name.
#[tauri::command]
pub async fn list_icons(daemon: State<'_, Daemon>) -> Result<Vec<String>, String> {
    let configs = daemon.configs.clone();
    blocking(move || configs.list_icons()).await?
}

/// The host's GPUs and which one `auto` picks, for the status bar form.
#[tauri::command]
pub async fn list_gpus() -> Result<GpuList, String> {
    blocking(|| GpuList::scan(Path::new("/sys"), Path::new("/proc"))).await
}

/// Runs button `button`'s action from the live config, as a device press would, and returns what
/// the device would see. The window names only the button, never an action, and no device gets
/// a press result.
#[tauri::command]
pub async fn test_action(
    daemon: State<'_, Daemon>,
    button: String,
) -> Result<ActionResult, String> {
    daemon.devices.test_press(&button).await
}

/// Asks the user for a PNG, JPEG, WebP or SVG in a native file dialog, copies it into the icons
/// directory and returns the file name for a button's `icon`, or `None` if they cancel. The
/// dialog is opened here, so the window never names a path, and the file must pass the same size
/// and decoding checks as a configured icon.
#[tauri::command]
pub async fn import_icon(
    app: AppHandle,
    daemon: State<'_, Daemon>,
) -> Result<Option<String>, String> {
    let Some(path) = pick_image(&app, "Import an icon").await? else {
        return Ok(None);
    };
    let configs = daemon.configs.clone();
    blocking(move || configs.import_icon(&path))
        .await?
        .map(Some)
}

/// The files in `screens/` that `[sleep]`'s `image` could name.
#[tauri::command]
pub async fn list_screen_images(daemon: State<'_, Daemon>) -> Result<Vec<String>, String> {
    let configs = daemon.configs.clone();
    blocking(move || configs.list_screen_images()).await?
}

/// Like [`import_icon`], for the sleep screen: copies the picked image into `screens/` and
/// returns the file name for `[sleep]`'s `image`, or `None` if the user cancels.
#[tauri::command]
pub async fn import_screen_image(
    app: AppHandle,
    daemon: State<'_, Daemon>,
) -> Result<Option<String>, String> {
    let Some(path) = pick_image(&app, "Import a sleep screen image").await? else {
        return Ok(None);
    };
    let configs = daemon.configs.clone();
    blocking(move || configs.import_screen_image(&path))
        .await?
        .map(Some)
}

/// Asks the user for a PNG, JPEG, WebP or SVG in a native file dialog, or `None` if they cancel.
async fn pick_image(app: &AppHandle, title: &str) -> Result<Option<std::path::PathBuf>, String> {
    let (tx, rx) = tokio::sync::oneshot::channel();
    app.dialog()
        .file()
        .set_title(title)
        .add_filter("Images", &["png", "jpg", "jpeg", "webp", "svg"])
        .pick_file(move |picked| {
            // The receiver only goes away if the command was dropped.
            let _ = tx.send(picked);
        });
    let picked = rx
        .await
        .map_err(|_| "the file dialog closed unexpectedly".to_string())?;
    picked
        .map(|picked| {
            picked
                .into_path()
                .map_err(|e| format!("not a local file: {e}"))
        })
        .transpose()
}

/// Runs `work` on the blocking pool, since it may decode images or wait on the config lock.
async fn blocking<T: Send + 'static>(
    work: impl FnOnce() -> T + Send + 'static,
) -> Result<T, String> {
    tauri::async_runtime::spawn_blocking(work)
        .await
        .map_err(|e| format!("background task failed: {e}"))
}
