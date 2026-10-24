// Puts the rule violations that `save_config` returns next to the fields they are about. The
// messages are the strings `Config::check` builds in app/src-tauri/src/daemon/config.rs, so this
// file reads their wording. A message it doesn't know still shows, on its button or at the top.

import type { Config, Metric } from "./config";

export type ActionField =
  | "timeout_s"
  | "argv"
  | "method"
  | "url"
  | "body"
  | "headers"
  | "secret_headers"
  | "insecure_http"
  | "insecure_tls";

export type Field =
  | "id"
  | "pos"
  | "label"
  | "bg"
  | "fg"
  | "icon"
  | "font"
  | "page"
  | "tile.font"
  | "tile.every_s"
  | "tile.json_pointer"
  | `action.${ActionField}`
  | `tile.source.${ActionField}`
  /** A problem with the button that no field owns. */
  | "button";

export type FieldErrors = Partial<Record<Field, string[]>>;

/** A field of an integration's form. A stat source's action fields are `source.<field>`. */
export type IntegrationField =
  | "id"
  | "label"
  | "token"
  | "every_s"
  | "json_pointer"
  | `source.${ActionField}`
  /** A problem with the integration that no field owns. */
  | "integration";

export type IntegrationErrors = Partial<Record<IntegrationField, string[]>>;

/** A field of the status bar form: a setting, or one metric's steps. */
export type StatusBarField =
  | "gpu"
  | "temp_unit"
  | "net_unit"
  | `thresholds.${Metric}`
  /** A `status_bar.*` problem that no field owns. */
  | "bar";

export type StatusBarErrors = Partial<Record<StatusBarField, string[]>>;

/** A field of the sleep screen form. */
export type SleepField =
  | "image"
  | "fit"
  | "bg"
  | "fg"
  | "show"
  | "text"
  | "anchor"
  | "spinner"
  | "spinner_color"
  | "offline_after_s"
  | "backlight"
  /** A `sleep.*` problem that no field owns. */
  | "sleep";

export type SleepErrors = Partial<Record<SleepField, string[]>>;

export type ErrorMap = {
  /** Messages for the layout section: the grid size, its font, the version and the page and tile counts. */
  grid: string[];
  /** Messages about the file or the save itself, not about a button. */
  general: string[];
  /** By button id. Every button with that id gets the message. */
  buttons: Map<string, FieldErrors>;
  /** By page id, for the page tabs. */
  pages: Map<string, string[]>;
  /** Messages about `[status_bar]`, by the field of the status bar form. */
  statusBar: StatusBarErrors;
  /** Messages about `[sleep]`, by the field of the sleep screen form. */
  sleep: SleepErrors;
  /** By integration id. Every message is about one integration. */
  integrations: Map<string, string[]>;
};

export const emptyErrors = (): ErrorMap => ({
  grid: [],
  general: [],
  buttons: new Map(),
  pages: new Map(),
  statusBar: {},
  sleep: {},
  integrations: new Map(),
});

export const hasErrors = (map: ErrorMap): boolean =>
  map.grid.length > 0 ||
  map.general.length > 0 ||
  map.buttons.size > 0 ||
  map.pages.size > 0 ||
  Object.keys(map.statusBar).length > 0 ||
  Object.keys(map.sleep).length > 0 ||
  map.integrations.size > 0;

function addStatusBarError(
  map: ErrorMap,
  field: StatusBarField,
  message: string,
) {
  map.statusBar[field] = [...(map.statusBar[field] ?? []), message];
}

/**
 * Which field of the status bar form a `status_bar.*` message is about. A threshold message loses
 * its prefix, since it shows beside the row it names.
 */
function statusBarField(message: string): [StatusBarField, string] {
  const setting = message.match(/^status_bar\.(gpu|temp_unit|net_unit) /);
  if (setting) {
    return [setting[1] as StatusBarField, message];
  }
  const metric = message.match(
    /^status_bar\.thresholds\.(cpu|mem|gpu|cpu_temp|gpu_temp|net_rx|net_tx): (.*)$/s,
  );
  if (metric) {
    return [`thresholds.${metric[1] as Metric}`, metric[2]];
  }
  return ["bar", message];
}

function addSleepError(map: ErrorMap, field: SleepField, message: string) {
  map.sleep[field] = [...(map.sleep[field] ?? []), message];
}

const SLEEP_FIELDS: SleepField[] = [
  "image",
  "fit",
  "bg",
  "fg",
  "show",
  "text",
  "anchor",
  "spinner",
  "spinner_color",
  "offline_after_s",
  "backlight",
];

/**
 * Which field of the sleep form a `sleep.*` message is about: the key after the prefix, ended by a
 * space or a colon (`sleep.show: "x" is not one of ...`, `sleep.text is needed ...`). The message
 * stays whole.
 */
