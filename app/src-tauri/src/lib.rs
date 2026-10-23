mod commands;
pub mod daemon;
mod tray;

use std::ffi::OsString;
use std::net::{Ipv4Addr, SocketAddr};
use std::path::{Path, PathBuf};
use std::sync::Arc;

use daemon::actions::Actions;
use daemon::config::Configs;
use daemon::mdns::{self, MdnsStatus};
use daemon::pairing::{Pairing, PAIR_PORT, PAIR_WINDOW};
use daemon::secrets::Secrets;
use daemon::server::{DaemonEvent, Server, PORT};
use daemon::store::Store;
use daemon::tiles::TileView;
use daemon::tls::Identity;
use tauri::{Emitter, Manager, WindowEvent};
use tauri_plugin_log::{RotationStrategy, Target, TargetKind};
use tokio::sync::{broadcast, watch};
use tokio_rustls::rustls::crypto::aws_lc_rs;

/// Where the daemon keeps its files: `$XDG_CONFIG_HOME/watchdeck`, or `~/.config/watchdeck` when
/// the variable is unset, empty or not an absolute path (the XDG spec says to ignore those).
fn config_dir(xdg: Option<OsString>, home: &Path) -> PathBuf {
    match xdg.map(PathBuf::from) {
        Some(base) if base.is_absolute() => base.join("watchdeck"),
        _ => home.join(".config").join("watchdeck"),
    }
}

