// Run with `pnpm test`. Node strips the types, so these import with the .ts extension.
import assert from "node:assert/strict";
import { test } from "node:test";
import { dotLines, textWidth, wrapLines } from "../src/lvtext.ts";
import { MONTSERRAT_24, MONTSERRAT_32 } from "../src/montserrat.ts";
import {
  cellAt,
  cellRect,
  cellSize,
  iconBox,
  labelLines,
  layoutButton,
} from "../src/preview.ts";

test("cells match build_grid on the grids the firmware was checked with", () => {
  assert.deepEqual(cellSize(5, 3), { w: 195, h: 124 });
  assert.deepEqual(cellRect(0, 0, cellSize(5, 3)), {
    w: 195,
    h: 124,
    x: 8,
    y: 0,
  });
  assert.deepEqual(cellRect(4, 2, cellSize(5, 3)), {
    w: 195,
    h: 124,
    x: 820,
    y: 264,
  });
});

test("icon boxes are what icons.rs pins in `the_box_follows_the_firmware_button`", () => {
  // Cell width less padding, by column count; the rows decide the height.
  const widths = [1000, 492, 322, 238, 187, 153, 129, 111];
  const heights = [256, 110, 44, 46, 26];
  for (let cols = 1; cols <= 8; cols++) {
    heights.forEach((high, i) => {
      assert.equal(
        iconBox(cols, i + 1),
        Math.min(high, widths[cols - 1]),
        `${cols}x${i + 1}`,
      );
    });
  }
});

test("a button has two label lines until that would leave its icon under 24 px", () => {
  assert.deepEqual(
    [1, 2, 3, 4, 5].map((rows) => labelLines(cellSize(5, rows))),
    [2, 2, 2, 1, 1],
  );
});

test("a point maps to its cell, and a gap or the outside to none", () => {
  const rect = cellRect(2, 1, cellSize(5, 3));
  assert.deepEqual(cellAt(rect.x + 5, rect.y + 5, 5, 3), [2, 1]);
  assert.equal(
    cellAt(rect.x - 1, rect.y + 5, 5, 3),
    null,
    "the gap before the cell",
  );
  assert.equal(
    cellAt(rect.x + rect.w, rect.y + 5, 5, 3),
    null,
    "the gap after it",
  );
  assert.equal(cellAt(-5, 10, 5, 3), null);
  assert.equal(cellAt(500, 400, 5, 3), null, "below the last row");
});

test("glyph widths are LVGL's advances rounded to whole pixels", () => {
  assert.equal(textWidth(MONTSERRAT_32, " "), 9);
  assert.equal(textWidth(MONTSERRAT_32, "Plex"), 70);
  assert.equal(textWidth(MONTSERRAT_32, "Restart Plex"), 196);
  assert.equal(textWidth(MONTSERRAT_24, "Plex"), 52);
});

test("a label wraps at spaces and drops the space it wrapped at", () => {
  assert.deepEqual(wrapLines("Plex", MONTSERRAT_32, 179), ["Plex"]);
  assert.deepEqual(wrapLines("Restart Plex", MONTSERRAT_32, 179), [
    "Restart",
    "Plex",
  ]);
  assert.deepEqual(wrapLines("", MONTSERRAT_32, 179), []);
});

test("a word wider than the label breaks inside itself", () => {
  const word = "Supercalifragilisticexpialidocious";
  const lines = wrapLines(word, MONTSERRAT_32, 179);
  assert.ok(lines.length > 1);
  assert.equal(lines.join(""), word);
  for (const line of lines) {
    assert.ok(textWidth(MONTSERRAT_32, line) <= 179, line);
  }
});

test("a line can break after a hyphen, a dot or an underscore", () => {
  for (const text of [
    "front-door-camera-live",
    "nas.local.example.org",
    "backup_to_cloud_now",
  ]) {
    const lines = wrapLines(text, MONTSERRAT_32, 179);
    assert.ok(lines.length > 1, text);
    assert.equal(lines.join(""), text);
    for (const line of lines) {
      assert.ok(textWidth(MONTSERRAT_32, line) <= 179, line);
    }
  }
  // Words end at a hyphen, so a line never stops in the middle of one when a hyphen is nearer.
  assert.deepEqual(wrapLines("lamp-on-the-desk-now-ok", MONTSERRAT_32, 179), [
    "lamp-on-",
    "the-desk-",
    "now-ok",
  ]);
});

