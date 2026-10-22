// Pure helpers behind the editor: turning form rows into config values, moving a button, and
// telling a change the device draws from one it doesn't.

import type { ButtonConfig, Config } from "./config";
import { DEFAULT_FONT, resolveFonts } from "./fonts.ts";

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
function drawn(b: ButtonConfig, layout: Config["layout"]) {
  const fonts = resolveFonts(layout, b);
  return [
    b.id,
    b.pos,
    b.label,
    b.bg,
    b.fg,
    b.icon ?? null,
    b.tile !== undefined,
    fonts.label,
    b.tile ? fonts.value : null,
  ];
}

/** Whether going from `a` to `b` changes the layout a device is sent. An action edit doesn't. */
export function layoutChanged(a: Config, b: Config): boolean {
  const key = (c: Config) =>
    JSON.stringify([
      c.layout.cols,
      c.layout.rows,
      // The message carries the grid's font even when no button shows it.
      c.layout.font ?? DEFAULT_FONT,
      c.button.map((button) => drawn(button, c.layout)),
    ]);
  return key(a) !== key(b);
}

/** `config` with every font key removed, to tell a font-only change from the rest. */
function withoutFonts(config: Config): Config {
  const { font: _, ...layout } = config.layout;
  return {
    ...config,
    layout,
    button: config.button.map(({ font: _b, tile, ...b }) => {
      if (!tile) {
        return b;
      }
      const { font: _t, ...rest } = tile;
      return { ...b, tile: rest };
    }),
  };
}

/**
 * The devices a save pushes a layout to. A change to fonts alone reaches only those that have the
 * "fonts" capability: the daemon sends the others nothing, so waiting for them would fail.
 */
export function layoutTargets(
  before: Config,
  after: Config,
  devices: { id: string; connected: boolean; caps: string[] }[],
): string[] {
  if (!layoutChanged(before, after)) {
    return [];
  }
  const fontOnly = !layoutChanged(withoutFonts(before), withoutFonts(after));
  return devices
    .filter((d) => d.connected && (!fontOnly || d.caps.includes("fonts")))
    .map((d) => d.id);
}

/**
 * `config` with the grid's font set to `font`. Montserrat is the default, so picking it removes the
 * key. The rest of the layout stays.
 */
export function withGridFont(config: Config, font: string): Config {
  const { font: _, ...rest } = config.layout;
  return {
    ...config,
    layout: font === DEFAULT_FONT ? rest : { ...rest, font },
  };
}
