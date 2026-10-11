import { convertFileSrc, invoke } from "@tauri-apps/api/core";
import { appConfigDir } from "@tauri-apps/api/path";
import type { Config, ConfigView, TileConfig } from "./config";

export type DeviceInfo = { id: string; connected: boolean };

/** Mirrors `PairingState` in `src-tauri/src/daemon/pairing.rs`, sent as the `pairing` event. */
export type PairingState =
  | { state: "closed" }
  | { state: "waiting" }
  | { state: "confirm"; device: string; peer: string }
  | { state: "paired"; device: string }
  | { state: "failed"; reason: string };

export const listDevices = () => invoke<DeviceInfo[]>("list_devices");
export const forgetDevice = (device: string) =>
  invoke<void>("forget_device", { device });
export const startPairing = () => invoke<void>("start_pairing");
export const pairingState = () => invoke<PairingState>("pairing_state");
export const confirmPairing = (code: string) =>
  invoke<void>("confirm_pairing", { code });
export const cancelPairing = () => invoke<void>("cancel_pairing");

export const getConfig = () => invoke<ConfigView>("get_config");

/**
 * Saves `config` over the file at `revision`. Resolves to the new revision. Rejects with the
 * rules that were broken, and nothing was written, or with one message if the call itself failed.
 */
export async function saveConfig(
  config: Config,
  revision: string,
): Promise<string> {
  try {
    return await invoke<string>("save_config", { config, revision });
  } catch (e) {
    throw Array.isArray(e) ? (e as string[]) : [String(e)];
  }
}

export const listSecrets = () => invoke<string[]>("list_secrets");
/** Write-only: nothing returns a secret's value. */
export const setSecret = (name: string, value: string) =>
  invoke<void>("set_secret", { name, value });

/** The latest reading of a polled tile, as `tile-values` carries it. */
export type TileReading = {
  btn: string;
  tile: TileConfig;
  text: string;
  state: "ok" | "bad";
};
export const tileValues = () => invoke<TileReading[]>("tile_values");

/** Opens the native file dialog in the daemon, imports the pick, and returns its name or null. */
export const importIcon = () => invoke<string | null>("import_icon");

/**
 * Turns an icon file name into a URL the webview may load, through the asset protocol, whose
 * scope in tauri.conf.json is only the icons directory. `configDir` is `appConfigDir()`.
 */
export const iconUrl = (configDir: string, name: string) =>
  convertFileSrc(`${configDir}/icons/${name}`);

export const configDir = () => appConfigDir();
