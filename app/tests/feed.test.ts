// Run with `pnpm test`. The feed page preview and the lines the Feed tab copies are held to the
// files they mirror: firmware/main/feedfmt.h, firmware/test/host_proto/main/test_feedfmt.c,
// firmware/main/feedpage.c and site/src/integrations.md. Each read counts what it found, so a
// reworded source can't turn a comparison into an empty loop that passes.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import {
  FEED_AGE_W,
  FEED_AREA_H,
  FEED_CARD_GAP,
  FEED_CARD_H,
  FEED_CARD_W,
  FEED_CARDS_PER_ROW,
  FEED_EDGE_W,
  FEED_META_GAP,
  FEED_MORE_H,
  FEED_NOTE_H,
  FEED_PAD,
  FEED_ROW_PAD,
  FEED_SIDE,
  FEED_SRC_W,
  feedAge,
  feedNoteRows,
  feedNotesTop,
  GRID_TOP,
  SCREEN_H,
} from "../src/preview.ts";
import { cliLine, curlLine, homeAssistantBlock } from "../src/pushlines.ts";

const read = (path: string) =>
  readFileSync(new URL(path, import.meta.url), "utf8");

const feedfmtH = read("../../firmware/main/feedfmt.h");
const feedpageC = read("../../firmware/main/feedpage.c");
const feedfmtTest = read("../../firmware/test/host_proto/main/test_feedfmt.c");
const integrationsMd = read("../../site/src/integrations.md");

/** `#define NAME <number>` in `source`, failing if there is none. */
function define(source: string, name: string): number {
  const found = source.match(new RegExp(`^#define ${name} (\\d+)`, "m"));
  if (!found) {
    throw new Error(`${name} is not defined as a number`);
  }
  return Number(found[1]);
}

test("the feed preview's constants are the firmware's", () => {
  assert.equal(define(feedfmtH, "FEED_AREA_H"), FEED_AREA_H);
  assert.equal(FEED_AREA_H, SCREEN_H - GRID_TOP);
  assert.equal(define(feedfmtH, "FEED_PAD"), FEED_PAD);
  assert.equal(define(feedfmtH, "FEED_CARD_H"), FEED_CARD_H);
  assert.equal(define(feedfmtH, "FEED_CARD_GAP"), FEED_CARD_GAP);
  assert.equal(define(feedfmtH, "FEED_CARDS_PER_ROW"), FEED_CARDS_PER_ROW);
  assert.equal(define(feedfmtH, "FEED_NOTE_H"), FEED_NOTE_H);
  assert.equal(define(feedfmtH, "FEED_MORE_H"), FEED_MORE_H);
  assert.equal(define(feedpageC, "SIDE"), FEED_SIDE);
  assert.equal(define(feedpageC, "CARD_W"), FEED_CARD_W);
  assert.equal(define(feedpageC, "EDGE_W"), FEED_EDGE_W);
  assert.equal(define(feedpageC, "ROW_PAD"), FEED_ROW_PAD);
  assert.equal(define(feedpageC, "AGE_W"), FEED_AGE_W);
  assert.equal(define(feedpageC, "SRC_W"), FEED_SRC_W);
  assert.equal(define(feedpageC, "META_GAP"), FEED_META_GAP);
});

test("the preview's row count matches every assert_rows case in test_feedfmt.c", () => {
  const cases = [
    ...feedfmtTest.matchAll(/assert_rows\((\d+), (\d+), (\d+), (\d+)\)/g),
  ];
  // The table has 11 cases today. A rewording that matches fewer must be noticed.
  assert.ok(cases.length >= 11, `found ${cases.length} assert_rows cases`);
  for (const [call, ...numbers] of cases) {
    const [cards, notes, rows, more] = numbers.map(Number);
    assert.deepEqual(feedNoteRows(cards, notes), { rows, more }, call);
  }
});

test("the fullest page, 8 cards and 12 notes, fits the area as test_feedfmt.c checks", () => {
  const { rows } = feedNoteRows(8, 12);
  const used =
    FEED_PAD +
    2 * FEED_CARD_H +
    FEED_CARD_GAP +
    FEED_PAD +
    rows * FEED_NOTE_H +
    FEED_MORE_H;
  assert.ok(used <= FEED_AREA_H, `${used} px`);
  assert.equal(
    feedNotesTop(8),
    FEED_PAD + 2 * FEED_CARD_H + FEED_CARD_GAP + FEED_PAD,
  );
  assert.equal(feedNotesTop(0), FEED_PAD);
  assert.equal(feedNotesTop(5), feedNotesTop(8));
  assert.equal(feedNotesTop(4), FEED_PAD + FEED_CARD_H + FEED_PAD);
  // The C file really holds that test, under the name this one cites.
  assert.ok(
    feedfmtTest.includes("test_feedfmt_the_fullest_page_fits_the_area"),
  );
});

/** The seconds in an expression of sums and products of whole numbers, such as `59 * 60 + 59`. */
function seconds(expression: string): number {
  const plain = expression.replace(/ull/g, "");
  assert.ok(/^[\d\s*+]+$/.test(plain), `unexpected expression ${expression}`);
  return plain
    .split("+")
    .map((sum) =>
      sum.split("*").reduce((product, n) => product * Number(n.trim()), 1),
    )
    .reduce((total, n) => total + n, 0);
}

