// The decisions behind the Layout page's save, dirty and on-disk logic, apart from React.

import type {
  ButtonConfig,
  Config,
  ConfigStatus,
  ConfigView,
} from "./config.ts";
import { canon, pageCount } from "./edit.ts";

/** Whether the draft differs from what was loaded or last saved. */
export const isDirty = (draft: Config, view: ConfigView): boolean =>
  canon(draft) !== canon(view.config);

/** Whether the file on disk is not the one the page is showing. */
export const diskChanged = (
  view: ConfigView,
  onDisk: ConfigStatus | null,
): boolean =>
  onDisk !== null &&
  (onDisk.revision !== view.revision ||
    canon(onDisk.errors) !== canon(view.errors));

/**
 * Whether a `config-status` event is about our own save, or one still to come, and so is no
 * news. During a save every event is ours. After it, the event for the revision just written is.
 */
export function ownStatus(
  event: ConfigStatus,
  save: { inFlight: boolean; revision: string | null },
): boolean {
  return (
    save.inFlight ||
    (save.revision !== null &&
      event.revision === save.revision &&
      event.errors.length === 0)
  );
}

/** Whether the page should reload from disk by itself: the file moved, and there are no edits to lose. */
export const shouldFollowDisk = (
  changed: boolean,
  dirty: boolean,
  saving: boolean,
): boolean => changed && !dirty && !saving;

/** What a save needs confirming first: it replaces a file that isn't live. */
export function replaceNotice(view: ConfigView, draft: Config): string {
  const pages = pageCount(draft);
  const onPages = pages >= 2 ? ` on ${pages} pages` : "";
  const grid = `${draft.layout.cols}x${draft.layout.rows} grid with ${draft.button.length} button${draft.button.length === 1 ? "" : "s"}${onPages}`;
  const what = view.loaded
    ? `the last good config, a ${grid}`
    : `an empty ${grid}. The app has not loaded your file since it started, so this is not your file`;
  return `This writes ${what}, over config.toml. The text now in the file is kept beside it as config.toml.bak.<UTC time>.`;
}

/**
 * Why `button` can't be tested yet, or null if it can. A test runs the saved button, so the draft
 * must match what `saved` (the live config) has under the same id, and the file on disk must not
 * have moved on, since the live config would then differ from what the form was built on.
 */
export function testBlock(
  button: ButtonConfig,
  saved: Config,
  diskMoved: boolean,
): string | null {
  if (diskMoved) {
    return "config.toml changed on disk; reload first.";
  }
  const live = saved.button.find((b) => b.id === button.id);
  if (!live) {
    return "Save the button first. A test runs the saved action.";
  }
  return canon(live) === canon(button)
    ? null
    : "Save your changes first. A test runs the saved action.";
}

export type PushOutcome =
  | { kind: "applied"; ms: number }
  | { kind: "sent"; ms: number }
  | { kind: "waiting" }
  | { kind: "late" };

/** A layout event for a device: when the window heard it, and the layout's rev. */
export type LayoutStamp = { at: number; rev: number };

/**
 * Whether `device` was sent the layout a save caused, and whether it said it applied it. A board
 * from before the ack never does, and stays "sent". An ack counts only if it came after this
 * push and names this rev or a later one, so an earlier session's or save's ack can't claim it.
 */
export function pushOutcome(
  pushed: LayoutStamp | undefined,
  applied: LayoutStamp | undefined,
  t0: number,
  late: boolean,
): PushOutcome {
  if (pushed !== undefined && pushed.at >= t0) {
    const ms = pushed.at - t0;
    const acked =
      applied !== undefined &&
      applied.at >= pushed.at &&
      applied.rev >= pushed.rev;
    return { kind: acked ? "applied" : "sent", ms };
  }
  return late ? { kind: "late" } : { kind: "waiting" };
}
