import { invoke } from "@tauri-apps/api/core";

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