function sleepField(message: string): SleepField {
  const key = message.match(/^sleep\.([a-z_]+)[ :]/)?.[1] as SleepField;
  return SLEEP_FIELDS.includes(key) ? key : "sleep";
}

/** Which field of an action `problem` is about, from the start of Rust's message. */
function actionField(problem: string): ActionField | null {
  const rules: [RegExp, ActionField][] = [
    [/^timeout_s /, "timeout_s"],
    [/^argv/, "argv"],
    [/^method /, "method"],
    [/^url /, "url"],
    [/^secret_headers would cross/, "insecure_http"],
    [/^insecure_tls /, "insecure_tls"],
    [/^body /, "body"],
    [/^header .* uses secret /, "secret_headers"],
    [/^header /, "headers"],
  ];
  return rules.find(([pattern]) => pattern.test(problem))?.[1] ?? null;
}

/**
 * An integration's messages, as the bucket holds them (the id's prefix already cut), sorted by the
 * field of its form. A stat source's messages start `source: `, then an action's wording. The
 * bare "is not a valid header name" shows under both header tables, as a button's does.
 */
export function integrationFields(messages: string[]): IntegrationErrors {
  const errors: IntegrationErrors = {};
  const add = (field: IntegrationField, message: string) => {
    errors[field] = [...(errors[field] ?? []), message];
  };
  for (const message of messages) {
    const source = message.match(/^source: (.*)$/s)?.[1];
    if (source !== undefined) {
      if (/^"[^"]*" is not a valid header name/.test(source)) {
        add("source.headers", message);
        add("source.secret_headers", message);
        continue;
      }
      const field = actionField(source);
      add(field ? `source.${field}` : "integration", message);
      continue;
    }
    const rules: [RegExp, IntegrationField][] = [
      [/^id /, "id"],
      [/^label/, "label"],
      [/^token/, "token"],
      [/^every_s /, "every_s"],
      [/^json_pointer /, "json_pointer"],
    ];
    add(
      rules.find(([pattern]) => pattern.test(message))?.[1] ?? "integration",
      message,
    );
  }
  return errors;
}

/** Which fields of a button `problem` is about, or "button". */
function fieldsOf(problem: string): Field[] {
  // The message doesn't say which table the name was in, so it shows under both.
  const bareName = /^"[^"]*" is not a valid header name/.test(problem);
  if (bareName) {
    return ["action.headers", "action.secret_headers"];
  }
  const source = problem.match(/^tile\.source: (.*)$/s);
  if (source && /^"[^"]*" is not a valid header name/.test(source[1])) {
    return ["tile.source.headers", "tile.source.secret_headers"];
  }
  return [fieldOf(problem)];
}

/** Which field of a button `problem` is about, or "button". */
function fieldOf(problem: string): Field {
  const simple: [RegExp, Field][] = [
    [/^id /, "id"],
    [/^another button has this id/, "id"],
    [/^pos /, "pos"],
    [/^label /, "label"],
    [/^icon/, "icon"],
    [/^font /, "font"],
    // `page "y" is not one of the [[page]] ids`, `page "y" is set, but the file has no [[page]] tables`.
    [/^page /, "page"],
    [/^tile\.font /, "tile.font"],
    [/^bg /, "bg"],
    [/^fg /, "fg"],
    [/^tile\.every_s/, "tile.every_s"],
    [/^tile\.json_pointer/, "tile.json_pointer"],
  ];
  const hit = simple.find(([pattern]) => pattern.test(problem));
  if (hit) {
    return hit[1];
  }
  const source = problem.match(/^tile\.source: (.*)$/s);
  if (source) {
    const field = actionField(source[1]);
    return field ? `tile.source.${field}` : "button";
  }
  const field = actionField(problem);
  return field ? `action.${field}` : "button";
}

const ESCAPES: Record<string, string> = {
  n: "\n",
  r: "\r",
  t: "\t",
  "0": "\0",
  "\\": "\\",
  '"': '"',
  "'": "'",
};

/**
 * Rust prints the id with `{:?}`: a double-quoted string with backslash escapes, `\u{1f600}`
 * among them. Falls back to the text between the quotes if an escape isn't one Rust writes.
 */
export function unquote(quoted: string): string {
  const inner = quoted.slice(1, -1);
  let out = "";
  for (let i = 0; i < inner.length; i++) {
    if (inner[i] !== "\\") {
      out += inner[i];
      continue;
    }
    const next = inner[i + 1];
    const unicode = inner.slice(i + 2).match(/^\{([0-9a-fA-F]{1,6})\}/);
    if (next === "u" && unicode) {
      const code = Number.parseInt(unicode[1], 16);
      if (code > 0x10ffff) {
        return inner;
      }
      out += String.fromCodePoint(code);
      i += 1 + unicode[0].length;
    } else if (next !== undefined && next in ESCAPES) {
      out += ESCAPES[next];
      i++;
    } else {
      return inner;
    }
  }
  return out;
}

