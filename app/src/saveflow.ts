// The decisions behind the Layout page's save, dirty and on-disk logic, apart from React.

import type { Config, ConfigStatus, ConfigView } from "./config.ts";
import { canon } from "./edit.ts";

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
  const grid = `${draft.layout.cols}x${draft.layout.rows} grid with ${draft.button.length} button${draft.button.length === 1 ? "" : "s"}`;
  const what = view.loaded
    ? `the last good config, a ${grid}`
    : `an empty ${grid}. The app has not loaded your file since it started, so this is not your file`;
  return `This writes ${what}, over config.toml. The text now in the file is kept beside it as config.toml.bak.<UTC time>.`;
}

export type PushOutcome =
  | { kind: "sent"; ms: number }
  | { kind: "waiting" }
  | { kind: "late" };

/** Whether `device` was sent the layout a save caused: `at` is when it was announced, if it was. */
export function pushOutcome(
  at: number | undefined,
  t0: number,
  late: boolean,
): PushOutcome {
  if (at !== undefined && at >= t0) {
    return { kind: "sent", ms: at - t0 };
  }
  return late ? { kind: "late" } : { kind: "waiting" };
}