/// Moves the folder an earlier version kept under Tauri's app config dir to `new`, so the TLS
/// identity survives and paired devices stay paired. Does nothing when `new` exists or `old`
/// doesn't. A rename, never a copy: it fails across filesystems, and the app then won't start.
fn migrate_config_dir(old: &Path, new: &Path) -> Result<(), String> {
    if !old.is_dir() {
        return Ok(());
    }
    if new.exists() {
        log::warn!(
            "not moving {} to {}: the new folder already exists, so the old one is ignored",
            old.display(),
            new.display()
        );
        return Ok(());
    }
    if let Some(parent) = new.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("creating {}: {e}", parent.display()))?;
    }
    std::fs::rename(old, new).map_err(|e| {
        format!(
            "moving the config folder from {} to {}: {e}. Move it by hand, then start the app again.",
            old.display(),
            new.display()
        )
    })?;
    log::info!(
        "moved the config folder from {} to {}",
        old.display(),
        new.display()
    );
    Ok(())
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // The process's one rustls crypto provider, for any TLS user that doesn't name its own.
    aws_lc_rs::default_provider()
        .install_default()
        .expect("nothing installs a crypto provider before run()");
    // Tauri's `tracing` feature stays off, so IPC bodies never reach the logs. Keep log lines
    // free of tokens, codes and secret values.
    let mut targets = vec![Target::new(TargetKind::LogDir { file_name: None })];
    if cfg!(debug_assertions) {
        targets.push(Target::new(TargetKind::Stdout));
    }
    tauri::Builder::default()
        // A second launch focuses the first one's window. Registered first, as the plugin asks.
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            tray::show_window(app)
        }))
        .plugin(tauri_plugin_autostart::init(
            tauri_plugin_autostart::MacosLauncher::LaunchAgent,
            Some(vec![tray::HIDDEN_ARG]),
        ))
        .plugin(tauri_plugin_opener::init())
        // The icon import dialog is opened from Rust (`import_icon`), never from the window.
        .plugin(tauri_plugin_dialog::init())
        // Closing the window hides it. The daemon and the device link keep running; Quit is in
        // the tray menu.
        .on_window_event(|window, event| {
            if let WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                if let Err(e) = window.hide() {
                    log::warn!("hiding the window: {e}");
                }
            }
        })
        .plugin(
            tauri_plugin_log::Builder::new()
                .level(log::LevelFilter::Info)
                .clear_targets()
                .targets(targets)
                .max_file_size(1_000_000)
                .rotation_strategy(RotationStrategy::KeepSome(3))
                .build(),
        )
        .setup(|app| {
            let home = std::env::home_dir().ok_or("HOME is not set")?;
            let dir = config_dir(std::env::var_os("XDG_CONFIG_HOME"), &home);
            migrate_config_dir(&app.path().app_config_dir()?, &dir)?;
            let identity = Identity::load_or_create(&dir)
                .map_err(|e| format!("TLS identity in {}: {e}", dir.display()))?;
            log::info!("TLS identity in {}, fp={}", dir.display(), identity.fp());
            let store =
                Store::load(&dir).map_err(|e| format!("device state in {}: {e}", dir.display()))?;
            let secrets = Arc::new(Secrets::new(&dir));
            let configs = Configs::open(&dir, secrets.clone())
                .map_err(|e| format!("config in {}: {e}", dir.display()))?;
            // The window loads icons through the asset protocol, from this one folder only. The
            // scope matches canonicalized request paths against patterns as given, so a symlinked
            // config dir needs its real path allowed too. The dir exists by now.
            let scope = app.asset_protocol_scope();
            scope.allow_directory(dir.join("icons"), false)?;
            if let Ok(real) = std::fs::canonicalize(&dir) {
                if real != dir {
                    scope.allow_directory(real.join("icons"), false)?;
                }
            }
            // Shell actions run in the home directory, wherever the app was started from.
            let actions = Actions::new(secrets.clone(), home)
                .map_err(|e| format!("HTTP client for actions: {e}"))?;

            // A daemon that can't listen is useless, so a bind failure aborts startup.
            let server = tauri::async_runtime::block_on(Server::bind(
                ("0.0.0.0", PORT),
                identity.server_config()?,
                store,
                configs.subscribe(),
                actions,
            ))?;
            log::info!("listening on {} (wss)", server.local_addr()?);
            tauri::async_runtime::spawn(configs.clone().watch_files());

            let devices = server.devices();
            // Subscribe before the tray reads its first status, so no device event slips between.
            let mut events = server.subscribe();
            let status_item = tray::build(app.handle(), &devices.list())?;
            let pairing = Pairing::new(
                SocketAddr::from((Ipv4Addr::UNSPECIFIED, PAIR_PORT)),
                PAIR_WINDOW,
                server.hostname().to_string(),
                identity.cert_pem().to_string(),
                devices.clone(),
            );
            let (mdns_status, mut mdns_rx) = watch::channel(MdnsStatus::Starting);
            tauri::async_runtime::spawn(mdns::advertise(
                server.hostname().to_string(),
                PORT,
                identity.fp(),
                pairing.advertised(),
                mdns_status,
            ));
            tauri::async_runtime::spawn(async move {
                while mdns_rx.changed().await.is_ok() {
                    log::info!("mDNS status: {:?}", *mdns_rx.borrow());
                }
            });

            // The window refetches the device list on `devices-changed`, and the pairing
            // dialog follows `pairing`.
            let handle = app.handle().clone();
            let tray_devices = devices.clone();
            tauri::async_runtime::spawn(async move {
                loop {
                    match events.recv().await {
                        Ok(DaemonEvent::LayoutPushed(device, rev)) => {
                            let payload = serde_json::json!({ "device": device, "rev": rev });
                            if let Err(e) = handle.emit("layout-pushed", payload) {
                                log::error!("emitting layout-pushed: {e}");
                            }
                            continue;
                        }
                        Ok(DaemonEvent::LayoutApplied(device, rev)) => {
                            let payload = serde_json::json!({ "device": device, "rev": rev });
                            if let Err(e) = handle.emit("layout-applied", payload) {
                                log::error!("emitting layout-applied: {e}");
                            }
                            continue;
                        }
                        Ok(event) => log::info!("{event:?}"),
                        Err(broadcast::error::RecvError::Lagged(n)) => {
                            log::warn!("missed {n} daemon events")
                        }
                        Err(broadcast::error::RecvError::Closed) => break,
                    }
                    if let Err(e) = handle.emit("devices-changed", ()) {
                        log::error!("emitting devices-changed: {e}");
                    }
                    if let Err(e) = status_item.set_text(tray::status_line(&tray_devices.list())) {
                        log::error!("updating the tray status: {e}");
                    }
                }
            });
            let handle = app.handle().clone();
            let mut pairing_rx = pairing.subscribe();
            tauri::async_runtime::spawn(async move {
                while pairing_rx.changed().await.is_ok() {
                    let state = pairing_rx.borrow_and_update().clone();
                    if let Err(e) = handle.emit("pairing", state) {
                        log::error!("emitting pairing: {e}");
                    }
                }
            });

            // The window follows `tile-values` for the live values on its preview.
            let handle = app.handle().clone();
            let mut tile_rx = devices.tile_values();
            tauri::async_runtime::spawn(async move {
                while tile_rx.changed().await.is_ok() {
                    let views = TileView::all(&tile_rx.borrow_and_update());
                    if let Err(e) = handle.emit("tile-values", views) {
                        log::error!("emitting tile-values: {e}");
                    }
                }
            });

            // The window follows `config-status` to show why config.toml isn't live.
            let handle = app.handle().clone();
            let mut config_status = configs.status();
            tauri::async_runtime::spawn(async move {
                while config_status.changed().await.is_ok() {
                    let status = config_status.borrow_and_update().clone();
                    if let Err(e) = handle.emit("config-status", status) {
                        log::error!("emitting config-status: {e}");
                    }
                }
            });

            app.manage(commands::Daemon {
                dir,
                devices,
                pairing,
                configs,
                secrets,
            });
            tauri::async_runtime::spawn(server.run());

            if !std::env::args().any(|a| a == tray::HIDDEN_ARG) {
                tray::show_window(app.handle());
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::list_devices,
            commands::forget_device,
            commands::start_pairing,
            commands::pairing_state,
            commands::confirm_pairing,
            commands::cancel_pairing,
            commands::get_config,
            commands::save_config,
            commands::convert_status_bar,
            commands::list_secrets,
            commands::set_secret,
            commands::delete_secret,
            commands::import_icon,
            commands::list_icons,
            commands::list_gpus,
            commands::test_action,
            commands::tile_values,
            commands::config_dir,
        ])
        .build(tauri::generate_context!())
        .expect("error while building tauri application")
        .run(|_, _| {});
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dir(xdg: Option<&str>) -> PathBuf {
        config_dir(xdg.map(OsString::from), Path::new("/home/me"))
    }

    #[test]
    fn xdg_config_home_wins_when_absolute() {
        assert_eq!(dir(Some("/x/cfg")), PathBuf::from("/x/cfg/watchdeck"));
    }

    #[test]
    fn falls_back_to_home_when_unset_empty_or_relative() {
        let fallback = PathBuf::from("/home/me/.config/watchdeck");
        assert_eq!(dir(None), fallback);
        assert_eq!(dir(Some("")), fallback);
        assert_eq!(dir(Some("rel/cfg")), fallback);
    }

    #[test]
    fn migration_moves_the_old_folder_once() {
        let tmp = tempfile::tempdir().unwrap();
        let old = tmp.path().join("old");
        let new = tmp.path().join("deep/new");
        std::fs::create_dir(&old).unwrap();
        std::fs::write(old.join("identity.key"), "k").unwrap();

        migrate_config_dir(&old, &new).unwrap();
        assert!(!old.exists());
        assert_eq!(
            std::fs::read_to_string(new.join("identity.key")).unwrap(),
            "k"
        );

        // With the new folder in place, a reappearing old one is left alone.
        std::fs::create_dir(&old).unwrap();
        migrate_config_dir(&old, &new).unwrap();
        assert!(old.is_dir());
    }

    #[test]
    fn migration_skips_when_there_is_no_old_folder() {
        let tmp = tempfile::tempdir().unwrap();
        let new = tmp.path().join("new");
        migrate_config_dir(&tmp.path().join("old"), &new).unwrap();
        assert!(!new.exists());
    }
}
