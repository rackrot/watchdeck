// What the Feed tab may say about a token kind's secret, and what Generate must ask first. It is
// kept apart from the form so tests/feedkinds.test.ts can hold it.

import { tokenSecretFor } from "./config.ts";

export type TokenState = {
  /** The secret Generate stores: `push-<id>`, whatever the integration names now. */
  conventional: string;
  /** The integration names a secret of its own, which Generate would leave behind. */
  pointsElsewhere: boolean;
  /** `set`: the secret exists and is this integration's. `exists`: it exists, but only by name. */
  status: "none" | "exists" | "set";
  /** Generate must be confirmed, because it replaces or re-points something. */
  needsConfirm: boolean;
};

/**
 * `named` is the integration's `token_secret`. A secret that exists counts as the integration's
 * token only when the saved config already gives it that secret (`claimed`), or when Generate made
 * it here (`made`). One that merely has the name, such as `push-push-2` for a new integration, may
 * be anything, so it is shown as existing and Generate asks before replacing it.
 */
export function tokenState(opts: {
  id: string;
  named: string;
  secrets: string[];
  claimed: boolean;
  made: string | null;
}): TokenState {
  const conventional = tokenSecretFor(opts.id);
  const pointsElsewhere = opts.named !== "" && opts.named !== conventional;
  const exists = opts.secrets.includes(opts.named);
  const known = exists && (opts.claimed || opts.made === opts.named);
  return {
    conventional,
    pointsElsewhere,
    status: known ? "set" : exists ? "exists" : "none",
    needsConfirm: pointsElsewhere || opts.secrets.includes(conventional),
  };
}
