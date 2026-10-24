// Run with `pnpm test`. The Feed tab's prometheus and grafana forms, and the token rules shared by
// the kinds that take one. Messages are Rust's own wording from daemon/config.rs, and the parts that
// mirror Rust (`is_card`, the contact point values) are held to config.rs and
// site/src/integrations.md. Each read counts what it found, so a reworded source can't turn a
// comparison into an empty loop that passes.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { test } from "node:test";
import {
  type Config,
  type IntegrationConfig,
  isCard,
  kindIsCard,
  MAX_CARD_SOURCES,
} from "../src/config.ts";
import {
  addIntegration,
  caseDuplicateNames,
  integrationIdProblem,
  parseDecimal,
  withIntegrationId,
  withOptional,
} from "../src/edit.ts";
import { integrationFields, mapErrors, numberErrors } from "../src/errors.ts";
import { grafanaContactPoint } from "../src/pushlines.ts";
import { tokenState } from "../src/tokenstate.ts";

const read = (path: string) =>
  readFileSync(new URL(path, import.meta.url), "utf8");

const configRs = read("../src-tauri/src/daemon/config.rs");
const integrationsMd = read("../../site/src/integrations.md");

const base: Config = {
  version: 1,
  layout: { cols: 5, rows: 3 },
  button: [],
};

type Prom = Extract<IntegrationConfig, { kind: "prometheus" }>;

const prometheus = (extra: Partial<IntegrationConfig> = {}): Config => ({
  ...base,
  integration: [
    {
      kind: "prometheus",
      id: "cpu",
      label: "CPU",
      url: "http://prom.lan:9090",
      query: "up",
      every_s: 30,
      ...extra,
    } as IntegrationConfig,
  ],
});

// ---- Kinds ----