test("the preview's age text matches every assert_age case in test_feedfmt.c", () => {
  const now = define(feedfmtTest, "NOW");
  const cases = [...feedfmtTest.matchAll(/assert_age\("([^"]+)", ([^;]+)\);/g)];
  assert.ok(cases.length >= 10, `found ${cases.length} assert_age cases`);
  for (const [call, want, ago] of cases) {
    assert.equal(feedAge(now - seconds(ago), now), want, call);
  }
  // The other cases there: no clock, no time, and a time in the future.
  assert.equal(feedAge(now - 300, now, false), "");
  assert.equal(feedAge(0, now), "");
  assert.equal(feedAge(now + 120, now), "now");
});

/** The fenced code blocks of the docs page, without their trailing newline. */
const blocks = [...integrationsMd.matchAll(/```\w*\n([\s\S]*?)```/g)].map((m) =>
  m[1].replace(/\n$/, ""),
);

/** The one block that has every piece of text in `needles`. */
function documented(...needles: string[]): string {
  const found = blocks.filter((b) => needles.every((n) => b.includes(n)));
  assert.equal(
    found.length,
    1,
    `${needles.join(" + ")}: ${found.length} blocks`,
  );
  return found[0];
}

test("the curl lines are the forms integrations.md documents", () => {
  assert.equal(
    curlLine({ lan: false, token: null, pin: "" }),
    documented("curl -sS -K -", "http://127.0.0.1:47822/v1/notes"),
  );
  assert.equal(
    curlLine({ lan: true, token: null, pin: "<the base64 from above>" }),
    documented("curl -sS -K -", "--pinnedpubkey"),
  );
});

test("the watchdeck line is the documented one, with the integration named", () => {
  const doc = documented("watchdeck notify --level ok");
  const first = doc.split("\n")[0];
  assert.equal(cliLine("ha")?.replace(" --integration ha", ""), first);
  assert.equal(cliLine("ha")?.includes("--integration ha"), true);
  // Not an id a shell word holds: no line, rather than one that could run something else.
  for (const id of ["", "a b", "a;b", "a.b", "$(x)", "a'b"]) {
    assert.equal(cliLine(id), null, JSON.stringify(id));
  }
});

test("the Home Assistant block is the documented secrets.yaml line and rest_command", () => {
  const secrets = documented('watchdeck_auth: "Bearer <the token>"');
  const rest = documented("rest_command:", "verify_ssl: false");
  assert.equal(
    homeAssistantBlock({ lan: true, token: null }),
    `# secrets.yaml\n${secrets}\n\n# configuration.yaml\n${rest}`,
  );

  // On the same machine it points at the loopback port, and drops verify_ssl, as the page says.
  const local = homeAssistantBlock({ lan: false, token: null });
  assert.equal(local.includes("verify_ssl"), false);
  assert.equal(local.includes("https://"), false);
  assert.ok(local.includes('url: "http://127.0.0.1:47822/v1/notes"'));
  assert.ok(
    local.includes(
      'url: "http://127.0.0.1:47822/v1/cards/{{ id | urlencode }}"',
    ),
  );
});

const TOKEN =
  "f00dfeedf00dfeedf00dfeedf00dfeedf00dfeedf00dfeedf00dfeedf00dfeed";

test("a copied curl line holds the token only as printf's argument, never in curl's argv", () => {
  for (const lan of [false, true]) {
    const line = curlLine({ lan, token: TOKEN, pin: "PIN=" });
    // The shell runs `printf ... TOKEN` itself (a builtin, so no process has it) and pipes it
    // to curl, whose command line starts after the pipe.
    const [printf, ...curl] = line.split(" |\n");
    assert.equal(curl.length, 1);
    assert.ok(printf.startsWith("printf 'header = "), printf);
    assert.ok(printf.includes(`'${TOKEN}'`), "the token is printf's argument");
    assert.equal(
      curl[0].includes(TOKEN),
      false,
      "the token is not in curl's argv",
    );
    assert.ok(
      curl[0].startsWith("  curl -sS -K - "),
      "the header comes from stdin",
    );
    assert.equal(curl[0].includes("Authorization"), false);
    assert.equal(curl[0].includes("Bearer"), false);
  }
  assert.ok(
    curlLine({ lan: true, token: TOKEN, pin: "PIN=" }).includes(
      '--pinnedpubkey "sha256//PIN="',
    ),
  );
  // Without the token the line is the documented one, which reads the variable.
  assert.ok(
    curlLine({ lan: false, token: null, pin: "" }).includes(
      '"$WATCHDECK_TOKEN"',
    ),
  );
  // The watchdeck command never takes it from the line at all.
  assert.equal(cliLine("ha")?.includes(TOKEN), false);
});

test("a token that is not plain hex is still one shell word, and one YAML string", () => {
  const odd = `a'b"c\\d $x`;
  const line = curlLine({ lan: false, token: odd, pin: "" });
  assert.ok(line.includes(`'a'\\''b"c\\d $x'`), line);
  const yaml = homeAssistantBlock({ lan: false, token: odd });
  assert.ok(yaml.includes(`watchdeck_auth: "Bearer a'b\\"c\\\\d $x"`), yaml);
});

test("the Home Assistant block holds the token in secrets.yaml only", () => {
  const block = homeAssistantBlock({ lan: true, token: TOKEN });
  assert.equal(block.split(TOKEN).length - 1, 1);
  assert.ok(block.includes(`watchdeck_auth: "Bearer ${TOKEN}"`));
});
