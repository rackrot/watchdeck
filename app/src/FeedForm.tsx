import { type ReactNode, useEffect, useRef, useState } from "react";
import ActionForm, { SecretHeaderRows } from "./ActionForm";
import { createPushToken, pushPin, testIntegration } from "./api";
import {
  type Config,
  type IntegrationConfig,
  isCard,
  kindIsCard,
  MAX_CARD_SOURCES,
  MAX_INTEGRATION_LABEL,
  MAX_INTEGRATIONS,
  NOTE_LEVELS,
  type NoteLevel,
  tokenSecretFor,
} from "./config";
import {
  addIntegration,
  caseDuplicateNames,
  duplicateNames,
  integrationIdProblem,
  moveIntegration,
  type Row,
  recordToRows,
  removeIntegration,
  rowsToRecord,
  setIntegration,
  withIntegrationId,
  withOptional,
  withPushLan,
} from "./edit";
import { type IntegrationErrors, integrationFields } from "./errors";
import FeedPreview from "./FeedPreview";
import {
  Field,
  NumberField,
  OptionalNumber,
  Problems,
  TestOutcome,
  type TestState,
} from "./fields";
import {
  cliLine,
  curlLine,
  grafanaContactPoint,
  homeAssistantBlock,
} from "./pushlines";
import { integrationTestBlock } from "./saveflow";
import { tokenState } from "./tokenstate";

type Kind = IntegrationConfig["kind"];

/**
 * The kinds the "Add" buttons make. A new kind is: its variant in `IntegrationConfig` (config.ts),
 * and then the compiler names the exhaustive places it missed: `newIntegration` (edit.ts),
 * `kindIsCard` (config.ts) and `KindFields` below. This list is not one of them: add one entry here
 * by hand, or the kind has no button.
 */
const ADD_KINDS: { kind: Kind; label: string; hint: string }[] = [
  {
    kind: "push",
    label: "Add push source",
    hint: "scripts and Home Assistant post notes and cards to it",
  },
  {
    kind: "stat",
    label: "Add stat card",
    hint: "a value polled from a command or a URL",
  },
  {
    kind: "prometheus",
    label: "Add Prometheus card",
    hint: "the first value of an instant query, polled",
  },
  {
    kind: "grafana",
    label: "Add Grafana alerts",
    hint: "a webhook contact point posts its alerts as notes",
  },
];

type Props = {
  /** The Layout panel is the one showing. Held tokens go when it is not. */
  active: boolean;
  /** The draft. */
  config: Config;
  /** What is saved and live, which Test runs. */
  saved: Config;
  /** The file on disk has moved on from the page, so a Test would not run what the form shows. */
  diskMoved: boolean;
  /** Rust's complaints from the last save, by integration id. */
  errors: Map<string, string[]>;
  /** The names `list_secrets` returned. */
  secrets: string[];
  onChange: (config: Config) => void;
  /** Problems the forms see before any call, which keep a save from going out. */
  onLocalProblems: (messages: string[]) => void;
  /** A token was stored: the secret names changed. */
  onSecretsChanged: () => void;
};

let nextKey = 0;
const newKey = () => `integration-${nextKey++}`;

/** A button that puts `text` on the clipboard, and says whether it did. */
function CopyButton({ text, label }: { text: string; label: string }) {
  const [state, setState] = useState<"idle" | "copied" | "failed">("idle");
  return (
    <button
      type="button"
      onClick={async () => {
        try {
          await navigator.clipboard.writeText(text);
          setState("copied");
        } catch {
          // The text is on the page to select by hand.
          setState("failed");
        }
      }}
    >
      {state === "copied"
        ? "Copied"
        : state === "failed"
          ? "Select it by hand"
          : label}
    </button>
  );
}

/** Text to copy, in a box, with its button. */
function CopyBlock({
  title,
  text,
  note,
}: {
  title: string;
  text: string;
  note?: string;
}) {
  return (
    <div className="copy-block">
      <div className="outside">
        <h5>{title}</h5>
        <CopyButton text={text} label="Copy" />
      </div>
      <pre className="mono">{text}</pre>
      {note && <p className="muted hint">{note}</p>}
    </div>
  );
}

