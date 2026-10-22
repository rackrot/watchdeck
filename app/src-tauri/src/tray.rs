//! The tray icon and its menu, plus the window helpers they share with single-instance.
//!
//! Linux StatusNotifierItem trays have no tooltip and no click events, so the device status
//! lives in a disabled menu item.

use tauri::menu::{CheckMenuItem, Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Manager, Wry};
use tauri_plugin_autostart::ManagerExt;

use crate::daemon::server::DeviceInfo;

/// Passed by the autostart entry so a login start stays in the tray.
pub const HIDDEN_ARG: &str = "--hidden";

/// The menu's status line: the connected devices, or "Offline".
pub fn status_line(devices: &[DeviceInfo]) -> String {
    let connected: Vec<&str> = devices
        .iter()
        .filter(|d| d.connected)
        .map(|d| d.id.as_str())
        .collect();
    if connected.is_empty() {
        "Offline".to_string()
    } else {
        format!("Connected: {}", connected.join(", "))
    }
}

/// Shows, unminimizes and focuses the main window. Focus can be refused by the compositor.
pub fn show_window(app: &AppHandle) {
    let Some(window) = app.get_webview_window("main") else {
        log::error!("no main window to show");
        return;
    };
    for (what, result) in [
        ("show", window.show()),
        ("unminimize", window.unminimize()),
        ("focus", window.set_focus()),
    ] {
        if let Err(e) = result {
            log::warn!("main window {what}: {e}");
        }
    }
}

/// Builds the tray. Returns the status item, for the caller to update as devices come and go.
pub fn build(app: &AppHandle, devices: &[DeviceInfo]) -> tauri::Result<MenuItem<Wry>> {
    let status = MenuItem::with_id(app, "status", status_line(devices), false, None::<&str>)?;
    let open = MenuItem::with_id(app, "open", "Open window", true, None::<&str>)?;
    let autostart = CheckMenuItem::with_id(
        app,
        "autostart",
        "Start at login",
        true,
        app.autolaunch().is_enabled().unwrap_or_else(|e| {
            log::warn!("reading the autostart state: {e}");
            false
        }),
        None::<&str>,
    )?;
    let quit = MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?;
    let menu = Menu::with_items(
        app,
        &[
            &status,
            &PredefinedMenuItem::separator(app)?,
            &open,
            &autostart,
            &PredefinedMenuItem::separator(app)?,
            &quit,
        ],
    )?;

    TrayIconBuilder::new()
        .icon(
            app.default_window_icon()
                .cloned()
                .ok_or(tauri::Error::WindowNotFound)?,
        )
        .menu(&menu)
        .on_menu_event(move |app, event| match event.id.as_ref() {
            "open" => show_window(app),
            "autostart" => {
                let launch = app.autolaunch();
                let result = match launch.is_enabled() {
                    Ok(true) => launch.disable(),
                    Ok(false) => launch.enable(),
                    Err(e) => Err(e),
                };
                if let Err(e) = result {
                    log::error!("toggling autostart: {e}");
                }
                // Show what the system holds, whether or not the toggle worked.
                if let Err(e) = autostart.set_checked(launch.is_enabled().unwrap_or(false)) {
                    log::warn!("updating the autostart checkbox: {e}");
                }
            }
            "quit" => app.exit(0),
            _ => {}
        })
        .build(app)?;
    Ok(status)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dev(id: &str, connected: bool) -> DeviceInfo {
        DeviceInfo {
            id: id.to_string(),
            connected,
            caps: Vec::new(),
        }
    }

    #[test]
    fn status_names_connected_devices() {
        assert_eq!(status_line(&[]), "Offline");
        assert_eq!(status_line(&[dev("a", false)]), "Offline");
        assert_eq!(
            status_line(&[dev("a", true), dev("b", false), dev("c", true)]),
            "Connected: a, c"
        );
    }
}
