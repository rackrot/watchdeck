mod commands;
pub mod daemon;

use std::net::{Ipv4Addr, SocketAddr};

use daemon::mdns::{self, MdnsStatus};
use daemon::pairing::{Pairing, PAIR_PORT, PAIR_WINDOW};
use daemon::server::{Server, PORT};
use daemon::store::Store;
use daemon::tls::Identity;
use tauri::{Emitter, Manager};
use tokio::sync::{broadcast, watch};
use tokio_rustls::rustls::crypto::aws_lc_rs;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();
    // The process's one rustls crypto provider, for any TLS user that doesn't name its own.
    aws_lc_rs::default_provider()
        .install_default()
        .expect("nothing installs a crypto provider before run()");
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .setup(|app| {
            let dir = app.path().app_config_dir()?;
            let identity = Identity::load_or_create(&dir)
                .map_err(|e| format!("TLS identity in {}: {e}", dir.display()))?;
            log::info!("TLS identity in {}, fp={}", dir.display(), identity.fp());
            let store =
                Store::load(&dir).map_err(|e| format!("device state in {}: {e}", dir.display()))?;

            // A daemon that can't listen is useless, so a bind failure aborts startup.
            let server = tauri::async_runtime::block_on(Server::bind(
                ("0.0.0.0", PORT),
                identity.server_config()?,
                daemon::actions::default_runner(),
                store,
            ))?;
            log::info!("listening on {} (wss)", server.local_addr()?);

            let devices = server.devices();
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
            let mut events = server.subscribe();
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

            app.manage(commands::Daemon { devices, pairing });
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
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
