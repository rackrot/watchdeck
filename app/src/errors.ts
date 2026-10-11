// Puts the rule violations that `save_config` returns next to the fields they are about. The
// messages are the strings `Config::check` builds in app/src-tauri/src/daemon/config.rs, so this
// file reads their wording. A message it doesn't know still shows, on its button or at the top.

import type { Config } from "./config";

export type ActionField =
  | "timeout_s"
  | "argv"
  | "method"
  | "url"
  | "body"
  | "headers"
  | "secret_headers"
  | "insecure_http";

export type Field =
  | "id"
  | "pos"
  | "label"
  | "bg"
  | "fg"
  | "icon"
  | "tile.every_s"
  | "tile.json_pointer"
  | `action.${ActionField}`
  | `tile.source.${ActionField}`
  /** A problem with the button that no field owns. */
  | "button";

export type FieldErrors = Partial<Record<Field, string[]>>;

export type ErrorMap = {
  /** Messages for the layout section: the grid size and the version. */
  grid: string[];
  /** Messages about the file or the save itself, not about a button. */
  general: string[];
  /** By button id. Every button with that id gets the message. */
  buttons: Map<string, FieldErrors>;
};

export const emptyErrors = (): ErrorMap => ({
  grid: [],
  general: [],
  buttons: new Map(),
});

export const hasErrors = (map: ErrorMap): boolean =>
  map.grid.length > 0 || map.general.length > 0 || map.buttons.size > 0;

/** Which field of an action `problem` is about, from the start of Rust's message. */
function actionField(problem: string): ActionField | null {
  const rules: [RegExp, ActionField][] = [
    [/^timeout_s /, "timeout_s"],
    [/^argv/, "argv"],
    [/^method /, "method"],
    [/^url /, "url"],
    [/^secret_headers would cross/, "insecure_http"],
    [/^body /, "body"],
    [/^header .* uses secret /, "secret_headers"],
    [/^header /, "headers"],
  ];
  return rules.find(([pattern]) => pattern.test(problem))?.[1] ?? null;
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
 * about a button the draft doesn't have goes to `general`, whole, so it still shows.
 */
export function mapErrors(messages: string[], ids?: string[]): ErrorMap {
  const map = emptyErrors();
  for (const message of messages) {
    const button = message.match(/^button ("(?:[^"\\]|\\.)*"): (.*)$/s);
    if (button) {
      const id = unquote(button[1]);
      if (ids && !ids.includes(id)) {
        map.general.push(message);
        continue;
      }
      for (const field of fieldsOf(button[2])) {
        addButtonError(map, id, field, button[2]);
      }
    } else if (/^(layout\.(cols|rows)|version) /.test(message)) {
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
  return map;
}
