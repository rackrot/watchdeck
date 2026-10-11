import { listen } from "@tauri-apps/api/event";
import { useCallback, useEffect, useState } from "react";
import {
  configDir,
  type DeviceInfo,
  iconUrl,
  listDevices,
  listSecrets,
  type TileReading,
  tileValues,
} from "./api";

/**
 * Subscribes to a daemon event, then reads the state once the listener is in place, so no change
 * falls between the two. `read` runs again on every event.
 */
function useDaemonState<T>(
  event: string,
  read: () => Promise<T>,
  initial: T,
): [T, string] {
  const [value, setValue] = useState(initial);
  const [error, setError] = useState("");
  useEffect(() => {
    const refresh = () => read().then(setValue, (e) => setError(String(e)));
    const unlisten = listen(event, refresh);
    unlisten.then(refresh, (e) => setError(String(e)));
    return () => {
      unlisten.then((stop) => stop());
    };
  }, [event, read]);
  return [value, error];
}

export const useDevices = (): [DeviceInfo[], string] =>
  useDaemonState("devices-changed", listDevices, []);

export const useTileReadings = (): TileReading[] =>
  useDaemonState("tile-values", tileValues, [])[0];

/** A function from an icon's file name to its URL, or null until the config dir is known. */
export function useIconUrls(): (name: string) => string | null {
  const [dir, setDir] = useState<string | null>(null);
  useEffect(() => {
    configDir().then(setDir, (e) => console.error("app config dir:", e));
  }, []);
  return (name) => (dir === null ? null : iconUrl(dir, name));
}

/** The secret names, and a function that reads them again. */
export function useSecrets(): [string[], () => void] {
  const [names, setNames] = useState<string[]>([]);
  const refresh = useCallback(() => {
    listSecrets().then(setNames, (e) => console.error("list_secrets:", e));
  }, []);
  useEffect(refresh, [refresh]);
  return [names, refresh];
}