type PushIntegration = Extract<IntegrationConfig, { kind: "push" }>;
type StatIntegration = Extract<IntegrationConfig, { kind: "stat" }>;
type PrometheusIntegration = Extract<IntegrationConfig, { kind: "prometheus" }>;
type GrafanaIntegration = Extract<IntegrationConfig, { kind: "grafana" }>;
/** The kinds whose senders present a token. */
type TokenIntegration = PushIntegration | GrafanaIntegration;

/** Whether the saved config gives integration `item` the secret it names now, so it is in use. */
function savedNamesSecret(saved: Config, item: TokenIntegration): boolean {
  return (saved.integration ?? []).some(
    (s) =>
      s.id === item.id &&
      (s.kind === "push" || s.kind === "grafana") &&
      s.token_secret === item.token_secret,
  );
}

/**
 * A token kind's token: whether its secret is set, Generate, and what Generate shows once. The token
 * is held here, in this component's state alone, from Generate until the id changes, the panel is
 * hidden, the tab is left, or Done is pressed. It is never stored by the page, and
 * `create_push_token` is the only way it reaches the window. `children` draws what uses the token.
 */
function TokenFields({
  integration,
  idProblem,
  errors,
  secrets,
  claimed,
  active,
  onChange,
  onSecretsChanged,
  children,
}: {
  integration: TokenIntegration;
  idProblem: string | null;
  errors: IntegrationErrors;
  secrets: string[];
  /** The saved config already gives this integration the secret it names. */
  claimed: boolean;
  active: boolean;
  onChange: (next: TokenIntegration) => void;
  onSecretsChanged: () => void;
  children: (token: string | null) => ReactNode;
}) {
  // The id the token was made for travels with it, so editing the id drops it.
  const [held, setHeld] = useState<{ id: string; token: string } | null>(null);
  // The secret Generate stored from here, which is the one it made and so one that is in use.
  const [made, setMade] = useState<string | null>(null);
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");

  const named = integration.token_secret;
  const { conventional, pointsElsewhere, status, needsConfirm } = tokenState({
    id: integration.id,
    named,
    secrets,
    claimed,
    made,
  });
  const known = status === "set";
  const exists = status !== "none";
  const token = active && held?.id === integration.id ? held.token : null;

  useEffect(() => {
    if (held && (!active || held.id !== integration.id)) {
      setHeld(null);
    }
  }, [held, active, integration.id]);

  async function generate() {
    setConfirming(false);
    setBusy(true);
    setError("");
    const { id } = integration;
    try {
      const fresh = await createPushToken(id);
      setHeld({ id, token: fresh });
      setMade(tokenSecretFor(id));
      onChange({ ...integration, token_secret: tokenSecretFor(id) });
      onSecretsChanged();
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }

  const label = pointsElsewhere
    ? "Generate a new token"
    : !needsConfirm
      ? "Generate token"
      : known
        ? "Regenerate token"
        : "Replace the existing secret";

  return (
    <>
      <div className="field wide">
        <span className="field-label">
          Token
          <span className="field-hint">
            stored as the secret {named || conventional}
          </span>
        </span>
        <div className="outside">
          {known ? (
            <span className="chip ok">token set</span>
          ) : exists ? (
            <span className="chip stale">a secret by this name exists</span>
          ) : (
            <span className="chip stale">no token yet</span>
          )}
          <button
            type="button"
            disabled={busy || idProblem !== null}
            onClick={() =>
              needsConfirm ? setConfirming(true) : void generate()
            }
          >
            {busy ? "Generating…" : label}
          </button>
          {idProblem && <span className="muted hint">Fix the id first.</span>}
        </div>
        {exists && !known && (
          <p className="muted hint">
            No saved integration uses it yet. Saving uses it as this one's
            token, as it is. Generate replaces its value.
          </p>
        )}
        <Problems list={errors.token} />
        {error && (
          <p className="problems" role="alert">
            {error}
          </p>
        )}
      </div>

      {confirming && (
        <div
          className="banner stale"
          role="alertdialog"
          aria-label="Confirm replacing the token"
        >
          <span>
            {pointsElsewhere ? (
              <>
                This integration uses the secret{" "}
                <span className="mono">{named}</span>. Generating stores a new
                token as <span className="mono">{conventional}</span> and
                re-points the integration at it.{" "}
                <span className="mono">{named}</span> itself is left as it is.
                {secrets.includes(conventional) &&
                  " The secret already called that is replaced."}
              </>
            ) : known ? (
              <>
                Regenerating replaces the token stored as{" "}
                <span className="mono">{conventional}</span>, at once and
                whether or not you save. Whatever still sends the old one gets a
                401 until it is given the new one.
              </>
            ) : (
              <>
                A secret named <span className="mono">{conventional}</span>{" "}
                exists, and nothing saved uses it. Generating replaces its
                value, at once and whether or not you save.
              </>
            )}
          </span>
          <button
            type="button"
            className="danger"
            onClick={() => void generate()}
          >
            {pointsElsewhere ? "Re-point and generate" : "Replace token"}
          </button>
          <button type="button" onClick={() => setConfirming(false)}>
            Cancel
          </button>
        </div>
      )}

      {token !== null && (
        <div className="token-once" role="status">
          <div className="outside">
            <h5>Your token</h5>
            <CopyButton text={token} label="Copy token" />
            <button type="button" onClick={() => setHeld(null)}>
              Done
            </button>
          </div>
          <code className="mono token">{token}</code>
          <p className="warning" role="note">
            This is the only time it is shown. It is stored in secrets.toml,
            which the app never reads back, so copy it now. The values below
            hold it too, and go when you leave this tab, switch to another one,
            or press Done.
          </p>
        </div>
      )}

      {children(token)}
    </>
  );
}

/** A push integration: its token, and the lines that use it. */
function PushFields({
  integration,
  idProblem,
  errors,
  secrets,
  claimed,
  active,
  lan,
  pin,
  pinError,
  onChange,
  onSecretsChanged,
}: {
  integration: PushIntegration;
  idProblem: string | null;
  errors: IntegrationErrors;
  secrets: string[];
  claimed: boolean;
  active: boolean;
  lan: boolean;
  pin: string | null;
  pinError: string;
  onChange: (next: PushIntegration) => void;
  onSecretsChanged: () => void;
}) {
  const pinText = pin ?? "<the pin>";
  const cli = cliLine(integration.id);
  return (
    <TokenFields
      integration={integration}
      idProblem={idProblem}
      errors={errors}
      secrets={secrets}
      claimed={claimed}
      active={active}
      onChange={(next) => onChange(next as PushIntegration)}
      onSecretsChanged={onSecretsChanged}
    >
      {(token) => (
        <>
          <CopyBlock
            title={lan ? "curl, from another host" : "curl, on this machine"}
            text={curlLine({ lan, token, pin: pinText })}
            note={
              token === null
                ? "Set WATCHDECK_TOKEN to the token first. The header goes to curl on stdin, so the token is never on a command line."
                : "The token is only printf's argument, and printf is built into the shell, so it stays out of ps. Your shell's history keeps what you type."
            }
          />
          {cli && (
            <CopyBlock
              title="watchdeck command, on this machine"
              text={cli}
              note="Reads the token from the config folder, so no token is on the line."
            />
          )}
          <CopyBlock
            title="Home Assistant rest_command"
            text={homeAssistantBlock({ lan, token })}
            note={
              lan
                ? "Replace desktop.lan with this machine's name. rest_command can't pin a key, so it needs verify_ssl: false: use a LAN you trust."
                : "For a Home Assistant on this machine. From another host, turn on the LAN listener below."
            }
          />
          {pinError && lan && (
            <p className="problems" role="alert">
              {pinError}
            </p>
          )}
        </>
      )}
    </TokenFields>
  );
}

/** A Test button for integration `id`, and what its last run got. */
function TestRow({
  id,
  testBlock,
}: {
  id: string;
  /** Why Test is disabled, or null if it can run. */
  testBlock: string | null;
}) {
  const [testing, setTesting] = useState(false);
  const [test, setTest] = useState<TestState>(null);
  async function runTest() {
    setTesting(true);
    setTest(null);
    try {
      setTest({ result: await testIntegration(id) });
    } catch (e) {
      setTest({ error: String(e) });
    } finally {
      setTesting(false);
    }
  }
  return (
    <>
      <div className="outside">
        <button
          type="button"
          disabled={testBlock !== null || testing}
          onClick={() => void runTest()}
        >
          {testing ? "Testing…" : "Test"}
        </button>
        {testBlock && <span className="muted hint">{testBlock}</span>}
      </div>
      <TestOutcome test={test} />
    </>
  );
}

/** A stat integration: how often, which part of the JSON, and the source, with a Test. */
function StatFields({
  integration,
  errors,
  secrets,
  testBlock,
  onChange,
  onDuplicates,
}: {
  integration: StatIntegration;
  errors: IntegrationErrors;
  secrets: string[];
  testBlock: string | null;
  onChange: (next: StatIntegration) => void;
  onDuplicates: (messages: string[]) => void;
}) {
  return (
    <>
      <div className="form-grid">
        <Field
          label="Poll every"
          hint="seconds, 2 to 86400"
          problems={errors.every_s}
        >
          <NumberField
            value={integration.every_s}
            onChange={(every_s) => onChange({ ...integration, every_s })}
          />
        </Field>
        <Field
          label="JSON pointer"
          hint="optional, like /state"
          problems={errors.json_pointer}
        >
          <input
            className="mono"
            value={integration.json_pointer ?? ""}
            spellCheck={false}
            onChange={(e) =>
              onChange({
                ...integration,
                json_pointer: e.currentTarget.value || undefined,
              })
            }
          />
        </Field>
      </div>
      <h5>Source</h5>
      <ActionForm
        action={integration.source}
        secrets={secrets}
        problems={(field) => errors[`source.${field}`]}
        onDuplicates={onDuplicates}
        onChange={(source) => onChange({ ...integration, source })}
      />
      <TestRow id={integration.id} testBlock={testBlock} />
    </>
  );
}

/** The longest suffix the board's card takes (`MAX_SUFFIX` in config.rs). */
const MAX_SUFFIX = 7;

/** A prometheus integration: the server, the query, how the value shows and when it is bad. */
function PrometheusFields({
  integration,
  errors,
  secrets,
  testBlock,
  onChange,
  onDuplicates,
}: {
  integration: PrometheusIntegration;
  errors: IntegrationErrors;
  secrets: string[];
  testBlock: string | null;
  onChange: (next: PrometheusIntegration) => void;
  onDuplicates: (messages: string[]) => void;
}) {
  const [headers, setHeaders] = useState<Row[]>(() =>
    recordToRows(integration.secret_headers),
  );
  useEffect(() => {
    onDuplicates(
      duplicateNames(headers).map(
        (n) => `Secret header ${n || "(blank)"} is listed twice`,
      ),
    );
    return () => onDuplicates([]);
  }, [headers, onDuplicates]);

  const set = <K extends keyof PrometheusIntegration>(
    key: K,
    value: PrometheusIntegration[K] | undefined | "",
  ) => onChange(withOptional(integration, key, value));
  const every = Number.isFinite(integration.every_s) ? integration.every_s : 0;
  const most = Math.min(30, every);
  return (
    <>
      <div className="form-grid">
        <Field
          label="Server"
          hint="the base URL, such as http://prom.lan:9090. The daemon adds /api/v1/query"
          wide
          problems={errors.url}
        >
          <input
            className="mono"
            value={integration.url}
            spellCheck={false}
            onChange={(e) =>
              onChange({ ...integration, url: e.currentTarget.value })
            }
          />
        </Field>
        <Field
          label="Query"
          hint="PromQL, one series or a scalar, up to 2000 bytes"
          wide
          problems={errors.query}
        >
          <textarea
            className="mono"
            rows={2}
            spellCheck={false}
            value={integration.query}
            onChange={(e) =>
              onChange({ ...integration, query: e.currentTarget.value })
            }
          />
        </Field>
        <Field
          label="Poll every"
          hint="seconds, 2 to 86400"
          problems={errors.every_s}
        >
          <NumberField
            value={integration.every_s}
            onChange={(every_s) => onChange({ ...integration, every_s })}
          />
        </Field>
        <Field
          label="Timeout"
          hint={
            most > 0
              ? `optional, 1 to ${most} seconds; unset: ${Math.min(10, every)}`
              : "optional, seconds"
          }
          problems={errors.timeout_s}
        >
          <OptionalNumber
            whole
            value={integration.timeout_s}
            onChange={(value) => set("timeout_s", value)}
          />
        </Field>
        <Field
          label="Decimals"
          hint="optional, 0 to 3; unset: as Prometheus gives it"
          problems={errors.decimals}
        >
          <OptionalNumber
            whole
            value={integration.decimals}
            onChange={(value) => set("decimals", value)}
          />
        </Field>
        <Field
          label="Suffix"
          hint={`optional, ${(integration.suffix ?? "").length}/${MAX_SUFFIX}`}
          problems={errors.suffix}
        >
          <input
            className="mono"
            value={integration.suffix ?? ""}
            spellCheck={false}
            onChange={(e) => set("suffix", e.currentTarget.value)}
          />
        </Field>
        <Field
          label="Bad above"
          hint="optional, a number"
          problems={errors.bad_above}
        >
          <OptionalNumber
            whole={false}
            value={integration.bad_above}
            onChange={(value) => set("bad_above", value)}
          />
        </Field>
        <Field
          label="Bad below"
          hint="optional, a number"
          problems={errors.bad_below}
        >
          <OptionalNumber
            whole={false}
            value={integration.bad_below}
            onChange={(value) => set("bad_below", value)}
          />
        </Field>
        <SecretHeaderRows
          rows={headers}
          secrets={secrets}
          problems={errors.secret_headers}
          onChange={(rows) => {
            setHeaders(rows);
            onChange(
              withOptional(integration, "secret_headers", rowsToRecord(rows)),
            );
          }}
        />
        <div className="field wide">
          <label className="check">
            <input
              type="checkbox"
              checked={integration.insecure_http ?? false}
              onChange={(e) =>
                set("insecure_http", e.currentTarget.checked || undefined)
              }
            />
            Allow secret headers over plain http://
          </label>
          {integration.insecure_http && (
            <p className="warning" role="note">
              Anyone on the network path can read these secrets. Use https://
              unless the server is on a network you trust.
            </p>
          )}
          <Problems list={errors.insecure_http} />
        </div>
        <div className="field wide">
          <label className="check">
            <input
              type="checkbox"
              checked={integration.insecure_tls ?? false}
              onChange={(e) =>
                set("insecure_tls", e.currentTarget.checked || undefined)
              }
            />
            Skip certificate verification (self-signed https://)
          </label>
          {integration.insecure_tls && (
            <p className="warning" role="note">
              Anyone on the network can intercept this request, and read or
              change what it gets back, including secret headers. Use it only
              for a server you trust on a network you trust.
            </p>
          )}
          <Problems list={errors.insecure_tls} />
        </div>
      </div>
      <TestRow id={integration.id} testBlock={testBlock} />
    </>
  );
}

/** Grafana's severity labels to a level: rows of a severity and the level its alerts get. */
function LevelRows({
  rows,
  onChange,
}: {
  rows: Row[];
  onChange: (rows: Row[]) => void;
}) {
  const set = (i: number, part: Partial<Row>) =>
    onChange(rows.map((r, j) => (j === i ? { ...r, ...part } : r)));
  return (
    <div className="rows">
      {rows.map((row, i) => (
        // biome-ignore lint/suspicious/noArrayIndexKey: a row has no identity but its place
        <div className="row-pair" key={i}>
          <input
            className="mono"
            aria-label="Severity"
            placeholder="severity"
            value={row.name}
            spellCheck={false}
            onChange={(e) => set(i, { name: e.currentTarget.value })}
          />
          <select
            aria-label="Level"
            value={row.value}
            onChange={(e) => set(i, { value: e.currentTarget.value })}
          >
            {NOTE_LEVELS.map((level) => (
              <option key={level}>{level}</option>
            ))}
          </select>
          <button
            type="button"
            className="quiet"
            aria-label={`Remove severity row ${i + 1}`}
            onClick={() => onChange(rows.filter((_, j) => j !== i))}
          >
            Remove
          </button>
        </div>
      ))}
      <button
        type="button"
        className="quiet"
        onClick={() => onChange([...rows, { name: "", value: "info" }])}
      >
        Add
      </button>
      <Problems
        list={caseDuplicateNames(rows).map(
          (name) =>
            `${name === "" ? "A blank severity" : name} is listed twice; severities ignore case`,
        )}
      />
    </div>
  );
}

/** A grafana integration: its token, the severity levels, and what to enter in the contact point. */
function GrafanaFields({
  integration,
  idProblem,
  errors,
  secrets,
  claimed,
  active,
  lan,
  onChange,
  onDuplicates,
  onSecretsChanged,
}: {
  integration: GrafanaIntegration;
  idProblem: string | null;
  errors: IntegrationErrors;
  secrets: string[];
  claimed: boolean;
  active: boolean;
  lan: boolean;
  onChange: (next: GrafanaIntegration) => void;
  onDuplicates: (messages: string[]) => void;
  onSecretsChanged: () => void;
}) {
  const [levels, setLevels] = useState<Row[]>(() =>
    recordToRows(integration.levels),
  );
  useEffect(() => {
    onDuplicates(
      caseDuplicateNames(levels).map(
        (n) =>
          `Severity ${n || "(blank)"} is listed twice; severities ignore case`,
      ),
    );
    return () => onDuplicates([]);
  }, [levels, onDuplicates]);

  return (
    <>
      <TokenFields
        integration={integration}
        idProblem={idProblem}
        errors={errors}
        secrets={secrets}
        claimed={claimed}
        active={active}
        onChange={(next) => onChange(next as GrafanaIntegration)}
        onSecretsChanged={onSecretsChanged}
      >
        {(token) => (
          <div className="copy-block">
            <div className="outside">
              <h5>Grafana contact point (type Webhook)</h5>
            </div>
            {grafanaContactPoint({ lan, token }).map(({ setting, value }) => (
              <div className="outside" key={setting}>
                <span className="field-label">{setting}</span>
                <code className="mono token">{value}</code>
                {(setting !== "Authorization header: credentials" ||
                  token !== null) && <CopyButton text={value} label="Copy" />}
              </div>
            ))}
            <p className="muted hint">
              {lan
                ? "Replace desktop.lan with this machine's name. Grafana verifies the certificate on https, and this one is self-signed: use the contact point's TLS setting if your Grafana has one, knowing a LAN eavesdropper then gets the token."
                : "For a Grafana on this machine, not in a container. From another host, turn on the LAN listener below."}{" "}
              The Test button on the contact point sends an alert, which shows
              as a note.
            </p>
          </div>
        )}
      </TokenFields>
      <fieldset className="field wide">
        <legend className="field-label">
          Levels
          <span className="field-hint">
            optional; critical is error and warning is warn already, anything
            else is info
          </span>
        </legend>
        <LevelRows
          rows={levels}
          onChange={(rows) => {
            setLevels(rows);
            onChange(
              withOptional(
                integration,
                "levels",
                rowsToRecord(rows.filter((r) => r.name.trim() !== "")) as
                  | Record<string, NoteLevel>
                  | undefined,
              ),
            );
          }}
        />
        <Problems list={errors.levels} />
      </fieldset>
    </>
  );
}

/** The fields that belong to one kind. A new kind adds its case here. */
function KindFields({
  integration,
  idProblem,
  errors,
  secrets,
  claimed,
  active,
  lan,
  pin,
  pinError,
  testBlock,
  onChange,
  onDuplicates,
  onSecretsChanged,
}: {
  integration: IntegrationConfig;
  idProblem: string | null;
  errors: IntegrationErrors;
  secrets: string[];
  claimed: boolean;
  active: boolean;
  lan: boolean;
  pin: string | null;
  pinError: string;
  testBlock: string | null;
  onChange: (next: IntegrationConfig) => void;
  onDuplicates: (messages: string[]) => void;
  onSecretsChanged: () => void;
}) {
  switch (integration.kind) {
    case "push":
      return (
        <PushFields
          integration={integration}
          idProblem={idProblem}
          errors={errors}
          secrets={secrets}
          claimed={claimed}
          active={active}
          lan={lan}
          pin={pin}
          pinError={pinError}
          onChange={onChange}
          onSecretsChanged={onSecretsChanged}
        />
      );
    case "stat":
      return (
        <StatFields
          integration={integration}
          errors={errors}
          secrets={secrets}
          testBlock={testBlock}
          onChange={onChange}
          onDuplicates={onDuplicates}
        />
      );
    case "prometheus":
      return (
        <PrometheusFields
          integration={integration}
          errors={errors}
          secrets={secrets}
          testBlock={testBlock}
          onChange={onChange}
          onDuplicates={onDuplicates}
        />
      );
    case "grafana":
      return (
        <GrafanaFields
          integration={integration}
          idProblem={idProblem}
          errors={errors}
          secrets={secrets}
          claimed={claimed}
          active={active}
          lan={lan}
          onChange={onChange}
          onDuplicates={onDuplicates}
          onSecretsChanged={onSecretsChanged}
        />
      );
    default: {
      const unhandled: never = integration;
      return unhandled;
    }
  }
}

/** One integration: its id and label, then its kind's fields, with the controls to move or remove it. */
function Integration({
  active,
  config,
  saved,
  diskMoved,
  index,
  errors,
  secrets,
  pin,
  pinError,
  onChange,
  onMove,
  onRemove,
  onDuplicates,
  onSecretsChanged,
}: {
  active: boolean;
  config: Config;
  saved: Config;
  diskMoved: boolean;
  index: number;
  errors: Map<string, string[]>;
  secrets: string[];
  pin: string | null;
  pinError: string;
  onChange: (config: Config) => void;
  onMove: (by: -1 | 1) => void;
  onRemove: () => void;
  /** Header names or severities listed twice in this integration, which a table can't hold. */
  onDuplicates: (messages: string[]) => void;
  onSecretsChanged: () => void;
}) {
  const list = config.integration ?? [];
  const integration = list[index];
  const fields = integrationFields(errors.get(integration.id) ?? []);
  const idProblem = integrationIdProblem(config, index);
  const set = (next: IntegrationConfig) =>
    onChange(setIntegration(config, index, next));
  return (
    <section aria-label={`Integration ${integration.id}`}>
      <div className="panel-head">
        <h3>
          {integration.label || integration.id}{" "}
          <span className="mono id">{integration.id}</span>{" "}
          <span className="chip">{integration.kind}</span>
        </h3>
        <span className="outside">
          <button
            type="button"
            disabled={index === 0}
            aria-label={`Move ${integration.id} earlier`}
            onClick={() => onMove(-1)}
          >
            Move up
          </button>
          <button
            type="button"
            disabled={index === list.length - 1}
            aria-label={`Move ${integration.id} later`}
            onClick={() => onMove(1)}
          >
            Move down
          </button>
          <button type="button" className="danger" onClick={onRemove}>
            Remove
          </button>
        </span>
      </div>
      <Problems list={fields.integration} />
      <div className="form-grid">
        <Field
          label="Id"
          hint="letters, digits, _ - ."
          problems={[...(idProblem ? [idProblem] : []), ...(fields.id ?? [])]}
        >
          <input
            className="mono"
            value={integration.id}
            spellCheck={false}
            onChange={(e) =>
              onChange(withIntegrationId(config, index, e.currentTarget.value))
            }
          />
        </Field>
        <Field
          label="Label"
          hint={`${integration.label.length}/${MAX_INTEGRATION_LABEL}`}
          problems={fields.label}
        >
          <input
            value={integration.label}
            onChange={(e) =>
              set({ ...integration, label: e.currentTarget.value })
            }
          />
        </Field>
      </div>
      <KindFields
        integration={integration}
        idProblem={idProblem}
        errors={fields}
        secrets={secrets}
        claimed={
          (integration.kind === "push" || integration.kind === "grafana") &&
          savedNamesSecret(saved, integration)
        }
        active={active}
        lan={config.push?.lan ?? false}
        pin={pin}
        pinError={pinError}
        testBlock={integrationTestBlock(integration, saved, diskMoved)}
        onChange={set}
        onDuplicates={onDuplicates}
        onSecretsChanged={onSecretsChanged}
      />
    </section>
  );
}

/**
 * The Feed tab: the integrations that fill the board's -1 page, in the order their cards show,
 * the push API's LAN switch, and a live preview of the page.
 */
export default function FeedForm({
  active,
  config,
  saved,
  diskMoved,
  errors,
  secrets,
  onChange,
  onLocalProblems,
  onSecretsChanged,
}: Props) {
  const list = config.integration ?? [];
  // A key for each integration that outlives edits to its id, so a form keeps its rows.
  const [keys, setKeys] = useState<string[]>(() => list.map(newKey));
  const [dupes, setDupes] = useState<Record<string, string[]>>({});
  const reports = useRef(new Map<string, (messages: string[]) => void>());
  const reporter = (key: string, label: string) => {
    let report = reports.current.get(key);
    if (!report) {
      report = (messages: string[]) =>
        setDupes((d) => ({
          ...d,
          [key]: messages.map((m) => `${label}: ${m}`),
        }));
      reports.current.set(key, report);
    }
    return report;
  };
  useEffect(() => {
    onLocalProblems(Object.values(dupes).flat());
    return () => onLocalProblems([]);
  }, [dupes, onLocalProblems]);

  const anyPush = list.some((i) => i.kind === "push");
  const anyReceiver = list.some(
    (i) => i.kind === "push" || i.kind === "grafana",
  );
  const lan = config.push?.lan ?? false;
  const [pin, setPin] = useState<string | null>(null);
  const [pinError, setPinError] = useState("");
  const wantPin = anyPush && lan;
  useEffect(() => {
    if (!wantPin || pin !== null) {
      return;
    }
    pushPin().then(
      (p) => {
        setPin(p);
        setPinError("");
      },
      (e) => setPinError(String(e)),
    );
  }, [wantPin, pin]);

  const cards = list.filter(isCard).length;
  const keyOf = (i: number) => keys[i] ?? `index-${i}`;

  return (
    <div className="button-form feed-form">
      <div className="panel-head">
        <h3>Feed</h3>
        <span className="outside">
          {ADD_KINDS.map(({ kind, label, hint }) => (
            <button
              type="button"
              key={kind}
              title={hint}
              disabled={
                list.length >= MAX_INTEGRATIONS ||
                (kindIsCard(kind) && cards >= MAX_CARD_SOURCES)
              }
              onClick={() => {
                setKeys([...keys, newKey()]);
                onChange(addIntegration(config, kind));
              }}
            >
              {label}
            </button>
          ))}
        </span>
      </div>
      <p className="muted hint">
        A board shows the feed one swipe to the right of page 1, while at least
        one integration is saved. Push sources take notes and cards from
        scripts, cron and Home Assistant; Grafana turns its alerts into notes;
        stat and Prometheus cards poll a command, a URL or a query. Up to{" "}
        {MAX_INTEGRATIONS} integrations, and {MAX_CARD_SOURCES} that show a
        card.
      </p>
      {list.length === 0 && (
        <p className="muted">No integrations yet. Add one above.</p>
      )}

      {list.map((integration, i) => (
        <Integration
          key={keyOf(i)}
          active={active}
          config={config}
          saved={saved}
          diskMoved={diskMoved}
          index={i}
          errors={errors}
          secrets={secrets}
          pin={pin}
          pinError={pinError}
          onChange={onChange}
          onMove={(by) => {
            const to = i + by;
            if (to >= 0 && to < keys.length) {
              const next = [...keys];
              [next[i], next[to]] = [next[to], next[i]];
              setKeys(next);
            }
            onChange(moveIntegration(config, i, by));
          }}
          onRemove={() => {
            setKeys(keys.filter((_, j) => j !== i));
            reports.current.delete(keyOf(i));
            setDupes(({ [keyOf(i)]: _, ...rest }) => rest);
            onChange(removeIntegration(config, i));
          }}
          onDuplicates={reporter(keyOf(i), integration.label || integration.id)}
          onSecretsChanged={onSecretsChanged}
        />
      ))}

      {anyReceiver && (
        <section>
          <h4>Push API</h4>
          <label className="check">
            <input
              type="checkbox"
              checked={lan}
              onChange={(e) =>
                onChange(withPushLan(config, e.currentTarget.checked))
              }
            />
            Also serve it on the LAN, over https (port 47823)
          </label>
          <p className="muted hint">
            Off, the API answers on this machine only (127.0.0.1, port 47822,
            plain http). On, other hosts can post with a token, over https with
            this app's own certificate. Open 47823/TCP in the firewall if one
            runs.
          </p>
          {lan && anyPush && (
            <div className="field wide">
              <span className="field-label">
                Certificate pin
                <span className="field-hint">
                  {"for curl --pinnedpubkey sha256//<pin>"}
                </span>
              </span>
              {pin !== null ? (
                <div className="outside">
                  <code className="mono token">{pin}</code>
                  <CopyButton text={pin} label="Copy pin" />
                </div>
              ) : (
                <span className="muted">{pinError || "Reading…"}</span>
              )}
              <p className="muted hint">
                This is the hash of the certificate's public key. It is not the
                fp the daemon logs at startup, which is a different value and
                not one to pin.
              </p>
            </div>
          )}
        </section>
      )}

      <section>
        <FeedPreview />
      </section>
    </div>
  );
}
