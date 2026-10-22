// The fonts a button can use: Montserrat (built into the firmware) and the six generated ones.
// `fonts.gen.ts` has the generated metrics, `montserrat.ts` Montserrat's; this joins them and holds
// the inheritance rule, as `Live::layout` in app/src-tauri/src/daemon/config.rs resolves it.

import type { ButtonConfig } from "./config.ts";
import { GENERATED_FONTS } from "./fonts.gen.ts";
import { type Font, MONTSERRAT_24, MONTSERRAT_32 } from "./montserrat.ts";

/** The font used when a config names none, and the fallback for a name this table lacks. */
export const DEFAULT_FONT = "montserrat";

/** The text sizes the grid draws in, named by their Montserrat size. */
export type Slot = 24 | 32;

/** A font's metrics for one slot. `px` is the em size to draw it at. */
export type SlotFont = Font & { px: number };

export type FontInfo = {
  id: string;
  /** What the selectors show. */
  name: string;
  /** For CSS `font-family`: the face `fonts.css` declares, then the UI font as a fallback. */
  family: string;
  slot24: SlotFont;
  slot32: SlotFont;
};

const FALLBACK = "var(--font-ui)";

export const FONTS: readonly FontInfo[] = [
  {
    id: DEFAULT_FONT,
    name: "Montserrat",
    family: FALLBACK,
    slot24: { ...MONTSERRAT_24, px: 24 },
    slot32: { ...MONTSERRAT_32, px: 32 },
  },
  ...GENERATED_FONTS.map(({ id, name, slot24, slot32 }) => ({
    id,
    name,
    family: `"${name}", ${FALLBACK}`,
    slot24,
    slot32,
  })),
];

/** The font with this id, or Montserrat for an id this table doesn't have. */
export function fontInfo(id: string | undefined): FontInfo {
  return FONTS.find((f) => f.id === id) ?? FONTS[0];
}

export function slotFont(id: string | undefined, slot: Slot): SlotFont {
  const font = fontInfo(id);
  return slot === 24 ? font.slot24 : font.slot32;
}

/**
 * The fonts a button's label and tile value are drawn in: the label's is `button.font`, else the
 * grid's, else Montserrat; the value's is `tile.font`, else the label's. A button without a tile
 * has no value, and `value` is then the label's font.
 */
export function resolveFonts(
  layout: { font?: string },
  button: Pick<ButtonConfig, "font" | "tile">,
): { label: string; value: string } {
  const label = button.font ?? layout.font ?? DEFAULT_FONT;
  return { label, value: button.tile?.font ?? label };
}
