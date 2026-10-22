// Run with `pnpm test`. Node strips the types, so these import with the .ts extension.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import { FONTS, fontInfo, slotFont } from "../src/fonts.ts";
import { dotLines, textWidth, wrapLines } from "../src/lvtext.ts";
import { MONTSERRAT_24, MONTSERRAT_32 } from "../src/montserrat.ts";
import {
  cellAt,
  cellRect,
  cellSize,
  GRID_H,
  iconBox,
  labelLines,
  layoutButton,
  PAGER_H,
  pagerLabel,
} from "../src/preview.ts";

test("the preview of a 5x3 grid is 97 px icons and no label, and with 2 pages 87 px and the label", () => {
  const preview = (pages: number, page: number) => ({
    icon: iconBox(5, 3, pages >= 2),
    label: pagerLabel(page, pages),
  });
  assert.deepEqual(preview(1, 0), { icon: 97, label: null });
  assert.deepEqual(preview(2, 0), { icon: 87, label: "Page 1 of 2" });
  assert.equal(pagerLabel(2, 4), "Page 3 of 4");
});

test("cells match build_grid on the grids the firmware was checked with", () => {
  assert.equal(GRID_H, 556); // the editor's stage at scale 1: 600 less GRID_TOP 44
  assert.deepEqual(cellSize(5, 3), { w: 195, h: 177 });
  assert.deepEqual(cellRect(0, 0, cellSize(5, 3)), {
    w: 195,
    h: 177,
    x: 8,
    y: 0,
  });
  assert.deepEqual(cellRect(4, 2, cellSize(5, 3)), {
    w: 195,
    h: 177,
    x: 820,
    y: 370,
  });
});

