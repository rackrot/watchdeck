// Pure helpers behind the editor: turning form rows into config values, moving a button, and
// telling a change the device draws from one it doesn't.

import type {
  ButtonConfig,
  Config,
  Metric,
  PageConfig,
  StatusBarConfig,
  Step,
} from "./config";
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

/** The most pages a config can have (`MAX_PAGES` in config.rs). */
export const MAX_PAGES = 4;

/** How many pages the config has: 1 without `[[page]]` tables. */
export const pageCount = (config: Config): number =>
  Math.max(1, config.page?.length ?? 0);

/** What a page's tab shows: its label, else its id. */
export const pageName = (page: PageConfig): string => page.label ?? page.id;

/** The page a button is on. One without `page` is on the first page, and so is a stray name. */
export function pageIndexOf(config: Config, button: ButtonConfig): number {
  const i = config.page?.findIndex((p) => p.id === button.page) ?? -1;
  return Math.max(0, i);
}

/** The button in cell `pos` of page `page`, other than number `except`, or undefined. */
export function buttonAt(
  config: Config,
  page: number,
  pos: [number, number],
  except?: number,
): ButtonConfig | undefined {
  return config.button.find(
    (b, i) =>
      i !== except &&
      pageIndexOf(config, b) === page &&
      b.pos[0] === pos[0] &&
      b.pos[1] === pos[1],
  );
}

/**
 * Moves button number `index` to `to`. If another button of its page is there the two swap, so a
 * move never makes an overlap.
 */
export function moveButton(
  config: Config,
  index: number,
  to: [number, number],
): Config {
  const mover = config.button[index];
  if (!mover) {
    return config;
  }
  const from = mover.pos;
  const page = pageIndexOf(config, mover);
  return {
    ...config,
    button: config.button.map((b, i) => {
      if (i === index) {
        return { ...b, pos: to };
      }
      const there =
        pageIndexOf(config, b) === page &&
        b.pos[0] === to[0] &&
        b.pos[1] === to[1];
      return there ? { ...b, pos: from } : b;
    }),
  };
}

/** `[[page]]` ids are 1 to 63 characters; a new page is `page-2` or the next free number. */
function freePageId(pages: PageConfig[]): string {
  const ids = new Set(pages.map((p) => p.id));
  let n = pages.length + 1;
  while (ids.has(`page-${n}`)) {
    n++;
  }
  return `page-${n}`;
}

/**
 * Adds a page at the end. The first one turns the implicit page into `[[page]] id = "main"`, and
 * the buttons without a `page` stay on it. A fifth page is refused: the config comes back as it was.
 */
export function addPage(config: Config): Config {
  const pages = config.page ?? [];
  if (pageCount(config) >= MAX_PAGES) {
    return config;
  }
  const base = pages.length === 0 ? [{ id: "main" }] : pages;
  return { ...config, page: [...base, { id: freePageId(base) }] };
}

/** Sets page `index`'s label. An empty label is removed, since the daemon refuses one. */
export function renamePage(
  config: Config,
  index: number,
  label: string,
): Config {
  return {
    ...config,
    page: config.page?.map((p, i) => {
      if (i !== index) {
        return p;
      }
      const { label: _, ...rest } = p;
      return label === "" ? rest : { ...rest, label };
    }),
  };
}

/**
 * Swaps page `index` with its neighbour `by` places away (-1 or 1). Buttons without a `page` are on
 * the first page, so they are given its id first: a move of the first page must not carry them along.
 */
export function movePage(config: Config, index: number, by: -1 | 1): Config {
  const pages = config.page;
  const to = index + by;
  if (!pages?.[index] || !pages[to]) {
    return config;
  }
  const home = pages[0].id;
  const next = [...pages];
  [next[index], next[to]] = [next[to], next[index]];
  return {
    ...config,
    page: next,
    button: config.button.map((b) =>
      b.page === undefined ? { ...b, page: home } : b,
    ),
  };
}

/** The buttons on page `index`. */
export const buttonsOnPage = (config: Config, index: number): ButtonConfig[] =>
  config.button.filter((b) => pageIndexOf(config, b) === index);

/**
 * Deletes page `index` and its buttons. Down to one page, the `[[page]]` tables and every `page`
 * key go too, so the file is in the form it had before it had pages.
 */
