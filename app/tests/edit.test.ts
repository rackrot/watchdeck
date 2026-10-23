import assert from "node:assert/strict";
import { test } from "node:test";
import type { ButtonConfig, Config } from "../src/config.ts";
import {
  addPage,
  barChanged,
  buttonAt,
  buttonsOnPage,
  canon,
  duplicateNames,
  layoutChanged,
  layoutTargets,
  moveButton,
  movePage,
  pageCount,
  pageIndexOf,
  pageName,
  parseWhole,
  pruneBar,
  recordToRows,
  removePage,
  renamePage,
  rowsToRecord,
  withGpu,
  withGridFont,
  withSteps,
} from "../src/edit.ts";
import { hasErrors, mapErrors, numberErrors, unquote } from "../src/errors.ts";
import { resolveFonts } from "../src/fonts.ts";

const button = (id: string, pos: [number, number]): ButtonConfig => ({
  id,
  pos,
  label: id,
  bg: "#1e88e5",
  fg: "#ffffff",
  action: { kind: "shell", argv: ["true"], timeout_s: 10 },
});

const config: Config = {
  version: 1,
  layout: { cols: 5, rows: 3 },
  button: [button("a", [0, 0]), button("b", [1, 0])],
};

test("header rows become a table, dropping blank rows and keeping the last of a repeat", () => {
  assert.equal(rowsToRecord([]), undefined);
  assert.equal(rowsToRecord([{ name: "", value: "" }]), undefined);
  assert.deepEqual(
    rowsToRecord([
      { name: "A", value: "1" },
      { name: "B", value: "2" },
      { name: "A", value: "3" },
    ]),
    { A: "3", B: "2" },
  );
  assert.deepEqual(recordToRows({ A: "1" }), [{ name: "A", value: "1" }]);
  assert.deepEqual(recordToRows(undefined), []);
  assert.deepEqual(
    duplicateNames([
      { name: "A", value: "" },
      { name: "A", value: "" },
      { name: "B", value: "" },
    ]),
    ["A"],
  );
});

test("a number field accepts whole numbers only", () => {
  assert.equal(parseWhole("12"), 12);
  assert.equal(parseWhole(" 7 "), 7);
  for (const bad of ["", "-1", "1.5", "1e3", "x", "0x10"]) {
    assert.ok(Number.isNaN(parseWhole(bad)), bad);
  }
});

test("moving onto an empty cell moves, and onto a button swaps", () => {
  const moved = moveButton(config, 0, [3, 2]);
  assert.deepEqual(
    moved.button.map((b) => b.pos),
    [
      [3, 2],
      [1, 0],
    ],
  );
  const swapped = moveButton(config, 0, [1, 0]);
  assert.deepEqual(
    swapped.button.map((b) => b.pos),
    [
      [1, 0],
      [0, 0],
    ],
  );
  assert.equal(moveButton(config, 9, [0, 0]), config);
  assert.deepEqual(config.button[0].pos, [0, 0], "the input is not changed");
});

test("only what a device draws counts as a layout change", () => {
  const edit = (change: Partial<ButtonConfig>): Config => ({
    ...config,
    button: [{ ...config.button[0], ...change }, config.button[1]],
  });
  assert.equal(layoutChanged(config, structuredClone(config)), false);
  assert.equal(
    layoutChanged(
      config,
      edit({ action: { kind: "shell", argv: ["false"], timeout_s: 30 } }),
    ),
    false,
    "an action edit",
  );
  const changes: Partial<ButtonConfig>[] = [
    { label: "new" },
    { bg: "#000000" },
    { fg: "#000000" },
    { icon: "a.png" },
    { pos: [4, 2] },
    { id: "z" },
    {
      tile: {
        every_s: 5,
        source: { kind: "shell", argv: ["true"], timeout_s: 10 },
      },
    },
  ];
  for (const change of changes) {
    assert.equal(
      layoutChanged(config, edit(change)),
      true,
      JSON.stringify(change),
    );
  }
  assert.equal(
    layoutChanged(config, { ...config, layout: { cols: 4, rows: 3 } }),
    true,
  );
  const tiled = edit({ tile: { every_s: 5, source: config.button[0].action } });
  const retiled = edit({
    tile: { every_s: 9, source: config.button[0].action },
  });
  assert.equal(
    layoutChanged(tiled, retiled),
    false,
    "a tile edit needs no layout",
  );
});

test("canon ignores key order", () => {
  assert.equal(
    canon({ a: 1, b: { c: 2, d: [3] } }),
    canon({ b: { d: [3], c: 2 }, a: 1 }),
  );
  assert.notEqual(canon({ a: 1 }), canon({ a: 2 }));
});

