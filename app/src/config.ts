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
  /** Skip certificate verification, for a self-signed https service. */
  insecure_tls?: boolean;
  timeout_s: number;
};

export type Action = ShellAction | HttpAction;

export type TileConfig = {
  every_s: number;
  json_pointer?: string;
  /** The value's font id. Absent: the button's font. */
  font?: string;
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
  /** A font id for the label, and for the tile value unless `tile.font` says otherwise. Absent: the grid's font. */
  font?: string;
  /** A `[[page]]` id. Absent: the first page. Only with `Config.page` tables. */
  page?: string;
  action: Action;
  tile?: TileConfig;
};

/** A `[[page]]` table. */
export type PageConfig = {
  id: string;
  /** What the editor's tab shows instead of the id. Config only: it never reaches a device. */
  label?: string;
};

/** A value at or past `at`, in the metric's display unit, is drawn in `color` (`#rrggbb`). */
export type Step = { at: number; color: string };

/** The status bar's metrics, as the wire and `[status_bar.thresholds]` name them. */
export type Metric =
  | "cpu"
  | "mem"
  | "gpu"
  | "cpu_temp"
  | "gpu_temp"
  | "net_rx"
  | "net_tx";

/** Up to 3 steps with `at` rising. An absent key takes the defaults; `[]` is never colored. */
export type Thresholds = { [M in Metric]?: Step[] };

export type TempUnit = "c" | "f";
export type NetUnit = "si" | "binary" | "bits";

/** `[status_bar]`. Absent keys take their defaults. */
export type StatusBarConfig = {
  /** `"auto"`, `"off"`, or a PCI address such as `0000:01:00.0`. Absent: auto. */
  gpu?: string;
  /** The temperature steps are in this unit. Absent: `c`. */
  temp_unit?: TempUnit;
  /** The rate steps are in MB/s (`si`), MiB/s (`binary`) or Mb/s (`bits`). Absent: `si`. */
  net_unit?: NetUnit;
  thresholds?: Thresholds;
};

/** The steps a metric without a key gets, for each temperature unit (`ConfigView`). */
export type StatusBarDefaults = Record<TempUnit, Required<Thresholds>>;

/** `[sleep]`. Absent keys take the device's defaults. The strings are checked by Rust, as `[status_bar]`'s are. */
export type SleepConfig = {
  /** A file in `screens/` beside the config. Absent: the logo. */
  image?: string;
  /** `cover` or `contain`. */
  fit?: string;
  /** `#rrggbb`. */
  bg?: string;
  /** `#rrggbb`, the text's color. */
  fg?: string;
  /** Any of `clock`, `date`, `host` and `text`, each once. */
  show?: string[];
  /** Needed when `show` has `text`. Printable ASCII, at most 47 characters. */
  text?: string;
  /** `center`, `top_left`, `top_right`, `bottom_left` or `bottom_right`. */
  anchor?: string;
  /** `arc`, `dots`, `bar`, `pulse` or `none`. */
  spinner?: string;
  /** `#rrggbb`. */
  spinner_color?: string;
  /** 0 to 3600 seconds. 0 never sleeps for a lost link. */
  offline_after_s?: number;
  /** 0 to 100, the backlight while asleep. */
  backlight?: number;
};

/** What a device uses for an absent `[sleep]` key: `sleepcfg_defaults` in firmware/main/sleepcfg.c. */
export const SLEEP_DEFAULTS = {
  fit: "cover",
  bg: "#000000",
  fg: "#d0d0d0",
  show: ["clock", "date"],
  anchor: "bottom_left",
  spinner: "arc",
  spinner_color: "#38bdf8",
  offline_after_s: 10,
  backlight: 100,
} as const;

/** `[push]`. */
export type PushConfig = {
  /** Also serves https on the LAN, with the daemon's own certificate. */
  lan?: boolean;
};

/** `[[integration]]`, tagged by `kind`. Its `id` and `label` name its notes and cards. */
export type IntegrationConfig =
  | {
      kind: "push";
      id: string;
      label: string;
      /** The name of a secret in `secrets.toml`, at least 32 characters. */
      token_secret: string;
    }
  | {
      kind: "stat";
      id: string;
      label: string;
      every_s: number;
      json_pointer?: string;
      source: Action;
    };

/** Rust's limits on `[[integration]]`: `MAX_INTEGRATIONS`, `MAX_INTEGRATION_ID` and `MAX_INTEGRATION_LABEL`. */
export const MAX_INTEGRATIONS = 16;
export const MAX_INTEGRATION_ID = 31;
export const MAX_INTEGRATION_LABEL = 23;

/** The push API's ports (`daemon/push.rs`): loopback over http, and the LAN's over https. */
export const PUSH_PORT = 47822;
export const PUSH_LAN_PORT = 47823;

/** The secret that holds push integration `id`'s token (`token_secret_name` in push.rs). */
export const tokenSecretFor = (id: string): string => `push-${id}`;

export type Config = {
  version: number;
  /** `font` is the grid's default font id. Absent: Montserrat. */
  layout: { cols: number; rows: number; font?: string };
  /** The TOML array of tables is `[[page]]`. Absent: one page. With tables: 1 to 4, the first is home. */
  page?: PageConfig[];
  /** The TOML array of tables is `[[button]]`. */
  button: ButtonConfig[];
  /**
   * Never part of a layout. A layout edit must carry it along, since a save rewrites the file. The
   * same goes for `sleep`, `push` and `integration`.
   */
  status_bar?: StatusBarConfig;
  sleep?: SleepConfig;
  push?: PushConfig;
  /** The TOML array of tables is `[[integration]]`. */
  integration?: IntegrationConfig[];
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
export type ConfigView = ConfigStatus & {
  config: Config;
  loaded: boolean;
  status_bar_defaults: StatusBarDefaults;
};

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

/** A new button at `pos`, on `page` (a `[[page]]` id) when the config has page tables. */
export function newButton(
  config: Config,
  pos: [number, number],
  page?: string,
): ButtonConfig {
  return {
    id: freeId(config.button),
    pos,
    ...(page !== undefined && { page }),
    label: "New",
    bg: "#1e88e5",
    fg: "#ffffff",
    action: newShell(),
  };
}
