mod commands;
pub mod daemon;
mod tray;

use std::net::{Ipv4Addr, SocketAddr};
use std::sync::Arc;

use daemon::actions::Actions;
use daemon::config::Configs;
use daemon::mdns::{self, MdnsStatus};
use daemon::pairing::{Pairing, PAIR_PORT, PAIR_WINDOW};
use daemon::secrets::Secrets;
use daemon::server::{Server, PORT};
use daemon::store::Store;
use daemon::tls::Identity;
use tauri::{Emitter, Manager, WindowEvent};
use tauri_plugin_log::{RotationStrategy, Target, TargetKind};
use tokio::sync::{broadcast, watch};
use tokio_rustls::rustls::crypto::aws_lc_rs;

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
        .plugin(
            tauri_plugin_log::Builder::new()
                .level(log::LevelFilter::Info)
                .clear_targets()
                .targets(targets)
                .max_file_size(1_000_000)
                .rotation_strategy(RotationStrategy::KeepSome(3))
                .build(),
        )
        .plugin(tauri_plugin_autostart::init(
            tauri_plugin_autostart::MacosLauncher::LaunchAgent,
            Some(vec![tray::HIDDEN_ARG]),
        ))
        .plugin(tauri_plugin_opener::init())
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
        .setup(|app| {
            let dir = app.path().app_config_dir()?;
            let identity = Identity::load_or_create(&dir)
                .map_err(|e| format!("TLS identity in {}: {e}", dir.display()))?;
            log::info!("TLS identity in {}, fp={}", dir.display(), identity.fp());
            let store =
                Store::load(&dir).map_err(|e| format!("device state in {}: {e}", dir.display()))?;
            let secrets = Arc::new(Secrets::new(&dir));
            let configs = Configs::open(&dir, secrets.clone())
                .map_err(|e| format!("config in {}: {e}", dir.display()))?;
            // Shell actions run in the home directory, wherever the app was started from.
            let home = std::env::home_dir().ok_or("HOME is not set")?;
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
            if !std::env::args().any(|a| a == tray::HIDDEN_ARG) {
                tray::show_window(app.handle());
            }
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
                devices,
                pairing,
                configs,
                secrets,
            });
            tauri::async_runtime::spawn(server.run());
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
            commands::list_secrets,
            commands::set_secret,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
