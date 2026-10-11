// Pure helpers behind the editor: turning form rows into config values, moving a button, and
// telling a change the device draws from one it doesn't.

import type { ButtonConfig, Config } from "./config";

export type Row = { name: string; value: string };

/** Rows to a header table. Blank rows are dropped. A name listed twice keeps its last value. */
export function rowsToRecord(rows: Row[]): Record<string, string> | undefined {
  const entries = rows.filter((r) => r.name !== "" || r.value !== "");
  return entries.length > 0
    ? Object.fromEntries(entries.map((r) => [r.name, r.value]))
    : undefined;
}

export function recordToRows(
  record: Record<string, string> | undefined,
): Row[] {
  return Object.entries(record ?? {}).map(([name, value]) => ({ name, value }));
}

/** Names that more than one row uses, which a table can't hold. */
export function duplicateNames(rows: Row[]): string[] {
  const seen = new Set<string>();
  const dupes = new Set<string>();
  for (const { name } of rows) {
    if (seen.has(name)) {
      dupes.add(name);
    }
    seen.add(name);
  }
  return [...dupes];
}

/** A whole number from what was typed, or NaN, which `numberErrors` reports. */
export function parseWhole(text: string): number {
  return /^\d+$/.test(text.trim()) ? Number(text) : Number.NaN;
}

/**
 * Moves button number `index` to `to`. If another button is there the two swap, so a move never
 * makes an overlap.
 */
export function moveButton(
  config: Config,
  index: number,
  to: [number, number],
): Config {
  const from = config.button[index]?.pos;
  if (!from) {
    return config;
  }
  return {
    ...config,
    button: config.button.map((b, i) => {
      if (i === index) {
        return { ...b, pos: to };
      }
      return b.pos[0] === to[0] && b.pos[1] === to[1] ? { ...b, pos: from } : b;
    }),
  };
}

/** JSON with sorted keys, so two values compare equal whatever order their keys were made in. */
export function canon(value: unknown): string {
  return JSON.stringify(value, (_, v) =>
    v && typeof v === "object" && !Array.isArray(v)
      ? Object.fromEntries(
          Object.entries(v).sort(([a], [b]) => (a < b ? -1 : 1)),
        )
      : v,
  );
}

/** What a device draws for a button, which is all of it except the action (`Live::layout`). */
function drawn(b: ButtonConfig) {
  return [
    b.id,
    b.pos,
    b.label,
    b.bg,
    b.fg,
    b.icon ?? null,
    b.tile !== undefined,
  ];
}

/** Whether going from `a` to `b` changes the layout a device is sent. An action edit doesn't. */
export function layoutChanged(a: Config, b: Config): boolean {
  const key = (c: Config) =>
    JSON.stringify([c.layout.cols, c.layout.rows, c.button.map(drawn)]);
  return key(a) !== key(b);
}