test("icon boxes are what icons.rs pins in `the_box_follows_the_firmware_button`", () => {
  // Cell width less padding, by column count; the rows decide the height.
  const widths = [1000, 492, 322, 238, 187, 153, 129, 111];
  const heights = [256, 190, 97, 51, 58];
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

test("the paged geometry table is the one firmware test_grid.c and icons.rs pin", () => {
  assert.equal(PAGER_H, 31);
  const widths = [1000, 492, 322, 238, 187, 153, 129, 111];
  // [rows, cell height, label lines, icon box] for the full grid (556 px), then for the grid less PAGER_H (525 px)
  const full = [
    [1, 548, 2, 256],
    [2, 270, 2, 190],
    [3, 177, 2, 97],
    [4, 131, 2, 51],
    [5, 103, 1, 58],
  ];
  const paged = [
    [1, 517, 2, 256],
    [2, 254, 2, 174],
    [3, 167, 2, 87],
    [4, 123, 2, 43],
    [5, 97, 1, 52],
  ];
  for (const [isPaged, table] of [
    [false, full],
    [true, paged],
  ] as const) {
    for (const [rows, h, lines, box] of table) {
      const cell = cellSize(1, rows, isPaged);
      assert.equal(cell.h, h, `${rows} rows paged ${isPaged}`);
      assert.equal(labelLines(cell), lines, `${rows} rows paged ${isPaged}`);
      for (let cols = 1; cols <= 8; cols++) {
        assert.equal(
          iconBox(cols, rows, isPaged),
          Math.min(box, widths[cols - 1]),
          `${cols}x${rows} paged ${isPaged}`,
        );
      }
    }
  }
  // Omitting the flag is the unpaged grid.
  assert.deepEqual(cellSize(5, 3), cellSize(5, 3, false));
  assert.equal(iconBox(5, 3), 97);
  assert.equal(iconBox(5, 3, true), 87);
});

test("a paged grid maps points on its shorter cells, and layoutButton follows it", () => {
  const cell = cellSize(5, 3, true);
  assert.deepEqual(cell, { w: 195, h: 167 });
  const rect = cellRect(2, 2, cell);
  assert.deepEqual(cellAt(rect.x + 5, rect.y + 5, 5, 3, true), [2, 2]);
  assert.equal(cellAt(rect.x + 5, rect.y + cell.h, 5, 3, true), null);
  const button = { label: "x", hasIcon: true, isTile: false };
  assert.equal(layoutButton(button, 5, 3).icon, 97);
  assert.equal(layoutButton(button, 5, 3, "", undefined, true).icon, 87);
});

test("a button has two label lines until that would leave its icon under 24 px", () => {
  assert.deepEqual(
    [1, 2, 3, 4, 5].map((rows) => labelLines(cellSize(5, rows))),
    [2, 2, 2, 2, 1],
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
  assert.equal(
    cellAt(500, 560, 5, 3),
    null,
    "below the last row, under the 556 px grid",
  );
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
  const two = dotLines(long, MONTSERRAT_32, 179, 70);
  assert.equal(two.length, 2);
  assert.ok(two[1].endsWith("..."), two.join("|"));
  const one = dotLines("Restart Plex", MONTSERRAT_32, 179, 35);
  assert.equal(one.length, 1);
  assert.ok(one[0].endsWith("..."), one[0]);
  assert.ok(textWidth(MONTSERRAT_32, one[0]) <= 179);
  // What shows is the start of the label, then the dots.
  assert.ok("Restart Plex".startsWith(one[0].slice(0, -3)));
});

test("a label that fits is left alone, in two lines or one", () => {
  assert.deepEqual(dotLines("Restart Plex", MONTSERRAT_32, 179, 70), [
    "Restart",
    "Plex",
  ]);
  assert.deepEqual(dotLines("Plex", MONTSERRAT_32, 179, 35), ["Plex"]);
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
    for (const [font, width, box, max] of [
      [MONTSERRAT_32, 179, 70, 2],
      [MONTSERRAT_32, 179, 35, 1],
      [MONTSERRAT_32, 103, 35, 1],
      [MONTSERRAT_24, 103, 27, 1],
    ] as const) {
      const lines = dotLines(text, font, width, box);
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

  // With an icon: a fixed two lines on 3 and 4 rows, one on 5.
  assert.equal(
    layoutButton({ label: "x", hasIcon: true, isTile: false }, 5, 3).label
      ?.height,
    70,
  );
  assert.equal(
    layoutButton({ label: "x", hasIcon: true, isTile: false }, 5, 4).label
      ?.height,
    70,
  );
  assert.equal(
    layoutButton({ label: "x", hasIcon: true, isTile: false }, 5, 5).label
      ?.height,
    35,
  );

  // A tile: 24 pt label on one line, the value under it in 32 pt. Every grid of up to 5 rows is at
  // least 80 px tall per cell (97 paged), so the value is 32 pt on all of them.
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
  assert.equal(short.value?.size, 32);

  // With an icon on 5 rows there is room for the icon and the value only; 4 rows keep a label.
  assert.equal(
    layoutButton({ label: "Load", hasIcon: true, isTile: true }, 5, 5).label,
    null,
  );
  assert.notEqual(
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

const LONG = "Home Assistant Garage Door Lights Dashboard Server Room Cameras";

test("every generated slot's line height fits its line box and keeps its line count", () => {
  for (const font of FONTS) {
    const { slot24, slot32 } = font;
    // 2 lines in the 70 px icon label, 1 in 35 px, 1 in 27 px (ui.c SLOT*_H).
    assert.ok(slot32.lineHeight <= 35, `${font.id} slot 32`);
    assert.ok(slot24.lineHeight <= 27, `${font.id} slot 24`);
    assert.equal(Math.floor(70 / slot32.lineHeight), 2, `${font.id} slot 32`);
    assert.equal(Math.floor(35 / slot32.lineHeight), 1, `${font.id} slot 32`);
    assert.equal(Math.floor(27 / slot24.lineHeight), 1, `${font.id} slot 24`);
  }
});

test("iconBox and labelLines, and every fixed box, are the same for every font on every grid size", () => {
  for (let cols = 1; cols <= 8; cols++) {
    for (let rows = 1; rows <= 5; rows++) {
      const plain = { label: "x", hasIcon: true, isTile: false };
      const tile = { label: "x", hasIcon: false, isTile: true };
      const box = (b: typeof plain, id: string) => {
        const l = layoutButton(b, cols, rows, "1", { label: id, value: id });
        return [l.icon, l.label?.height ?? null, l.value?.height ?? null];
      };
      for (const { id } of FONTS) {
        assert.deepEqual(
          box(plain, id),
          box(plain, "montserrat"),
          `${id} icon ${cols}x${rows}`,
        );
        assert.deepEqual(
          box(tile, id),
          box(tile, "montserrat"),
          `${id} tile ${cols}x${rows}`,
        );
      }
    }
  }
});

test("the fonts with the smallest and largest line height cut a long label where the box ends", () => {
  const byHeight = (slot: 24 | 32) =>
    FONTS.map((f) => f.id).sort(
      (a, b) =>
        slotFont(a, slot).lineHeight - slotFont(b, slot).lineHeight ||
        a.localeCompare(b),
    );
  const ends = (slot: 24 | 32) => {
    const sorted = byHeight(slot);
    return [sorted[0], sorted[sorted.length - 1]];
  };
  const [small, large] = ends(32);
  const [small24, large24] = ends(24);
  assert.notEqual(
    slotFont(small, 32).lineHeight,
    slotFont(large, 32).lineHeight,
  );
  for (const id of [small, large]) {
    const two = dotLines(LONG, slotFont(id, 32), 179, 70);
    assert.equal(two.length, 2, `${id} 70 px`);
    assert.ok(two[1].endsWith("..."), `${id}: ${two.join("|")}`);
    const one = dotLines(LONG, slotFont(id, 32), 179, 35);
    assert.equal(one.length, 1, `${id} 35 px`);
    assert.ok(one[0].endsWith("..."), `${id}: ${one[0]}`);
  }
  for (const id of [small24, large24]) {
    const one = dotLines(LONG, slotFont(id, 24), 179, 27);
    assert.equal(one.length, 1, `${id} 27 px`);
    assert.ok(one[0].endsWith("..."), `${id}: ${one[0]}`);
  }
});

test("the same text breaks differently in Orbitron and Inter", () => {
  const text = "Garage Door Opener Lights";
  const orbitron = wrapLines(text, slotFont("orbitron", 32), 179);
  const inter = wrapLines(text, slotFont("inter", 32), 179);
  assert.notDeepEqual(orbitron, inter);
  assert.ok(orbitron.length > inter.length, `${orbitron} / ${inter}`);
});

test("fonts.css declares a face for every generated font", () => {
  const css = readFileSync(
    new URL("../src/fonts.css", import.meta.url),
    "utf8",
  );
  for (const { id, name } of FONTS) {
    if (id !== "montserrat") {
      assert.ok(css.includes(`font-family: "${name}";`), name);
    }
  }
});

test("a laid out button carries its font's family and size", () => {
  const button = { label: "Load", hasIcon: false, isTile: true };
  const l = layoutButton(button, 5, 3, "0.42", {
    label: "orbitron",
    value: "fira-code",
  });
  assert.equal(l.label?.family, fontInfo("orbitron").family);
  assert.equal(l.label?.px, slotFont("orbitron", 24).px);
  assert.equal(l.value?.family, fontInfo("fira-code").family);
  assert.equal(l.value?.px, slotFont("fira-code", 32).px);
  // A font the table lacks draws as Montserrat, as the board does.
  const odd = layoutButton(button, 5, 3, "0.42", {
    label: "nope",
    value: "nope",
  });
  assert.equal(odd.label?.family, fontInfo("montserrat").family);
});
