// Prevents additional console window on Windows in release, DO NOT REMOVE!!
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    // `watchdeck notify ...` and the other push commands, before anything of the app starts.
    if let Some(code) = watchdeck_lib::cli::main() {
        std::process::exit(code);
    }

    // WebKitGTK's DMA-BUF renderer crashes on Wayland with NVIDIA ("Error 71 (Protocol error)
    // dispatching to Wayland display"), which takes the daemon down with the window. Set the
    // variable to 0 in the environment to opt back in. This runs before any threads start.
    #[cfg(target_os = "linux")]
    if std::env::var_os("WEBKIT_DISABLE_DMABUF_RENDERER").is_none() {
        std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
    }

    watchdeck_lib::run()
}