test("a font change at any of the three levels is a layout change", () => {
  const tiled = (tile?: string): Config => ({
    ...config,
    button: [
      {
        ...config.button[0],
        tile: { every_s: 5, source: config.button[0].action, font: tile },
      },
      config.button[1],
    ],
  });
  const withFont = (c: Config, font?: string): Config => ({
    ...c,
    layout: { ...c.layout, font },
  });
  // The grid's font: even with no button to show it, the message carries it.
  assert.equal(layoutChanged(config, withFont(config, "inter")), true, "grid");
  // A button's font.
  assert.equal(
    layoutChanged(config, {
      ...config,
      button: [{ ...config.button[0], font: "orbitron" }, config.button[1]],
    }),
    true,
    "button",
  );
  // A tile's value font.
  assert.equal(layoutChanged(tiled(), tiled("fira-code")), true, "tile");
  assert.equal(
    layoutChanged(
      withFont(tiled(), "inter"),
      withFont(tiled("inter"), "inter"),
    ),
    false,
    "a pinned value font that equals what it inherits draws the same",
  );
  assert.equal(
    layoutChanged(config, withFont(config, "montserrat")),
    false,
    "Montserrat is the default",
  );
  // An action-only edit, with fonts set, is still not one.
  const fonty = withFont(tiled("fira-code"), "inter");
  assert.equal(
    layoutChanged(fonty, {
      ...fonty,
      button: [
        {
          ...fonty.button[0],
          action: { kind: "shell", argv: ["false"], timeout_s: 30 },
        },
        fonty.button[1],
      ],
    }),
    false,
  );
});

test("a font-only save reaches only the devices that have the fonts capability", () => {
  const devices = [
    { id: "new", connected: true, caps: ["fonts"] },
    { id: "old", connected: true, caps: [] },
    { id: "gone", connected: false, caps: ["fonts"] },
  ];
  const withTile = (font?: string): Config => ({
    ...config,
    button: [
      {
        ...config.button[0],
        tile: { every_s: 5, source: config.button[0].action, font },
      },
      config.button[1],
    ],
  });
  // Font changes at the grid, button and tile levels.
  for (const after of [
    withGridFont(config, "inter"),
    {
      ...config,
      button: [{ ...config.button[0], font: "orbitron" }, config.button[1]],
    },
  ]) {
    assert.deepEqual(layoutTargets(config, after, devices), ["new"]);
  }
  assert.deepEqual(layoutTargets(withTile(), withTile("fira-code"), devices), [
    "new",
  ]);
  // Any other change still goes to every connected device, even with a font change in it.
  const moved = withGridFont(
    { ...config, layout: { ...config.layout, cols: 4 } },
    "inter",
  );
  assert.deepEqual(layoutTargets(config, moved, devices), ["new", "old"]);
  // No layout change, no targets.
  assert.deepEqual(layoutTargets(config, config, devices), []);
});

test("picking Montserrat for the grid removes layout.font and keeps the rest", () => {
  const inter = withGridFont(config, "inter");
  assert.deepEqual(inter.layout, { cols: 5, rows: 3, font: "inter" });
  assert.deepEqual(withGridFont(inter, "montserrat").layout, {
    cols: 5,
    rows: 3,
  });
  assert.equal("font" in withGridFont(inter, "montserrat").layout, false);
});

// The rows of `fonts_go_on_the_wire_only_where_they_differ_from_what_they_inherit` in config.rs:
// (grid, button font, tile (null: none; else its font or undefined), message font, label_font,
// value_font). The board resolves label = label_font ?? message font ?? montserrat and
// value = value_font ?? label, so resolveFonts must give back exactly that.
test("resolveFonts matches the daemon's wire matrix", () => {
  const U = undefined;
  const rows: [
    string | undefined,
    string | undefined,
    { font: string | undefined } | null,
    string | undefined,
    string | undefined,
    string | undefined,
  ][] = [
    [U, U, null, U, U, U],
    [U, U, { font: U }, U, U, U],
    ["inter", U, { font: U }, "inter", U, U],
    [U, "orbitron", { font: U }, U, "orbitron", U],
    [U, U, { font: "fira-code" }, U, U, "fira-code"],
    [
      "inter",
      "orbitron",
      { font: "fira-code" },
      "inter",
      "orbitron",
      "fira-code",
    ],
    ["inter", "montserrat", null, "inter", "montserrat", U],
    [U, "montserrat", null, U, U, U],
    ["montserrat", U, null, U, U, U],
    ["inter", "inter", { font: "inter" }, "inter", U, U],
    [U, "orbitron", { font: "orbitron" }, U, "orbitron", U],
  ];
  for (const [grid, font, tile, message, labelFont, valueFont] of rows) {
    const got = resolveFonts(
      { font: grid },
      {
        font,
        tile: tile
          ? { every_s: 5, source: config.button[0].action, font: tile.font }
          : undefined,
      },
    );
    const label = labelFont ?? message ?? "montserrat";
    assert.equal(got.label, label, `grid ${grid} font ${font}`);
    if (tile) {
      assert.equal(got.value, valueFont ?? label, `grid ${grid} font ${font}`);
    }
  }
  // `a_tile_value_in_montserrat_under_another_label_font_is_sent`: the label is the grid's Inter,
  // the value the tile's pinned Montserrat.
  assert.deepEqual(
    resolveFonts(
      { font: "inter" },
      {
        tile: {
          every_s: 5,
          source: config.button[0].action,
          font: "montserrat",
        },
      },
    ),
    { label: "inter", value: "montserrat" },
  );
  // Nothing set is Montserrat; a lone tile font moves only the value.
  assert.deepEqual(resolveFonts({}, {}), {
    label: "montserrat",
    value: "montserrat",
  });
});

