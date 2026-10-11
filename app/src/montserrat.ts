// Advance widths of the glyphs the preview needs from the LVGL Montserrat fonts that
// firmware/main/ui.c draws with (lv_font_montserrat_24 and _32), so the preview breaks lines where
// the device does. Taken from `.adv_w` in
// firmware/managed_components/lvgl__lvgl/src/font/lv_font_montserrat_{24,32}.c, in 1/16 px, for
// ASCII 0x20 to 0x7e. `lineHeight` is the font's `.line_height`. LVGL's pair kerning is left out:
// on typical labels it moves a line by about 1 px (3 px at the 95th percentile).

export type Font = {
  lineHeight: number;
  /** Advance of ASCII `0x20 + i`, in 1/16 px. */
  adv: readonly number[];
};

export const MONTSERRAT_24: Font = {
  lineHeight: 27,
  adv: [
    103, 103, 150, 270, 238, 324, 263, 81, 129, 130, 154, 223, 87, 147, 87, 135,
    256, 142, 220, 220, 257, 220, 237, 230, 247, 237, 87, 87, 223, 223, 223,
    220, 397, 281, 291, 278, 317, 257, 244, 296, 312, 119, 197, 276, 228, 367,
    312, 323, 277, 323, 279, 238, 225, 304, 273, 432, 258, 248, 252, 128, 135,
    128, 224, 192, 230, 230, 262, 219, 262, 235, 136, 265, 262, 107, 109, 237,
    107, 406, 262, 244, 262, 262, 157, 192, 159, 260, 215, 345, 212, 215, 200,
    135, 115, 135, 223,
  ],
};

export const MONTSERRAT_32: Font = {
  lineHeight: 35,
  adv: [
    138, 137, 200, 360, 318, 432, 351, 108, 173, 173, 205, 298, 116, 196, 116,
    180, 342, 189, 294, 293, 343, 294, 316, 306, 330, 316, 116, 116, 298, 298,
    298, 293, 529, 375, 388, 370, 423, 343, 325, 395, 416, 159, 263, 368, 304,
    489, 416, 430, 370, 430, 372, 318, 301, 405, 365, 577, 345, 331, 336, 170,
    180, 170, 298, 256, 307, 306, 349, 292, 349, 313, 181, 353, 349, 143, 145,
    315, 143, 541, 349, 325, 349, 349, 210, 257, 212, 347, 286, 460, 283, 286,
    267, 180, 153, 180, 298,
  ],
};
