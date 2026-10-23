import { convertFileSrc, invoke } from "@tauri-apps/api/core";
import type { Config, ConfigView, TileConfig } from "./config";

/** `caps` are what the live session's device supports, such as "fonts". Empty while offline. */
export type DeviceInfo = { id: string; connected: boolean; caps: string[] };

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

/** Rejects, naming the buttons, while the live config still uses the secret. */
export const deleteSecret = (name: string) =>
  invoke<void>("delete_secret", { name });

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

/** The files in the icons directory that a button's `icon` could name. */
export const listIcons = () => invoke<string[]>("list_icons");

/** Mirrors `GpuInfo` in `src-tauri/src/daemon/gpu.rs`. `id` is the PCI address. */
export type GpuInfo = {
  id: string;
  name: string;
  driver: string | null;
  readable: boolean;
  boot_vga: boolean;
};
/** `auto` is the id the status bar's `auto` picks now, or null if no GPU is readable. */
export type GpuList = { gpus: GpuInfo[]; auto: string | null };
export const listGpus = () => invoke<GpuList>("list_gpus");

/** Mirrors `ActionResult` in `src-tauri/src/daemon/actions.rs`: what a device would be told. */
export type ActionResult = {
  ok: boolean;
  code: number;
  summary: string;
  text: string;
  ms: number;
};

/**
 * Runs a button's saved action, as a press would, and returns the result. The daemon takes the
 * action from its live config, so only the id goes over. No device is told. Rejects for an
 * unknown button.
 */
export const testAction = (button: string) =>
  invoke<ActionResult>("test_action", { button });

/**
 * Turns an icon file name into a URL the webview may load, through the asset protocol, whose
 * scope is only the icons directory, which the daemon allows at startup. `configDir` is the
 * daemon's config folder, from `config_dir`.
 */
export const iconUrl = (configDir: string, name: string) =>
  convertFileSrc(`${configDir}/icons/${name}`);

export const configDir = () => invoke<string>("config_dir");
