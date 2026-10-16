// The config as Rust serializes it: `Config` in app/src-tauri/src/daemon/config.rs. Fields that
// Rust leaves out when empty are optional here. Rust is the authority on what is valid.

export type ShellAction = {
  kind: "shell";
  argv: string[];
  timeout_s: number;
};

export type HttpAction = {
  kind: "http";
  method: string;
  url: string;
  body?: string;
  /** Header name -> value, sent as written. */
  headers?: Record<string, string>;
  /** Header name -> the name of a secret. */
  secret_headers?: Record<string, string>;
  insecure_http?: boolean;
  timeout_s: number;
};

export type Action = ShellAction | HttpAction;

export type TileConfig = {
  every_s: number;
  json_pointer?: string;
  source: Action;
};

export type ButtonConfig = {
  id: string;
  /** `[column, row]`, from 0. */
  pos: [number, number];
  label: string;
  bg: string;
  fg: string;
  icon?: string;
  action: Action;
  tile?: TileConfig;
};

export type Config = {
  version: number;
  layout: { cols: number; rows: number };
  /** Opt-in error reporting. Edited by hand; the window only keeps it through a save. */
  sentry?: { dsn: string; pii: boolean };
  /** The TOML array of tables is `[[button]]`. */
  button: ButtonConfig[];
};

/** `get_config`, and the `config-status` event without the config. */
export type ConfigStatus = {
  /** Pass to `save_config`. Refused if the file has changed since. */
  revision: string;
  /** Why the file on disk is not live. Empty when it is. */
  errors: string[];
};

/**
 * `get_config`. `loaded` is false when no good config has loaded since the app started, so
 * `config` is an empty grid and not any version of the file.
 */
export type ConfigView = ConfigStatus & { config: Config; loaded: boolean };

export const METHODS = [
  "GET",
  "HEAD",
  "POST",
  "PUT",
  "PATCH",
  "DELETE",
] as const;
export const DEFAULT_TIMEOUT_S = 10;
export const MIN_EVERY_S = 2;

export const newShell = (): ShellAction => ({
  kind: "shell",
  argv: [""],
  timeout_s: DEFAULT_TIMEOUT_S,
});

export const newHttp = (): HttpAction => ({
  kind: "http",
  method: "GET",
  url: "https://",
  timeout_s: DEFAULT_TIMEOUT_S,
});

/** An id like `button-3` that no button has. */
function freeId(taken: ButtonConfig[]): string {
  const ids = new Set(taken.map((b) => b.id));
  let n = taken.length + 1;
  while (ids.has(`button-${n}`)) {
    n++;
  }
  return `button-${n}`;
}

export function newButton(config: Config, pos: [number, number]): ButtonConfig {
  return {
    id: freeId(config.button),
    pos,
    label: "New",
    bg: "#1e88e5",
    fg: "#ffffff",
    action: newShell(),
  };
}
