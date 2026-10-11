import type { ReactNode } from "react";

/** The save controls, and what the last save did. */
export default function SaveBar({
  dirty,
  repair,
  saving,
  onSave,
  onRevert,
  status,
}: {
  dirty: boolean;
  /** The file on disk is not live, so a save replaces it. */
  repair: boolean;
  saving: boolean;
  onSave: () => void;
  onRevert: () => void;
  status: ReactNode;
}) {
  return (
    <div className="save-bar">
      <div className="save-status" aria-live="polite">
        {saving ? (
          <span className="chip busy">Saving…</span>
        ) : dirty ? (
          <span className="chip stale">Unsaved changes</span>
        ) : (
          status
        )}
      </div>
      <span className="muted note">
        Saving rewrites config.toml without its comments.
      </span>
      <button type="button" disabled={!dirty || saving} onClick={onRevert}>
        Revert
      </button>
      <button
        type="button"
        className="primary"
        disabled={(!dirty && !repair) || saving}
        onClick={onSave}
      >
        {repair ? "Replace config.toml" : "Save"}
      </button>
    </div>
  );
}