test("font problems land on the font fields and the grid", () => {
  const list = "montserrat, inter";
  const map = mapErrors([
    `layout.font "x" is not one of ${list}`,
    `button "a": font "x" is not one of ${list}`,
    `button "a": tile.font "x" is not one of ${list}`,
  ]);
  assert.equal(map.grid.length, 1);
  assert.equal(map.general.length, 0);
  const a = map.buttons.get("a") ?? {};
  assert.equal(a.font?.length, 1);
  assert.equal(a["tile.font"]?.length, 1);
  assert.equal(a.button, undefined);
});

// The messages below are the ones Config::check and Configs::save build in config.rs.
test("button problems land on the field they are about", () => {
  const map = mapErrors([
    'button "plex": pos [7, 0] is outside the 5x3 grid',
    'button "plex": pos [0, 0] is taken by button "b"',
    'button "plex": label is 60 bytes, at most 47',
    'button "plex": label must be printable ASCII, the only characters the device can draw',
    "button \"plex\": id must be 1 to 63 letters, digits, '_', '-' or '.'",
    'button "plex": another button has this id',
    'button "plex": bg "red" is not a #rrggbb color',
    'button "plex": fg "" is not a #rrggbb color',
    'button "plex": icon "x.png": can\'t decode the image',
    'button "plex": icon must be a file name in icons/: 1 to 63 letters',
    'button "plex": timeout_s is 0, expected 1 to 600',
    'button "plex": argv is empty',
    'button "plex": argv[0], the program, is empty',
    'button "plex": method "FETCH" is not one of GET, HEAD, POST, PUT, PATCH, DELETE',
    'button "plex": url does not parse: relative URL without a base',
    'button "plex": url has no host',
    'button "plex": secret_headers would cross the network in the clear over http://; use https://',
    'button "plex": insecure_tls only applies to https:// urls',
    'button "plex": tile.source: insecure_tls only applies to https:// urls',
    'button "plex": body is over 64 KB',
    'button "plex": "bad name" is not a valid header name',
    'button "plex": header "A" is set twice; names ignore case',
    'button "plex": header "A" must have a value of printable ASCII on one line',
    'button "plex": header "Authorization" uses secret "ha", which is not in secrets.toml',
  ]);
  const plex = map.buttons.get("plex") ?? {};
  const count = (field: keyof typeof plex) => plex[field]?.length ?? 0;
  assert.deepEqual(
    {
      pos: count("pos"),
      label: count("label"),
      id: count("id"),
      bg: count("bg"),
      fg: count("fg"),
      icon: count("icon"),
      timeout: count("action.timeout_s"),
      argv: count("action.argv"),
      method: count("action.method"),
      url: count("action.url"),
      insecure: count("action.insecure_http"),
      insecureTls: count("action.insecure_tls"),
      tileInsecureTls: count("tile.source.insecure_tls"),
      body: count("action.body"),
      headers: count("action.headers"),
      secrets: count("action.secret_headers"),
      other: count("button"),
    },
    {
      pos: 2,
      label: 2,
      id: 2,
      bg: 1,
      fg: 1,
      icon: 2,
      timeout: 1,
      argv: 2,
      method: 1,
      url: 2,
      insecure: 1,
      insecureTls: 1,
      tileInsecureTls: 1,
      body: 1,
      headers: 3,
      secrets: 2,
      other: 0,
    },
  );
  assert.equal(map.buttons.size, 1);
});

