// Line breaking and "..." for labels, ported from LVGL 9's lv_text.c (lv_text_get_next_word and
// lv_text_get_next_line) and lv_label.c (the DOTS long mode, centered), with the firmware's
// settings: break characters " ,.;:-_)]}" (CONFIG_LV_TXT_BREAK_CHARS), no long-word splitting
// (CONFIG_LV_TXT_LINE_BREAK_LONG_LEN=0), no letter or line spacing. Labels are printable ASCII,
// so there are no newlines or multibyte letters to handle. Widths come from `montserrat.ts`
// without kerning.

import type { Font } from "./montserrat";

const BREAK_CHARS = " ,.;:-_)]}";

function glyphWidth(font: Font, ch: string): number {
  const code = ch.charCodeAt(0);
  const index = code >= 0x20 && code <= 0x7e ? code - 0x20 : 0x3f - 0x20;
  return (font.adv[index] + 8) >> 4;
}

export function textWidth(font: Font, text: string): number {
  let width = 0;
  for (const ch of text) {
    width += glyphWidth(font, ch);
  }
  return width;
}

/** One word from `start`: how far to advance, and its width. 0 means the word doesn't fit. */
function nextWord(
  text: string,
  start: number,
  font: Font,
  maxWidth: number,
  breakAll: boolean,
): { advance: number; width: number } {
  let i = start;
  let current = 0;
  let width = 0;
  let wordLen = 0;
  let breakAt = -1;
  while (i < text.length) {
    const ch = text[i];
    wordLen++;
    current += glyphWidth(font, ch);
    if (ch !== " " && breakAt < 0 && current > maxWidth) {
      breakAt = i - start;
      if (breakAll) {
        break;
      }
    }
    if (BREAK_CHARS.includes(ch)) {
      if (i === start && breakAt < 0) {
        width = current;
      }
      wordLen--;
      break;
    }
    if (breakAt < 0) {
      width = current;
    }
    i++;
  }
  if (breakAt < 0) {
    // A break character that starts the word is a word of its own.
    return { advance: wordLen === 0 ? i - start + 1 : i - start, width };
  }
  return breakAll ? { advance: breakAt, width } : { advance: 0, width: 0 };
}

/** Bytes in the line that starts at `start`, including the spaces that wrapping swallows. */
function nextLine(
  text: string,
  start: number,
  font: Font,
  maxWidth: number,
  breakAll: boolean,
): number {
  let i = 0;
  let room = maxWidth;
  let hanging = 0;
  while (start + i < text.length && room > hanging) {
    const word = nextWord(
      text,
      start + i,
      font,
      room - hanging,
      breakAll || i === 0,
    );
    if (word.advance === 0) {
      break;
    }
    // A space is charged to the line only if a word follows it there.
    if (/^ +$/.test(text.slice(start + i, start + i + word.advance))) {
      hanging += word.width;
    } else {
      room -= word.width + hanging;
      hanging = 0;
    }
    i += word.advance;
  }
  if (i === 0) {
    i = 1;
  }
  while (text[start + i] === " ") {
    i++;
  }
  return i;
}

/**
 * Where each line starts and ends. In DOTS mode (`dotsLines` set) the last visible line, and any
 * after it, may break inside a word.
 */
function lineSpans(
  text: string,
  font: Font,
  width: number,
  dotsLines?: number,
): [number, number][] {
  const spans: [number, number][] = [];
  const { lineHeight } = font;
  let breakAll = false;
  let start = 0;
  while (start < text.length) {
    if (
      dotsLines !== undefined &&
      spans.length * lineHeight + 2 * lineHeight > dotsLines * lineHeight
    ) {
      breakAll = true;
    }
    const end = start + nextLine(text, start, font, width, breakAll);
    spans.push([start, end]);
    start = end;
  }
  return spans;
}

const shown = (text: string, [start, end]: [number, number]) =>
  text.slice(start, end).replace(/ +$/, "");

/** The lines of a label that wraps and grows to fit (`LV_LABEL_LONG_WRAP`). */
export function wrapLines(text: string, font: Font, width: number): string[] {
  return lineSpans(text, font, width).map((span) => shown(text, span));
}

/**
 * The lines of a label of fixed height, `maxLines` lines tall (`LV_LABEL_LONG_DOT`). Text that
 * needs more lines ends in "...", which replaces the three letters at the right edge of the
 * last visible line.
 */
export function dotLines(
  text: string,
  font: Font,
  width: number,
  maxLines: number,
): string[] {
  const needed = lineSpans(text, font, width).length;
  let label = text;
  if (needed > maxLines && needed > 1 && text.length > 3) {
    const spans = lineSpans(text, font, width, maxLines);
    const [start, end] = spans[Math.min(maxLines, spans.length) - 1];
    // The letter under x = width - 3 dots, on the last visible line.
    const target = width - 3 * glyphWidth(font, ".");
    let x =
      Math.trunc(width / 2) -
      Math.trunc(textWidth(font, shown(text, [start, end])) / 2);
    let at = end - 1;
    for (let j = start; j < end; j++) {
      const w = glyphWidth(font, text[j]);
      if (target < x + w) {
        at = j;
        break;
      }
      x += w;
    }
    at = Math.min(at, text.length - 3);
    label = `${text.slice(0, at)}...`;
  }
  return lineSpans(label, font, width, maxLines)
    .slice(0, maxLines)
    .map((span) => shown(label, span));
}
