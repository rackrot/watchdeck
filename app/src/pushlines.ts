// The lines the Feed tab offers to copy for a push integration. Each is the form site/src/integrations.md
// documents, and tests/feed.test.ts holds them to its text. A `token` of null gives the document's
// placeholder (`$WATCHDECK_TOKEN`, `<the token>`); the tab passes the real one, which it shows once.

import { PUSH_LAN_PORT, PUSH_PORT } from "./config.ts";

/** What the LAN examples call this machine. The reader replaces it. */
export const LAN_HOST = "desktop.lan";

/** An id the shell takes unquoted: the characters a push integration's id may hold. */
export const SHELL_SAFE_ID = /^[A-Za-z0-9_-]+$/;

/** `text` as one single-quoted shell word. */
const shellQuote = (text: string): string => `'${text.replace(/'/g, `'\\''`)}'`;

/**
 * A `curl` line that posts a note, with the header on stdin (`-K -`), so the token is never on a
 * command line (`ps`, `/proc/<pid>/cmdline`): `printf` is built into the shell and the token is
 * only its argument. The loopback form is plain http; with `lan`, https to `LAN_HOST` with the
 * certificate pinned by `pin`, the base64 of its public key's hash.
 */
export function curlLine(opts: {
  lan: boolean;
  token: string | null;
  pin: string;
}): string {
  const token =
    opts.token === null ? '"$WATCHDECK_TOKEN"' : shellQuote(opts.token);
  const printf = `printf 'header = "Authorization: Bearer %s"\\n' ${token} |`;
  if (!opts.lan) {
    return `${printf}
  curl -sS -K - \\
    -H "Content-Type: application/json" \\
    -d '{"title": "Deploy finished", "level": "ok"}' \\
    http://127.0.0.1:${PUSH_PORT}/v1/notes`;
  }
  return `${printf}
  curl -sS -K - -k --pinnedpubkey "sha256//${opts.pin}" \\
    -H "Content-Type: application/json" \\
    -d '{"title": "From the LAN"}' \\
    https://${LAN_HOST}:${PUSH_LAN_PORT}/v1/notes`;
}

/**
 * The `watchdeck notify` line for integration `id`, or null if the id isn't one a shell takes
 * unquoted. It carries no token: the command reads `secrets.toml` in the config folder.
 */
export function cliLine(id: string): string | null {
  return SHELL_SAFE_ID.test(id)
    ? `watchdeck notify --integration ${id} --level ok "Backup done" "4 min, 12 GB"`
    : null;
}

/**
 * Home Assistant's two files: `secrets.yaml` with the bearer header, and the `rest_command` block
 * for `configuration.yaml`. Over the LAN it names `LAN_HOST` and sets `verify_ssl: false`, which
 * `rest_command` needs for a certificate it can't verify. Without `lan` it points at the loopback
 * port, for a Home Assistant on the same machine.
 */
export function homeAssistantBlock(opts: {
  lan: boolean;
  token: string | null;
}): string {
  const base = opts.lan
    ? `https://${LAN_HOST}:${PUSH_LAN_PORT}`
    : `http://127.0.0.1:${PUSH_PORT}`;
  const verify = opts.lan ? "\n    verify_ssl: false" : "";
  const bearer =
    opts.token === null ? "<the token>" : opts.token.replace(/[\\"]/g, "\\$&");
  return `# secrets.yaml
watchdeck_auth: "Bearer ${bearer}"

# configuration.yaml
rest_command:
  watchdeck_notify:
    url: "${base}/v1/notes"
    method: post${verify}
    headers:
      authorization: !secret watchdeck_auth
    content_type: "application/json"
    payload: >-
      {"title": {{ title | to_json }}, "body": {{ (message | default('')) | to_json }},
       "level": {{ (level | default('info')) | to_json }}}
  watchdeck_card:
    url: "${base}/v1/cards/{{ id | urlencode }}"
    method: put${verify}
    headers:
      authorization: !secret watchdeck_auth
    content_type: "application/json"
    payload: >-
      {"label": {{ label | to_json }}, "value": {{ value | string | to_json }},
       "state": {{ (state | default('ok')) | to_json }}}`;
}