test("tile problems land on the tile's fields, its source's included", () => {
  const map = mapErrors([
    'button "load": tile.every_s is 1, expected 2 to 86400',
    'button "load": tile.json_pointer must start with \'/\', like "/state"',
    'button "load": tile.source: argv is empty',
    'button "load": tile.source: timeout_s is 0, expected 1 to 600',
    'button "load": tile.source: header "A" uses secret "ha", which is not in secrets.toml',
    'button "load": tile.source: something new',
  ]);
  const load = map.buttons.get("load") ?? {};
  assert.equal(load["tile.every_s"]?.length, 1);
  assert.equal(load["tile.json_pointer"]?.length, 1);
  assert.equal(load["tile.source.argv"]?.length, 1);
  assert.equal(load["tile.source.timeout_s"]?.length, 1);
  assert.equal(load["tile.source.secret_headers"]?.length, 1);
  assert.equal(
    load.button?.length,
    1,
    "a message it doesn't know stays on the button",
  );
});

test("grid, file and save problems are not tied to a button", () => {
  const map = mapErrors([
    "layout.cols is 9, expected 1 to 8",
    "layout.rows is 0, expected 1 to 5",
    "version is 2, expected 1",
    "config.toml changed since it was loaded; load it again before saving",
    "Could not save config.toml: Permission denied",
  ]);
  assert.equal(map.grid.length, 3);
  assert.equal(map.general.length, 2);
  assert.equal(map.buttons.size, 0);
});

test("a button id with quotes or escapes is read back as written", () => {
  const map = mapErrors(['button "a\\"b": id must be 1 to 63 letters, digits']);
  assert.deepEqual([...map.buttons.keys()], ['a"b']);
});

test("numbers that can't be sent are reported before the call", () => {
  assert.equal(numberErrors(config).grid.length, 0);
  const bad: Config = {
    version: 1,
    layout: { cols: Number.NaN, rows: -1 },
    button: [
      {
        ...button("a", [Number.NaN, 0]),
        action: { kind: "shell", argv: ["x"], timeout_s: Number.NaN },
        tile: {
          every_s: 1.5,
          source: { kind: "shell", argv: ["x"], timeout_s: Number.NaN },
        },
      },
    ],
  };
  const map = numberErrors(bad);
  assert.equal(map.grid.length, 2);
  const a = map.buttons.get("a") ?? {};
  assert.deepEqual(Object.keys(a).sort(), [
    "action.timeout_s",
    "pos",
    "tile.every_s",
    "tile.source.timeout_s",
  ]);
});

test("Rust's debug escapes in an id are read back, and a bad one falls back", () => {
  assert.equal(unquote('"plain"'), "plain");
  assert.equal(unquote('"a\\"b"'), 'a"b');
  assert.equal(unquote('"back\\\\slash"'), "back\\slash");
  assert.equal(unquote('"tab\\there"'), "tab\there");
  assert.equal(unquote('"cafe\\u{e9}"'), "cafe\u00e9");
  assert.equal(unquote('"\\u{1f600}"'), "\u{1f600}");
  assert.equal(unquote('"nul\\0"'), "nul\0");
  // Not an escape Rust writes: the text between the quotes, as is.
  assert.equal(unquote('"bad\\q"'), "bad\\q");
  assert.equal(unquote('"bad\\u{zz}"'), "bad\\u{zz}");
  assert.equal(unquote('"big\\u{ffffff}"'), "big\\u{ffffff}");
});

test("an error about an id with a Rust escape reaches that button", () => {
  const map = mapErrors([
    'button "cafe\\u{e9}": label is 60 bytes, at most 47',
  ]);
  assert.deepEqual([...map.buttons.keys()], ["cafe\u00e9"]);
});

test("an error about a button the draft doesn't have goes to the general list, whole", () => {
  const message = 'button "ghost": pos [7, 0] is outside the 5x3 grid';
  const map = mapErrors([message, 'button "a": argv is empty'], ["a", "b"]);
  assert.deepEqual(map.general, [message]);
  assert.deepEqual([...map.buttons.keys()], ["a"]);
});

test("an invalid header name shows under both header tables", () => {
  const plain = mapErrors([
    'button "a": "bad name" is not a valid header name',
  ]).buttons.get("a");
  assert.equal(plain?.["action.headers"]?.length, 1);
  assert.equal(plain?.["action.secret_headers"]?.length, 1);
  const tile = mapErrors([
    'button "a": tile.source: "bad name" is not a valid header name',
  ]).buttons.get("a");
  assert.equal(tile?.["tile.source.headers"]?.length, 1);
  assert.equal(tile?.["tile.source.secret_headers"]?.length, 1);
  assert.equal(tile?.["action.headers"], undefined);
});

// Pages. `a` and `b` are on the first page (`b` implicitly), `c` is on "media".
const pagedConfig: Config = {
  ...config,
  page: [{ id: "main" }, { id: "media", label: "Media" }],
  button: [
    { ...button("a", [0, 0]), page: "main" },
    button("b", [1, 0]),
    { ...button("c", [0, 0]), page: "media" },
  ],
};