test("isCard is true for stat and prometheus and false for push and grafana, as is_card in config.rs", () => {
  const all: IntegrationConfig["kind"][] = [
    "push",
    "stat",
    "prometheus",
    "grafana",
  ];
  const mine = Object.fromEntries(all.map((k) => [k, kindIsCard(k)]));
  assert.deepEqual(mine, {
    push: false,
    stat: true,
    prometheus: true,
    grafana: false,
  });

  // Rust's arms: `Self::A { .. } | Self::B { .. } => true`.
  const body = configRs.match(
    /pub fn is_card\(&self\) -> bool \{\s*match self \{([\s\S]*?)\n {8}\}/,
  )?.[1];
  if (!body) {
    throw new Error("is_card is no longer a match");
  }
  const arms = [
    ...body.matchAll(/((?:Self::\w+ \{ \.\. \}(?: \| )?)+) => (true|false)/g),
  ];
  assert.equal(arms.length, 2, "is_card has two arms");
  const rust: Record<string, boolean> = {};
  for (const [, kinds, value] of arms) {
    for (const [, name] of kinds.matchAll(/Self::(\w+)/g)) {
      rust[name.toLowerCase()] = value === "true";
    }
  }
  assert.deepEqual(rust, mine);

  for (const kind of all) {
    const c = addIntegration(base, kind);
    assert.equal(isCard((c.integration ?? [])[0]), kindIsCard(kind), kind);
  }
  assert.equal(MAX_CARD_SOURCES, 8);
  assert.ok(/const MAX_CARD_SOURCES: usize = 8;/.test(configRs));
});

test("a new prometheus or grafana integration starts valid in shape, with no optional key", () => {
  const p = addIntegration(base, "prometheus").integration?.[0];
  assert.deepEqual(p, {
    kind: "prometheus",
    id: "prometheus-1",
    label: "Prometheus",
    url: "http://localhost:9090",
    query: "up",
    every_s: 30,
  });
  const g = addIntegration(base, "grafana").integration?.[0];
  assert.deepEqual(g, {
    kind: "grafana",
    id: "grafana-1",
    label: "Grafana",
    token_secret: "push-grafana-1",
  });
});

test("a grafana secret follows its id while conventional, and its id may not hold a dot", () => {
  const c = addIntegration(base, "grafana");
  const renamed = withIntegrationId(c, 0, "graf").integration?.[0];
  assert.equal(
    renamed?.kind === "grafana" && renamed.token_secret,
    "push-graf",
  );
  assert.ok(
    /grafana integration's id can't hold '\.'/.test(
      integrationIdProblem(withIntegrationId(c, 0, "a.b"), 0) ?? "",
    ),
  );
  // A prometheus id may hold one, since it stores no secret.
  const p = addIntegration(base, "prometheus");
  assert.equal(integrationIdProblem(withIntegrationId(p, 0, "a.b"), 0), null);
});

test("timeout_s and the other optional keys are left out when unset", () => {
  const p = prometheus().integration?.[0] as Prom;
  assert.equal("timeout_s" in p, false);

  const set = withOptional(p, "timeout_s", 5);
  assert.equal("timeout_s" in set && set.timeout_s, 5);
  const cleared = withOptional(set, "timeout_s", undefined);
  assert.equal("timeout_s" in cleared, false);
  assert.equal(JSON.stringify(cleared), JSON.stringify(p));

  // Empty text clears a suffix, and undefined an unchecked box.
  const suffixed = withOptional(p, "suffix", "%");
  assert.equal("suffix" in withOptional(suffixed, "suffix", ""), false);
  // Zero is a value: `decimals = 0` is not the same as no decimals.
  const zero = withOptional(p, "decimals", 0);
  assert.equal("decimals" in zero && zero.decimals, 0);
});

test("a decimal is a number as typed, and anything else is NaN", () => {
  assert.equal(parseDecimal("90"), 90);
  assert.equal(parseDecimal(" 0.5 "), 0.5);
  assert.equal(parseDecimal("-3.25"), -3.25);
  for (const bad of ["", "1e3", "1.", ".5", "abc", "1,5", "--1"]) {
    assert.ok(Number.isNaN(parseDecimal(bad)), bad);
  }
});

test("severities that differ only in case are duplicates, reported once in the first spelling", () => {
  const rows = (...names: string[]) =>
    names.map((name) => ({ name, value: "info" }));
  assert.deepEqual(caseDuplicateNames(rows("page", "Page", "PAGE", "other")), [
    "page",
  ]);
  assert.deepEqual(caseDuplicateNames(rows("a", "b")), []);
  assert.deepEqual(caseDuplicateNames(rows("", "")), [""]);
});

// ---- Errors ----

test("a prometheus integration's messages land on the field they name", () => {
  const route = (message: string) =>
    Object.keys(integrationFields([message]))[0];
  const cases: [string, string][] = [
    ["every_s is 1, expected 2 to 86400", "every_s"],
    [
      "timeout_s is over 10, the lesser of 30 and every_s: a query must end before the next one starts",
      "timeout_s",
    ],
    ["timeout_s is 0, expected 1 to 600", "timeout_s"],
    ["query must be 1 to 2000 bytes of text, with no NUL", "query"],
    ["decimals is over 3", "decimals"],
    ["suffix: at most 7 printable ASCII characters", "suffix"],
    ["url is the server's base, with no query or fragment", "url"],
    ["url does not parse: relative URL without a base", "url"],
    ["url is not http:// or https://", "url"],
    ["url has no host", "url"],
    [
      "secret_headers would cross the network in the clear over http://; use https://, or set insecure_http = true",
      "insecure_http",
    ],
    ["insecure_tls only applies to https:// urls", "insecure_tls"],
    ['"bad name" is not a valid header name', "secret_headers"],
    [
      'header "Authorization" uses secret "nope", which is not in secrets.toml',
      "secret_headers",
    ],
    ['header "X-A" is set twice; names ignore case', "secret_headers"],
    ["id must be 1 to 31 letters, digits, '_', '-' or '.'", "id"],
    ["label: at most 23 printable ASCII characters", "label"],
    ["something new", "integration"],
  ];
  for (const [message, field] of cases) {
    assert.equal(route(message), field, message);
  }
  // The same wording through mapErrors, prefixed with the id as Rust does.
  const map = mapErrors(
    [`integration "cpu": decimals is over 3`],
    [],
    [],
    ["cpu"],
  );
  assert.deepEqual(integrationFields(map.integrations.get("cpu") ?? []), {
    decimals: ["decimals is over 3"],
  });
});

test("a stat source's action messages stay under source, apart from prometheus's own", () => {
  assert.deepEqual(integrationFields(["source: url does not parse: x"]), {
    "source.url": ["source: url does not parse: x"],
  });
});

test("a grafana integration's messages land on token and levels", () => {
  const fields = integrationFields([
    'token_secret "push-graf" is not in secrets.toml',
    'token "push-graf" is shorter than 32 characters',
    'levels: "Page" is listed twice; severities ignore case',
    "levels: a severity is 1 to 31 printable ASCII characters",
  ]);
  assert.equal(fields.token?.length, 2);
  assert.equal(fields.levels?.length, 2);
  assert.equal(fields.integration, undefined);
});

test("every prometheus message in config.rs reaches a field, none falling to the integration", () => {
  // The messages Rust writes with `problems.push(format!(` for a prometheus integration, by their
  // first words. A reworded one that no rule routes would show up under the card as a general error.
  const starts = [
    ...configRs.matchAll(
      /problems\.push\(format!\(\s*"((?:timeout_s|query|decimals|suffix|url)[^"{]*)/g,
    ),
  ].map((m) => m[1]);
  assert.ok(starts.length >= 4, `found ${starts.length} messages`);
  for (const start of starts) {
    assert.ok(
      Object.keys(integrationFields([start]))[0] !== "integration",
      start,
    );
  }
});

test("prometheus numbers that can't be sent are reported with the integration's id", () => {
  const bad = prometheus({
    every_s: Number.NaN,
    timeout_s: Number.NaN,
    decimals: -1,
    bad_above: Number.NaN,
    bad_below: Number.POSITIVE_INFINITY,
  });
  assert.deepEqual(numberErrors(bad).integrations.get("cpu"), [
    "every_s must be a whole number",
    "timeout_s must be a whole number",
    "decimals must be a whole number",
    "bad_above must be a number",
    "bad_below must be a number",
  ]);
  // Unset optional numbers are fine, and so is zero.
  assert.equal(numberErrors(prometheus()).integrations.size, 0);
  assert.equal(
    numberErrors(prometheus({ decimals: 0, bad_below: -2.5 })).integrations
      .size,
    0,
  );
});

test("the preview colors a card by its state as the board does: unknown grey, ok blue-grey, bad red", () => {
  const feedpage = read("../../firmware/main/feedpage.c");
  const states = feedpage.match(
    /state_bg\[3\] = \{lv_palette_darken\(LV_PALETTE_GREY, 2\), lv_palette_darken\(LV_PALETTE_BLUE_GREY, 3\),\s*lv_palette_darken\(LV_PALETTE_RED, 2\)\}/,
  );
  assert.ok(states, "state_bg is no longer unknown, ok, bad");
  // Material's palette values for those three, which LVGL's palette is.
  const theme = read("../src/theme.css");
  assert.ok(/--dev-tile-unknown: #616161;/.test(theme));
  assert.ok(/--dev-card-ok: #37474f;/.test(theme));
  assert.ok(/--dev-tile-bad: #d32f2f;/.test(theme));
  const css = read("../src/App.css");
  assert.ok(/\.feed-card \{[^}]*var\(--dev-tile-unknown\)/.test(css));
  assert.ok(/\.feed-card\.ok \{[^}]*var\(--dev-card-ok\)/.test(css));
  assert.ok(/\.feed-card\.bad \{[^}]*var\(--dev-tile-bad\)/.test(css));
});

// ---- Grafana's contact point ----

test("the grafana contact point values are the ones integrations.md documents", () => {
  const section = integrationsMd.slice(
    integrationsMd.indexOf("### The contact point"),
  );
  assert.ok(section.length > 100, "the contact point section moved");
  const rows = [...section.matchAll(/^\| ([^|]+) \| ([^|]+) \|$/gm)].filter(
    ([, setting]) => setting !== "Setting" && !setting.startsWith("---"),
  );
  assert.equal(rows.length, 4, "the table has four settings");

  const local = grafanaContactPoint({ lan: false, token: null });
  const lan = grafanaContactPoint({ lan: true, token: null });
  assert.deepEqual(
    local.map((r) => r.setting),
    rows.map(([, setting]) => setting.trim()),
  );

  const doc = (setting: string) =>
    rows.find(([, s]) => s.trim() === setting)?.[2] ?? "";
  const value = (list: typeof local, setting: string) =>
    list.find((r) => r.setting === setting)?.value ?? "";

  // The URL cell names both forms in backticks, the loopback one and the LAN one.
  const urls = [...doc("URL").matchAll(/`([^`]+)`/g)].map((m) => m[1]);
  assert.ok(urls.includes(value(local, "URL")), value(local, "URL"));
  assert.ok(urls.includes(value(lan, "URL")), value(lan, "URL"));
  assert.equal(value(local, "URL"), "http://127.0.0.1:47822/v1/grafana");
  assert.equal(value(lan, "URL"), "https://desktop.lan:47823/v1/grafana");
  assert.ok(/^`POST`/.test(doc("HTTP method")));
  assert.equal(value(local, "HTTP method"), "POST");
  assert.ok(/^`Bearer`/.test(doc("Authorization header: scheme")));
  assert.equal(value(local, "Authorization header: scheme"), "Bearer");
  assert.ok(/^the token/.test(doc("Authorization header: credentials")));
});

test("the contact point holds the real token only when given one", () => {
  const without = grafanaContactPoint({ lan: false, token: null });
  assert.equal(
    without.find((r) => r.setting.endsWith("credentials"))?.value,
    "<the token>",
  );
  const token = "ab".repeat(32);
  const withToken = grafanaContactPoint({ lan: false, token });
  assert.equal(
    withToken.find((r) => r.setting.endsWith("credentials"))?.value,
    token,
  );
  assert.equal(JSON.stringify(without).includes(token), false);
});

// ---- The token's state ----

test("a new integration does not show token set because an unrelated secret has its name", () => {
  const state = tokenState({
    id: "push-2",
    named: "push-push-2",
    secrets: ["push-push-2"],
    claimed: false,
    made: null,
  });
  assert.equal(state.status, "exists");
  assert.equal(state.needsConfirm, true);
});

test("the integration's own saved secret is set, as is one Generate made here", () => {
  const args = { id: "ha", named: "push-ha", secrets: ["push-ha"] };
  assert.equal(
    tokenState({ ...args, claimed: true, made: null }).status,
    "set",
  );
  assert.equal(
    tokenState({ ...args, claimed: false, made: "push-ha" }).status,
    "set",
  );
  // A secret Generate made for another name does not vouch for this one.
  assert.equal(
    tokenState({ ...args, claimed: false, made: "push-old" }).status,
    "exists",
  );
  assert.equal(
    tokenState({
      id: "ha",
      named: "push-ha",
      secrets: [],
      claimed: true,
      made: null,
    }).status,
    "none",
  );
});

test("a token_secret of its own is reported as set, and Generate asks before re-pointing it", () => {
  const own = tokenState({
    id: "ha",
    named: "shared",
    secrets: ["shared"],
    claimed: true,
    made: null,
  });
  assert.equal(own.status, "set");
  assert.equal(own.pointsElsewhere, true);
  assert.equal(own.needsConfirm, true);
  assert.equal(own.conventional, "push-ha");

  // A secret that is not there is "none", and the conventional name being free needs no question
  // unless the integration points elsewhere.
  const missing = tokenState({
    id: "ha",
    named: "push-ha",
    secrets: ["other"],
    claimed: false,
    made: null,
  });
  assert.deepEqual(
    [missing.status, missing.pointsElsewhere, missing.needsConfirm],
    ["none", false, false],
  );
  const blank = tokenState({
    id: "ha",
    named: "",
    secrets: [],
    claimed: false,
    made: null,
  });
  assert.deepEqual([blank.status, blank.needsConfirm], ["none", false]);
});