export function addButtonError(
  map: ErrorMap,
  id: string,
  field: Field,
  message: string,
) {
  const errors = map.buttons.get(id) ?? {};
  errors[field] = [...(errors[field] ?? []), message];
  map.buttons.set(id, errors);
}

/**
 * Sorts messages by what they are about. With `ids`, the ids of the draft's buttons, a message
 * about a button the draft doesn't have goes to `general`, whole, so it still shows. `pageIds`
 * and `integrationIds` do the same for pages and integrations.
 */
export function mapErrors(
  messages: string[],
  ids?: string[],
  pageIds?: string[],
  integrationIds?: string[],
): ErrorMap {
  const map = emptyErrors();
  for (const message of messages) {
    const button = message.match(/^button ("(?:[^"\\]|\\.)*"): (.*)$/s);
    const page = message.match(/^page ("(?:[^"\\]|\\.)*"): (.*)$/s);
    const integration = message.match(
      /^integration ("(?:[^"\\]|\\.)*"): (.*)$/s,
    );
    if (page) {
      const id = unquote(page[1]);
      if (pageIds && !pageIds.includes(id)) {
        map.general.push(message);
        continue;
      }
      map.pages.set(id, [...(map.pages.get(id) ?? []), page[2]]);
    } else if (button) {
      const id = unquote(button[1]);
      if (ids && !ids.includes(id)) {
        map.general.push(message);
        continue;
      }
      for (const field of fieldsOf(button[2])) {
        addButtonError(map, id, field, button[2]);
      }
    } else if (message.startsWith("status_bar.")) {
      const [field, text] = statusBarField(message);
      addStatusBarError(map, field, text);
    } else if (message.startsWith("sleep.")) {
      addSleepError(map, sleepField(message), message);
    } else if (integration) {
      const id = unquote(integration[1]);
      if (integrationIds && !integrationIds.includes(id)) {
        map.general.push(message);
        continue;
      }
      map.integrations.set(id, [
        ...(map.integrations.get(id) ?? []),
        integration[2],
      ]);
    } else if (
      /^(layout\.(cols|rows|font)|version) |^\d+ pages, at most |^\d+ tiles across all pages, at most /.test(
        message,
      )
    ) {
      map.grid.push(message);
    } else {
      map.general.push(message);
    }
  }
  return map;
}

const isCount = (n: number) => Number.isSafeInteger(n) && n >= 0;

/**
 * Numbers that can't be sent at all, which Rust would refuse with a decoding error instead of a
 * rule. Ranges stay Rust's to judge.
 */
export function numberErrors(config: Config): ErrorMap {
  const map = emptyErrors();
  const whole = "must be a whole number";
  if (!isCount(config.layout.cols)) {
    map.grid.push(`layout.cols ${whole}`);
  }
  if (!isCount(config.layout.rows)) {
    map.grid.push(`layout.rows ${whole}`);
  }
  for (const b of config.button) {
    if (!b.pos.every(isCount)) {
      addButtonError(map, b.id, "pos", `pos ${whole}`);
    }
    if (!isCount(b.action.timeout_s)) {
      addButtonError(map, b.id, "action.timeout_s", `timeout_s ${whole}`);
    }
    if (b.tile && !isCount(b.tile.every_s)) {
      addButtonError(map, b.id, "tile.every_s", `tile.every_s ${whole}`);
    }
    if (b.tile && !isCount(b.tile.source.timeout_s)) {
      addButtonError(map, b.id, "tile.source.timeout_s", `timeout_s ${whole}`);
    }
  }
  for (const i of config.integration ?? []) {
    if (i.kind !== "stat") {
      continue;
    }
    const bad = [
      ...(isCount(i.every_s) ? [] : [`every_s ${whole}`]),
      ...(isCount(i.source.timeout_s) ? [] : [`source: timeout_s ${whole}`]),
    ];
    if (bad.length > 0) {
      map.integrations.set(i.id, [
        ...(map.integrations.get(i.id) ?? []),
        ...bad,
      ]);
    }
  }
  // Only the type is checked: a negative or huge `at` is a rule, which Rust words.
  const thresholds = config.status_bar?.thresholds ?? {};
  for (const [metric, steps] of Object.entries(thresholds)) {
    if (steps?.some((step) => !Number.isSafeInteger(step.at))) {
      addStatusBarError(map, `thresholds.${metric as Metric}`, `at ${whole}`);
    }
  }
  for (const key of ["offline_after_s", "backlight"] as const) {
    const value = config.sleep?.[key];
    if (value !== undefined && !Number.isSafeInteger(value)) {
      addSleepError(map, key, `sleep.${key} ${whole}`);
    }
  }
  return map;
}