const bothCaps = { id: "both", connected: true, caps: ["fonts", "pages"] };
const pagesOnly = { id: "pages", connected: true, caps: ["pages"] };
const fontsOnly = { id: "fonts", connected: true, caps: ["fonts"] };
const neither = { id: "old", connected: true, caps: [] };
const everyone = [bothCaps, pagesOnly, fontsOnly, neither];

const ids = (...devices: { id: string }[]) => devices.map((d) => d.id);

/** `pagedConfig` with the button called `id` changed. */
const editPaged = (id: string, change: Partial<ButtonConfig>): Config => ({
  ...pagedConfig,
  button: pagedConfig.button.map((b) =>
    b.id === id ? { ...b, ...change } : b,
  ),
});

test("layoutChanged counts the page ids in order and each button's page, never a label", () => {
  assert.equal(layoutChanged(pagedConfig, pagedConfig), false);
  // A label is config only.
  assert.equal(
    layoutChanged(pagedConfig, renamePage(pagedConfig, 1, "Films")),
    false,
  );
  assert.equal(
    layoutChanged(pagedConfig, renamePage(pagedConfig, 1, "")),
    false,
  );
  // An action edit on any page is not a layout change.
  assert.equal(
    layoutChanged(
      pagedConfig,
      editPaged("c", { action: { kind: "shell", argv: ["x"], timeout_s: 1 } }),
    ),
    false,
  );
  // A page added, a page's id, the order of the pages and a button's page all are.
  assert.equal(layoutChanged(config, addPage(config)), true);
  assert.equal(layoutChanged(pagedConfig, addPage(pagedConfig)), true);
  const renamedId = {
    ...pagedConfig,
    page: [{ id: "main" }, { id: "films" }],
    button: pagedConfig.button.map((b) =>
      b.page === "media" ? { ...b, page: "films" } : b,
    ),
  };
  assert.equal(layoutChanged(pagedConfig, renamedId), true);
  assert.equal(layoutChanged(pagedConfig, movePage(pagedConfig, 1, -1)), true);
  assert.equal(
    layoutChanged(pagedConfig, editPaged("b", { page: "media" })),
    true,
  );
  assert.equal(
    layoutChanged(pagedConfig, editPaged("c", { label: "C!" })),
    true,
  );
});

test("layoutTargets compares each device's view, as the daemon does", () => {
  // An edit on page 2 reaches only the devices that have pages.
  const onSecond = editPaged("c", { label: "Films" });
  assert.deepEqual(
    layoutTargets(pagedConfig, onSecond, everyone),
    ids(bothCaps, pagesOnly),
  );
  // An edit on the home page reaches every connected device.
  const onHome = editPaged("a", { label: "Home" });
  assert.deepEqual(
    layoutTargets(pagedConfig, onHome, everyone),
    ids(...everyone),
  );
  // From 1 page to 2 with no icon on the home page: only the pages devices get a new layout. The
  // others see the same home page, and `paged` only resizes icons.
  assert.deepEqual(
    layoutTargets(config, addPage(config), everyone),
    ids(bothCaps, pagesOnly),
  );
  assert.deepEqual(
    layoutTargets(addPage(config), config, everyone),
    ids(bothCaps, pagesOnly),
  );
  // With an icon on the home page its box changes size, so every device is sent a layout.
  const iconed: Config = {
    ...config,
    button: [{ ...config.button[0], icon: "a.png" }, config.button[1]],
  };
  assert.deepEqual(
    layoutTargets(iconed, addPage(iconed), everyone),
    ids(...everyone),
  );
  assert.deepEqual(
    layoutTargets(addPage(iconed), iconed, everyone),
    ids(...everyone),
  );
  // A font-only edit on page 2 reaches only the devices with both capabilities.
  const fontOnSecond = editPaged("c", { font: "orbitron" });
  assert.deepEqual(
    layoutTargets(pagedConfig, fontOnSecond, everyone),
    ids(bothCaps),
  );
  // A font-only edit on page 1 reaches the devices with fonts.
  const fontOnHome = editPaged("a", { font: "orbitron" });
  assert.deepEqual(
    layoutTargets(pagedConfig, fontOnHome, everyone),
    ids(bothCaps, fontsOnly),
  );
  // A label edit, or an action edit, reaches nobody.
  assert.deepEqual(
    layoutTargets(pagedConfig, renamePage(pagedConfig, 0, "Home"), everyone),
    [],
  );
  assert.deepEqual(layoutTargets(pagedConfig, pagedConfig, everyone), []);
  // Moving a button from page 2 to the home page changes the home view too.
  assert.deepEqual(
    layoutTargets(
      pagedConfig,
      editPaged("c", { page: "main", pos: [2, 2] }),
      everyone,
    ),
    ids(...everyone),
  );
  // Offline devices are never targets.
  assert.deepEqual(
    layoutTargets(pagedConfig, onHome, [{ ...bothCaps, connected: false }]),
    [],
  );
});

