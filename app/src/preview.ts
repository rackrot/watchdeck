// How the device draws the button grid, so the editor can show the same thing. These numbers
// mirror two files and must change with them:
//   firmware/main/ui.c    GRID_TOP, GRID_GAP, ICON_PAD, ICON_GAP, ICON_MIN, TILE_BIG_MIN_H and
//                         build_grid (cell sizes, label and tile layout, tile colors)
//   firmware/main/display.h  the 1024x600 screen
// `iconBox` is `box_px` in app/src-tauri/src/daemon/icons.rs, which sizes the icons the daemon
// sends. Lines break as in `lvtext.ts`.
//
// Text sits in two slots, named by their Montserrat size, with line boxes of 27 and 35 px (ui.c
// SLOT24_H and SLOT32_H). Every fixed height below is a slot's box, whatever the font's own
// line height is, so a font change never moves an icon box.

import { DEFAULT_FONT, fontInfo, type Slot, slotFont } from "./fonts.ts";
import { dotLines, wrapLines } from "./lvtext.ts";

export const SCREEN_W = 1024;
export const SCREEN_H = 600;
export const GRID_TOP = 204;
export const GRID_GAP = 8;
export const GRID_H = SCREEN_H - GRID_TOP;
const ICON_PAD = 4;
const ICON_GAP = 2;
const ICON_MIN = 24;
const ICON_MAX = 256;
const TILE_BIG_MIN_H = 80;
/** The line box of each text slot. */
const SLOT_H: Record<Slot, number> = { 24: 27, 32: 35 };
/** The label is `cell_w - 16` wide, centered in a cell padded by 4 on each side. */
const LABEL_INSET = 16;

export const MAX_COLS = 8;
export const MAX_ROWS = 5;

export type Cell = { w: number; h: number };
export type Rect = Cell & { x: number; y: number };

export function cellSize(cols: number, rows: number): Cell {
  return {
    w: Math.trunc((SCREEN_W - (cols + 1) * GRID_GAP) / cols),
    h: Math.trunc((GRID_H - rows * GRID_GAP) / rows),
  };
}

/** The cell at (`col`, `row`), in grid pixels: the origin is the grid's top left corner. */
export function cellRect(col: number, row: number, cell: Cell): Rect {
  return {
    ...cell,
    x: GRID_GAP + col * (cell.w + GRID_GAP),
    y: row * (cell.h + GRID_GAP),
  };
}

/** The cell under a point in grid pixels, or null in a gap or outside the grid. */
export function cellAt(
  x: number,
  y: number,
  cols: number,
  rows: number,
): [number, number] | null {
  const cell = cellSize(cols, rows);
  const col = Math.floor((x - GRID_GAP) / (cell.w + GRID_GAP));
  const row = Math.floor(y / (cell.h + GRID_GAP));
  if (col < 0 || col >= cols || row < 0 || row >= rows) {
    return null;
  }
  const rect = cellRect(col, row, cell);
  const inside =
    x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
  return inside ? [col, row] : null;
}

/** How many lines of label an icon button has room for: 2 unless that leaves the icon under 24 px. */
export function labelLines(cell: Cell): 1 | 2 {
  const room = cell.h - 2 * ICON_PAD;
  const line = SLOT_H[32];
  return room >= 2 * line + ICON_GAP + ICON_MIN ? 2 : 1;
}

/** The side of the square an icon fits in, as `box_px` in icons.rs. */
export function iconBox(cols: number, rows: number): number {
  const cell = cellSize(cols, rows);
  const room = cell.h - 2 * ICON_PAD;
  const label = labelLines(cell) * SLOT_H[32];
  return Math.min(cell.w - 2 * ICON_PAD, room - label - ICON_GAP, ICON_MAX);
}

/**
 * A tile button wears its configured color only while ok. Before its first reading it is grey and
 * when bad it is red (ui.c `tile_bg`: lv_palette_darken of GREY and RED, level 2), which the CSS
 * has as --dev-tile-unknown and --dev-tile-bad.
 */
export type TileState = "unknown" | "ok" | "bad";

export type TextBox = {
  lines: string[];
  /** The slot the text is in, named by its Montserrat size. */
  size: Slot;
  /** CSS `font-family`. */
  family: string;
  /** The em size to draw the font at: the size the firmware font was converted at. */
  px: number;
  /** The font's own line height, which spaces the lines. */
  lineHeight: number;
  /** The box's height: the slot's line box times the lines for a label with an icon or a tile, else as tall as its lines. */
  height: number;
};

/** The fonts of a button's label and tile value, as `resolveFonts` gives them. */
export type ButtonFonts = { label: string; value: string };

export type ButtonLayout = {
  /** Null when there is no room for the label: a tile with an icon on a grid of 4 or 5 rows. */
  label: TextBox | null;
  /** A tile's value, under the label. */
  value: TextBox | null;
  /** The icon's box in pixels, or null for a button without an icon. */
  icon: number | null;
};

/** Text in a box of `lines` slot line boxes, cut with dots as LVGL cuts it. */
function fixed(
  text: string,
  fontId: string,
  size: Slot,
  width: number,
  lines: number,
): TextBox {
  const height = lines * SLOT_H[size];
  const font = slotFont(fontId, size);
  return {
    lines: dotLines(text, font, width, height),
    size,
    family: fontInfo(fontId).family,
    px: font.px,
    lineHeight: font.lineHeight,
    height,
  };
}

/**
 * What `build_grid` puts in a button: a label (wrapping, or fixed at two lines with an icon, one
 * line on a tile) and for a tile a value under it. `tileText` is the tile's current text, empty
 * before its first reading. `fonts` are the resolved font ids, Montserrat when not given.
 */
export function layoutButton(
  button: { label: string; hasIcon: boolean; isTile: boolean },
  cols: number,
  rows: number,
  tileText = "",
  fonts: ButtonFonts = { label: DEFAULT_FONT, value: DEFAULT_FONT },
): ButtonLayout {
  const cell = cellSize(cols, rows);
  const width = cell.w - LABEL_INSET;
  const lines = labelLines(cell);
  const icon = button.hasIcon ? iconBox(cols, rows) : null;
  if (!button.isTile) {
    if (button.hasIcon) {
      return {
        label: fixed(button.label, fonts.label, 32, width, lines),
        value: null,
        icon,
      };
    }
    // A wrapping label grows to its text, so it keeps the font's own line height.
    const font = slotFont(fonts.label, 32);
    const wrapped = wrapLines(button.label, font, width);
    const label: TextBox = {
      lines: wrapped,
      size: 32,
      family: fontInfo(fonts.label).family,
      px: font.px,
      lineHeight: font.lineHeight,
      // An empty label is still one line tall.
      height: Math.max(1, wrapped.length) * font.lineHeight,
    };
    return { label, value: null, icon };
  }
  const label =
    button.hasIcon && lines === 1
      ? null
      : fixed(button.label, fonts.label, 24, width, 1);
  const size = cell.h >= TILE_BIG_MIN_H ? 32 : 24;
  const value = fixed(
    tileText.replace(/\n/g, " "),
    fonts.value,
    size,
    width,
    1,
  );
  return { label, value, icon };
}
