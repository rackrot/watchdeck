import { listen } from "@tauri-apps/api/event";
import { useCallback, useEffect, useRef, useState } from "react";
import {
  configDir,
  type DeviceInfo,
  EMPTY_FEED,
  type Feed,
  feedSnapshot,
  iconUrl,
  listDevices,
  listSecrets,
  screenUrl,
  type TileReading,
  tileValues,
} from "./api";
import { SCREEN_W } from "./preview";

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

/** The -1 page's snapshot, kept current by `feed-changed`. */
export const useFeed = (): Feed =>
  useDaemonState("feed-changed", feedSnapshot, EMPTY_FEED)[0];

/** A function from a file name to its URL, or null until the config dir is known. */
function useFileUrls(
  url: (dir: string, name: string) => string,
): (name: string) => string | null {
  const [dir, setDir] = useState<string | null>(null);
  useEffect(() => {
    configDir().then(setDir, (e) => {
      console.error("app config dir:", e);
    });
  }, []);
  return (name) => (dir === null ? null : url(dir, name));
}

export const useIconUrls = () => useFileUrls(iconUrl);
export const useScreenUrls = () => useFileUrls(screenUrl);

/** The width of an element, kept current. */
export function useWidth() {
  const ref = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(SCREEN_W);
  useEffect(() => {
    const el = ref.current;
    if (!el) {
      return;
    }
    const watch = new ResizeObserver(() => setWidth(el.clientWidth));
    watch.observe(el);
    setWidth(el.clientWidth);
    return () => watch.disconnect();
  }, []);
  return [ref, width] as const;
}

/** The secret names, and a function that reads them again. */
export function useSecrets(): [string[], () => void] {
  const [names, setNames] = useState<string[]>([]);
  const refresh = useCallback(() => {
    listSecrets().then(setNames, (e) => {
      console.error("list_secrets:", e);
    });
  }, []);
  useEffect(refresh, [refresh]);
  return [names, refresh];
}