test("a fixed-height label ends in dots when its text needs more lines", () => {
  const long = "Home Assistant Garage Door Lights";
  const two = dotLines(long, MONTSERRAT_32, 179, 2);
  assert.equal(two.length, 2);
  assert.ok(two[1].endsWith("..."), two.join("|"));
  const one = dotLines("Restart Plex", MONTSERRAT_32, 179, 1);
  assert.equal(one.length, 1);
  assert.ok(one[0].endsWith("..."), one[0]);
  assert.ok(textWidth(MONTSERRAT_32, one[0]) <= 179);
  // What shows is the start of the label, then the dots.
  assert.ok("Restart Plex".startsWith(one[0].slice(0, -3)));
});

test("a label that fits is left alone, in two lines or one", () => {
  assert.deepEqual(dotLines("Restart Plex", MONTSERRAT_32, 179, 2), [
    "Restart",
    "Plex",
  ]);
  assert.deepEqual(dotLines("Plex", MONTSERRAT_32, 179, 1), ["Plex"]);
});

test("dotted labels never exceed their lines or their width", () => {
  let seed = 7;
  const next = () => {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return seed;
  };
  const alphabet =
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_.";
  for (let n = 0; n < 500; n++) {
    const length = 1 + (next() % 47);
    const text = Array.from(
      { length },
      () => alphabet[next() % alphabet.length],
    ).join("");
    for (const [font, width, max] of [
      [MONTSERRAT_32, 179, 2],
      [MONTSERRAT_32, 179, 1],
      [MONTSERRAT_32, 103, 1],
      [MONTSERRAT_24, 103, 1],
    ] as const) {
      const lines = dotLines(text, font, width, max);
      assert.ok(lines.length <= max, `${text} -> ${lines.join("|")}`);
      for (const line of lines) {
        assert.ok(
          textWidth(font, line) <= width,
          `${text} -> ${lines.join("|")}`,
        );
      }
    }
  }
});

test("a button lays out as build_grid does", () => {
  // Plain: the label wraps and grows.
  const plain = layoutButton(
    { label: "Restart Plex", hasIcon: false, isTile: false },
    5,
    3,
  );
  assert.deepEqual(plain.label?.lines, ["Restart", "Plex"]);
  assert.equal(plain.label?.height, 70);
  assert.equal(plain.icon, null);
  assert.equal(plain.value, null);

  // With an icon: a fixed two lines on 3 rows, one on 4.
  assert.equal(
    layoutButton({ label: "x", hasIcon: true, isTile: false }, 5, 3).label
      ?.height,
    70,
  );
  assert.equal(
    layoutButton({ label: "x", hasIcon: true, isTile: false }, 5, 4).label
      ?.height,
    35,
  );

  // A tile: 24 pt label on one line, the value under it in 32 pt in tall cells, 24 pt in short ones.
  const tile = layoutButton(
    { label: "Load", hasIcon: false, isTile: true },
    5,
    3,
    "0.42",
  );
  assert.deepEqual(
    [tile.label?.size, tile.value?.size, tile.value?.lines],
    [24, 32, ["0.42"]],
  );
  const short = layoutButton(
    { label: "Load", hasIcon: false, isTile: true },
    5,
    5,
    "0.42",
  );
  assert.equal(short.value?.size, 24);

  // With an icon on 4 or 5 rows there is room for the icon and the value only.
  assert.equal(
    layoutButton({ label: "Load", hasIcon: true, isTile: true }, 5, 4).label,
    null,
  );
  assert.notEqual(
    layoutButton({ label: "Load", hasIcon: true, isTile: true }, 5, 3).label,
    null,
  );

  // A value is one line, whatever it holds.
  const wide = layoutButton(
    { label: "Load", hasIcon: false, isTile: true },
    5,
    3,
    "a b\nc ".repeat(20),
  );
  assert.equal(wide.value?.lines.length, 1);
});