export function removePage(config: Config, index: number): Config {
  const pages = config.page?.filter((_, i) => i !== index);
  const button = config.button.filter((b) => pageIndexOf(config, b) !== index);
  if (!pages || pages.length < 2) {
    const { page: _, ...rest } = config;
    return { ...rest, button: button.map(({ page: _p, ...b }) => b) };
  }
  return { ...config, page: pages, button };
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

/** What a device names in `hello.caps` that changes the layout it is sent. */
type Caps = { pages: boolean; fonts: boolean };

/**
 * What the daemon sends a session, as a key (`Layout` and its views in config.rs): each page's id
 * and buttons, in order. A view of one page has no page id, so a single `[[page]]` table, or a
 * rename of the first page, changes nothing in it. A page's label never counts. `paged` is in the
 * key because icons are drawn for a shorter cell with 2 or more pages, so their ids change.
 */
function viewKey(config: Config, caps: Caps): string {
  const c = caps.fonts ? config : withoutFonts(config);
  const count = pageCount(c);
  // Without "pages" a device is sent the first page only.
  const sent = caps.pages ? count : 1;
  const pages = Array.from({ length: sent }, (_, i) => [
    sent > 1 ? (c.page?.[i]?.id ?? "") : "",
    buttonsOnPage(c, i).map((button) => drawn(button, c.layout)),
  ]);
  const sentIcons = Array.from({ length: sent }, (_, i) => i).some((i) =>
    buttonsOnPage(c, i).some((b) => b.icon !== undefined),
  );
  return JSON.stringify([
    c.layout.cols,
    c.layout.rows,
    // The message carries the grid's font even when no button shows it.
    c.layout.font ?? DEFAULT_FONT,
    // The message has no `paged` field: it only sizes icons, so it matters to a view that names
    // one. Residual: a raster already smaller than its box keeps its id, so the daemon may still
    // push nothing; the config can't tell.
    count >= 2 && sentIcons,
    pages,
  ]);
}

const FULL: Caps = { pages: true, fonts: true };

/** Whether going from `a` to `b` changes the layout a device is sent. An action edit doesn't. */
export const layoutChanged = (a: Config, b: Config): boolean =>
  viewKey(a, FULL) !== viewKey(b, FULL);

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
 * The devices a save pushes a layout to: those whose view of the layout changes, as the daemon
 * compares them. A change to fonts alone reaches only the "fonts" devices, and one to a page after
 * the first only the "pages" devices. The daemon sends the others nothing, so waiting for them
 * would fail.
 */
export function layoutTargets(
  before: Config,
  after: Config,
  devices: { id: string; connected: boolean; caps: string[] }[],
): string[] {
  return devices
    .filter((d) => {
      const caps = {
        pages: d.caps.includes("pages"),
        fonts: d.caps.includes("fonts"),
      };
      return d.connected && viewKey(before, caps) !== viewKey(after, caps);
    })
    .map((d) => d.id);
}

/** Whether going from `a` to `b` changes `[status_bar]`, which a device is never sent a layout for. */
export const barChanged = (a: Config, b: Config): boolean =>
  canon(a.status_bar ?? null) !== canon(b.status_bar ?? null);

/**
 * `bar` without parts that say nothing: a unit that is the default, a `thresholds` table with no
 * keys, and a bar with no keys.
 */
export function pruneBar(bar: StatusBarConfig): StatusBarConfig | undefined {
  const { thresholds, temp_unit, net_unit, ...base } = bar;
  const rest = {
    ...base,
    ...(temp_unit !== undefined && temp_unit !== "c" && { temp_unit }),
    ...(net_unit !== undefined && net_unit !== "si" && { net_unit }),
  };
  const next =
    thresholds && Object.keys(thresholds).length > 0
      ? { ...rest, thresholds }
      : rest;
  return Object.keys(next).length > 0 ? next : undefined;
}

/**
 * `bar` with `metric`'s steps set to `steps`. `undefined` removes the key, so the metric takes the
 * defaults for the unit, where `[]` would mean never colored.
 */
export function withSteps(
  bar: StatusBarConfig | undefined,
  metric: Metric,
  steps: Step[] | undefined,
): StatusBarConfig | undefined {
  const { [metric]: _, ...others } = bar?.thresholds ?? {};
  return pruneBar({
    ...bar,
    thresholds: steps === undefined ? others : { ...others, [metric]: steps },
  });
}

/** `bar` with `gpu` set. `auto` is the default, so picking it removes the key. */
export function withGpu(
  bar: StatusBarConfig | undefined,
  gpu: string,
): StatusBarConfig | undefined {
  const { gpu: _, ...rest } = bar ?? {};
  return pruneBar(gpu === "auto" ? rest : { ...rest, gpu });
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