test("a single [[page]] table, or a rename of the only page, changes nothing a device sees", () => {
  const solo = { ...config, page: [{ id: "main" }] };
  assert.equal(layoutChanged(config, solo), false);
  assert.equal(layoutChanged(solo, { ...solo, page: [{ id: "home" }] }), false);
  assert.deepEqual(layoutTargets(config, solo, everyone), []);
  // Adding the second page does change it.
  assert.equal(layoutChanged(solo, addPage(solo)), true);
});

test("page problems land on the page tabs, a button's page problem on its page select, counts on the grid", () => {
  const map = mapErrors(
    [
      'page "media": label is 25 characters, at most 24',
      'page "media": label must be printable ASCII',
      'page "main": another page has this id',
      'button "c": page "nope" is not one of the [[page]] ids',
      'button "b": page "x" is set, but the file has no [[page]] tables',
      "5 pages, at most 4",
      "41 tiles across all pages, at most 40",
    ],
    ["a", "b", "c"],
    ["main", "media"],
  );
  assert.deepEqual(map.pages.get("media"), [
    "label is 25 characters, at most 24",
    "label must be printable ASCII",
  ]);
  assert.deepEqual(map.pages.get("main"), ["another page has this id"]);
  assert.equal(map.buttons.get("c")?.page?.length, 1);
  assert.equal(map.buttons.get("b")?.page?.length, 1);
  assert.deepEqual(map.grid, [
    "5 pages, at most 4",
    "41 tiles across all pages, at most 40",
  ]);
  assert.deepEqual(map.general, []);
  assert.equal(hasErrors(map), true);
  // Only a page error is still an error.
  assert.equal(hasErrors(mapErrors(['page "x": label is empty'])), true);
  // A page the draft doesn't have goes to the general list, whole.
  const stray = mapErrors(['page "gone": label is empty'], [], ["main"]);
  assert.deepEqual(stray.general, ['page "gone": label is empty']);
  assert.equal(stray.pages.size, 0);
  // A page id with a Rust escape reads back as written.
  assert.ok(mapErrors(['page "a\\"b": label is empty']).pages.has('a"b'));
});

test("adding the first page writes [[page]] tables, and deleting back to one removes them", () => {
  const two = addPage(config);
  assert.deepEqual(two.page, [{ id: "main" }, { id: "page-2" }]);
  // The buttons were on the implicit page, and still are, without a key.
  assert.deepEqual(two.button, config.button);
  assert.equal(pageCount(two), 2);
  // Back to one: the file is in its old form, no `page` key anywhere.
  const back = removePage(two, 1);
  assert.equal(canon(back), canon(config));
  assert.equal("page" in back, false);
  // With keys on the buttons, they go too.
  const keyed = removePage(editPaged("b", { page: "main" }), 1);
  assert.equal("page" in keyed, false);
  assert.ok(keyed.button.every((b) => !("page" in b)));
  assert.deepEqual(
    keyed.button.map((b) => b.id),
    ["a", "b"],
  );
});

test("a fifth page is refused and ids stay free", () => {
  let c = config;
  for (let i = 0; i < 6; i++) {
    c = addPage(c);
  }
  assert.equal(pageCount(c), 4);
  assert.deepEqual(
    c.page?.map((p) => p.id),
    ["main", "page-2", "page-3", "page-4"],
  );
  // A taken id is skipped.
  const taken = addPage({
    ...config,
    page: [{ id: "main" }, { id: "page-3" }],
  });
  assert.deepEqual(
    taken.page?.map((p) => p.id),
    ["main", "page-3", "page-4"],
  );
});

test("deleting a page of three keeps the tables and drops only its buttons", () => {
  const three = addPage(pagedConfig);
  const gone = removePage(three, 1);
  assert.deepEqual(
    gone.page?.map((p) => p.id),
    ["main", "page-3"],
  );
  assert.deepEqual(
    gone.button.map((b) => b.id),
    ["a", "b"],
  );
  assert.deepEqual(
    buttonsOnPage(three, 1).map((b) => b.id),
    ["c"],
  );
  // Deleting the home page: the buttons without a `page` were on it.
  const noHome = removePage(three, 0);
  assert.deepEqual(
    noHome.button.map((b) => b.id),
    ["c"],
  );
  assert.deepEqual(
    noHome.page?.map((p) => p.id),
    ["media", "page-3"],
  );
});

