import assert from "node:assert/strict";
import { test } from "node:test";
import type { ButtonConfig, Config, ConfigView } from "../src/config.ts";
import {
  diskChanged,
  isDirty,
  ownStatus,
  pushOutcome,
  replaceNotice,
  shouldFollowDisk,
  testBlock,
} from "../src/saveflow.ts";

const config: Config = { version: 1, layout: { cols: 5, rows: 3 }, button: [] };
const view: ConfigView = { config, revision: "r1", errors: [], loaded: true };

test("the draft is dirty only when it differs from what was loaded", () => {
  assert.equal(isDirty(structuredClone(config), view), false);
  assert.equal(
    isDirty({ ...config, layout: { cols: 4, rows: 3 } }, view),
    true,
  );
});

test("the file has moved when its revision or its errors differ from what the page holds", () => {
  assert.equal(diskChanged(view, null), false);
  assert.equal(diskChanged(view, { revision: "r1", errors: [] }), false);
  assert.equal(diskChanged(view, { revision: "r2", errors: [] }), true);
  assert.equal(diskChanged(view, { revision: "r1", errors: ["bad"] }), true);
});

test("an event about our own save is no news, during the save and after it", () => {
  const event = { revision: "r2", errors: [] };
  assert.equal(ownStatus(event, { inFlight: true, revision: null }), true);
  assert.equal(ownStatus(event, { inFlight: false, revision: "r2" }), true);
  assert.equal(ownStatus(event, { inFlight: false, revision: "r1" }), false);
  assert.equal(ownStatus(event, { inFlight: false, revision: null }), false);
  // The same revision with errors is a different state of the file.
  assert.equal(
    ownStatus(
      { revision: "r2", errors: ["x"] },
      { inFlight: false, revision: "r2" },
    ),
    false,
  );
});

test("the page follows the file only with nothing to lose and no save running", () => {
  assert.equal(shouldFollowDisk(true, false, false), true);
  assert.equal(
    shouldFollowDisk(true, true, false),
    false,
    "edits would be lost",
  );
  assert.equal(shouldFollowDisk(true, false, true), false, "a save is running");
  assert.equal(shouldFollowDisk(false, false, false), false);
});

test("the replace notice says what is written and where the old text goes", () => {
  const empty = replaceNotice(
    { ...view, loaded: false, errors: ["bad"] },
    config,
  );
  assert.ok(empty.includes("an empty 5x3 grid with 0 buttons"), empty);
  assert.ok(empty.includes("not your file"), empty);
  assert.ok(empty.includes("config.toml.bak.<UTC time>"), empty);
  const good = replaceNotice({ ...view, errors: ["bad"] }, config);
  assert.ok(good.includes("last good config"), good);
});

test("a button can be tested only when the draft matches the saved one", () => {
  const button: ButtonConfig = {
    id: "a",
    pos: [0, 0],
    label: "A",
    bg: "#000000",
    fg: "#ffffff",
    action: { kind: "shell", argv: ["true"], timeout_s: 5 },
  };
  const saved: Config = { ...config, button: [button] };
  assert.equal(testBlock(structuredClone(button), saved, false), null);
  assert.ok(
    testBlock({ ...button, id: "new" }, saved, false)?.includes(
      "Save the button",
    ),
  );
  const edited = { ...button, action: { ...button.action, timeout_s: 6 } };
  assert.ok(testBlock(edited, saved, false)?.includes("Save your changes"));
  assert.ok(testBlock({ ...button, label: "B" }, saved, false));
  assert.ok(
    testBlock(structuredClone(button), saved, true)?.includes(
      "changed on disk",
    ),
    "the file moved, so the live action may differ from the form",
  );
});

test("a push is sent, waiting or late", () => {
  assert.deepEqual(pushOutcome(1300, 1000, false), { kind: "sent", ms: 300 });
  assert.deepEqual(
    pushOutcome(900, 1000, false),
    { kind: "waiting" },
    "before this save",
  );
  assert.deepEqual(pushOutcome(undefined, 1000, false), { kind: "waiting" });
  assert.deepEqual(pushOutcome(undefined, 1000, true), { kind: "late" });
});