test("moving the first page keeps the buttons that had no page key where they were", () => {
  const moved = movePage(pagedConfig, 0, 1);
  assert.deepEqual(
    moved.page?.map((p) => p.id),
    ["media", "main"],
  );
  const b = moved.button.find((x) => x.id === "b");
  assert.equal(b?.page, "main");
  assert.equal(pageIndexOf(moved, b as ButtonConfig), 1);
  // The ends stay put.
  assert.equal(movePage(pagedConfig, 0, -1), pagedConfig);
  assert.equal(movePage(pagedConfig, 1, 1), pagedConfig);
});

test("a name is the label, an empty name removes it, and the tab falls back to the id", () => {
  const named = renamePage(pagedConfig, 0, "Home");
  assert.deepEqual(named.page?.[0], { id: "main", label: "Home" });
  assert.deepEqual(named.page?.[1], pagedConfig.page?.[1]);
  const cleared = renamePage(named, 0, "");
  assert.deepEqual(cleared.page?.[0], { id: "main" });
  assert.equal("label" in (cleared.page?.[0] ?? {}), false);
  assert.equal(pageName({ id: "main" }), "main");
  assert.equal(pageName({ id: "media", label: "Media" }), "Media");
});

test("a move swaps within a page, and a cell on another page is not in the way", () => {
  // `a` (main) and `c` (media) share cell 0,0.
  const swapped = moveButton(pagedConfig, 0, [1, 0]);
  assert.deepEqual(
    swapped.button.map((b) => b.pos),
    [
      [1, 0],
      [0, 0],
      [0, 0],
    ],
  );
  // Which button holds a cell on a page, for the form's refusal.
  assert.equal(buttonAt(pagedConfig, 1, [0, 0])?.id, "c");
  assert.equal(buttonAt(pagedConfig, 0, [0, 0])?.id, "a");
  assert.equal(buttonAt(pagedConfig, 0, [0, 0], 0), undefined);
  assert.equal(buttonAt(pagedConfig, 1, [1, 0]), undefined);
});

test("every layout edit keeps status_bar in the draft, and a status_bar edit is no layout change", () => {
  const status_bar: Config["status_bar"] = {
    gpu: "off",
    temp_unit: "f",
    net_unit: "bits",
    thresholds: {
      mem: [{ at: 90, color: "#e53935" }],
      net_rx: [],
    },
  };
  const withBar: Config = { ...pagedConfig, status_bar };
  const edits: Config[] = [
    moveButton(withBar, 0, [1, 0]),
    addPage(withBar),
    renamePage(withBar, 0, "Home"),
    movePage(withBar, 0, 1),
    removePage(withBar, 1),
    withGridFont(withBar, "inter"),
  ];
  for (const edited of edits) {
    assert.deepEqual(edited.status_bar, status_bar);
  }
  // Dropping to one page strips the page keys and still keeps the bar.
  assert.deepEqual(
    removePage(removePage(withBar, 1), 0).status_bar,
    status_bar,
  );

  const changed: Config = {
    ...withBar,
    status_bar: { ...status_bar, temp_unit: "c" },
  };
  assert.equal(layoutChanged(withBar, changed), false);
  assert.equal(layoutChanged(pagedConfig, withBar), false);
  const devices = [{ id: "d", connected: true, caps: ["pages", "fonts"] }];
  assert.deepEqual(layoutTargets(withBar, changed, devices), []);
});

test("barChanged is true for a [status_bar] edit and false for a button edit", () => {
  const bar: Config["status_bar"] = { temp_unit: "f" };
  const withBar: Config = { ...config, status_bar: bar };
  assert.equal(barChanged(config, withBar), true);
  assert.equal(
    barChanged(withBar, { ...withBar, status_bar: { gpu: "off" } }),
    true,
  );
  assert.equal(barChanged(withBar, withBar), false);
  assert.equal(barChanged(config, config), false);
  // A button edit, even one a device draws, leaves the bar alone.
  assert.equal(barChanged(withBar, moveButton(withBar, 0, [2, 0])), false);
  assert.equal(
    barChanged(withBar, {
      ...withBar,
      button: withBar.button.map((b) => ({ ...b, label: "x" })),
    }),
    false,
  );
});

test("a [status_bar]-only edit has no layout targets, and a button edit still does", () => {
  const devices = [
    { id: "d", connected: true, caps: ["pages", "fonts"] },
    { id: "old", connected: true, caps: [] },
  ];
  const edited: Config = {
    ...config,
    status_bar: {
      gpu: "0000:01:00.0",
      temp_unit: "f",
      net_unit: "binary",
      thresholds: { mem: [{ at: 90, color: "#e53935" }], cpu_temp: [] },
    },
  };
  assert.equal(barChanged(config, edited), true);
  assert.deepEqual(layoutTargets(config, edited, devices), []);
  assert.deepEqual(layoutTargets(edited, config, devices), []);
  const moved = moveButton(edited, 0, [2, 0]);
  assert.equal(barChanged(edited, moved), false);
  assert.deepEqual(layoutTargets(edited, moved, devices), ["d", "old"]);
});

test("steps and the GPU are set without leaving empty tables behind", () => {
  const step = { at: 90, color: "#e53935" };
  const one = withSteps(undefined, "mem", [step]);
  assert.deepEqual(one, { thresholds: { mem: [step] } });
  // `[]` is never colored and stays; removing the key takes the defaults and drops the table.
  assert.deepEqual(withSteps(one, "cpu", []), {
    thresholds: { mem: [step], cpu: [] },
  });
  assert.equal(withSteps(one, "mem", undefined), undefined);
  assert.deepEqual(withSteps({ temp_unit: "f", ...one }, "mem", undefined), {
    temp_unit: "f",
  });
  // A unit that is the default says nothing, so switching back leaves no no-op table.
  assert.equal(pruneBar({ temp_unit: "c", net_unit: "si" }), undefined);
  assert.deepEqual(pruneBar({ temp_unit: "f", net_unit: "si", ...one }), {
    temp_unit: "f",
    ...one,
  });
  assert.deepEqual(withGpu(undefined, "off"), { gpu: "off" });
  assert.equal(withGpu({ gpu: "off" }, "auto"), undefined);
  assert.deepEqual(withGpu({ gpu: "off", net_unit: "bits" }, "auto"), {
    net_unit: "bits",
  });
});

test("every status_bar message lands next to the field it is about", () => {
  const map = mapErrors([
    'status_bar.gpu "x" is not auto, off or a PCI address such as 0000:01:00.0',
    'status_bar.temp_unit "k" is not c or f',
    'status_bar.net_unit "x" is not si, binary or bits',
    "status_bar.thresholds.mem: 4 steps, at most 3",
    "status_bar.thresholds.cpu_temp: at 400 is outside 34 to 302 °F",
    "status_bar.thresholds.cpu_temp: steps must rise, 90 then 80",
    'status_bar.thresholds.gpu_temp: color "red" is not #rrggbb',
    "status_bar.thresholds.gpu: at 0 is outside 1 to 100 %",
    "status_bar.thresholds.net_rx: at 100001 is outside 1 to 100000 MiB/s",
    "status_bar.thresholds.net_tx: steps must rise, 80 then 80",
    "status_bar.thresholds.cpu: 4 steps, at most 3",
    "status_bar.something new",
  ]);
  assert.deepEqual(map.statusBar, {
    gpu: [
      'status_bar.gpu "x" is not auto, off or a PCI address such as 0000:01:00.0',
    ],
    temp_unit: ['status_bar.temp_unit "k" is not c or f'],
    net_unit: ['status_bar.net_unit "x" is not si, binary or bits'],
    "thresholds.mem": ["4 steps, at most 3"],
    "thresholds.cpu_temp": [
      "at 400 is outside 34 to 302 °F",
      "steps must rise, 90 then 80",
    ],
    "thresholds.gpu_temp": ['color "red" is not #rrggbb'],
    "thresholds.gpu": ["at 0 is outside 1 to 100 %"],
    "thresholds.net_rx": ["at 100001 is outside 1 to 100000 MiB/s"],
    "thresholds.net_tx": ["steps must rise, 80 then 80"],
    "thresholds.cpu": ["4 steps, at most 3"],
    bar: ["status_bar.something new"],
  });
  // None of them is a general or grid message, so none shows twice.
  assert.deepEqual(map.general, []);
  assert.deepEqual(map.grid, []);
  assert.equal(hasErrors(map), true);
  assert.equal(hasErrors(mapErrors([])), false);
});

test("a step's at that isn't a whole number is reported before the call", () => {
  const withAt = (at: number): Config => ({
    ...config,
    status_bar: {
      thresholds: {
        mem: [{ at: 80, color: "#ffb300" }],
        cpu_temp: [
          { at: 85, color: "#ffb300" },
          { at, color: "#e53935" },
        ],
      },
    },
  });
  assert.equal(hasErrors(numberErrors(withAt(95))), false);
  // Out of range is a rule, which Rust words.
  assert.equal(hasErrors(numberErrors(withAt(-5))), false);
  for (const bad of [1.5, Number.NaN, Number.POSITIVE_INFINITY, 2 ** 60]) {
    assert.deepEqual(numberErrors(withAt(bad)).statusBar, {
      "thresholds.cpu_temp": ["at must be a whole number"],
    });
  }
  assert.equal(hasErrors(numberErrors(config)), false);
});
