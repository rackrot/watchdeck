import { useEffect, useRef, useState } from "react";
import ActionForm from "./ActionForm";
import { createPushToken, pushPin, testIntegration } from "./api";
import {
  type Config,
  type IntegrationConfig,
  MAX_INTEGRATION_LABEL,
  MAX_INTEGRATIONS,
  tokenSecretFor,
} from "./config";
import {
  addIntegration,
  integrationIdProblem,
  moveIntegration,
  removeIntegration,
  setIntegration,
  withIntegrationId,
  withPushLan,
} from "./edit";
import { type IntegrationErrors, integrationFields } from "./errors";
import FeedPreview from "./FeedPreview";
import {
  Field,
  NumberField,
  Problems,
  TestOutcome,
  type TestState,
} from "./fields";
import { cliLine, curlLine, homeAssistantBlock } from "./pushlines";
import { integrationTestBlock } from "./saveflow";

/** The card sources the board holds (`MAX_CARD_SOURCES`). */
const MAX_CARD_SOURCES = 8;

type Kind = IntegrationConfig["kind"];

/**
 * The kinds the "Add" buttons make. A new kind (prometheus, grafana) is: its variant in
 * `IntegrationConfig` (config.ts) and arm in `addIntegration` (edit.ts), whether it counts as a card
 * source below, one entry here, and one case in `KindFields`. TypeScript names the places missed.
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
];

/** Whether the integration shows a card, as `is_card` in config.rs. */
const isCard = (integration: IntegrationConfig): boolean =>
  integration.kind === "stat";

type Props = {
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

/**
 * A push integration: its token, and the lines that use it. The token is held here, in this
 * component's state alone, from Generate until the tab is left or Done is pressed. It is never
 * stored by the page, and `create_push_token` is the only way it reaches the window.
 */
function PushFields({
  integration,
  idProblem,
  errors,
  secrets,
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
  lan: boolean;
  pin: string | null;
  pinError: string;
  onChange: (next: PushIntegration) => void;
  onSecretsChanged: () => void;
}) {
  // The id the token was made for travels with it, so editing the id drops it from view.
  const [held, setHeld] = useState<{ id: string; token: string } | null>(null);
  const [confirming, setConfirming] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState("");
  const token = held?.id === integration.id ? held.token : null;
  const hasToken = secrets.includes(tokenSecretFor(integration.id));

  async function generate() {
    setConfirming(false);
    setBusy(true);
    setError("");
    const { id } = integration;
    try {
      const fresh = await createPushToken(id);
      setHeld({ id, token: fresh });
      onChange({ ...integration, token_secret: tokenSecretFor(id) });
      onSecretsChanged();
    } catch (e) {
      setError(String(e));
    } finally {
      setBusy(false);
    }
  }

  const pinText = pin ?? "<the pin>";
  const cli = cliLine(integration.id);
  return (
    <>
      <div className="field wide">
        <span className="field-label">
          Token
          <span className="field-hint">
            stored as the secret {tokenSecretFor(integration.id)}
          </span>
        </span>
        <div className="outside">
          {hasToken ? (
            <span className="chip ok">token set</span>
          ) : (
            <span className="chip stale">no token yet</span>
          )}
          <button
            type="button"
            disabled={busy || idProblem !== null}
            onClick={() => (hasToken ? setConfirming(true) : void generate())}
          >
            {busy
              ? "Generating…"
              : hasToken
                ? "Regenerate token"
                : "Generate token"}
          </button>
          {idProblem && <span className="muted hint">Fix the id first.</span>}
        </div>
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
            Regenerating replaces the token stored as{" "}
            <span className="mono">{tokenSecretFor(integration.id)}</span>, at
            once and whether or not you save. Whatever still sends the old one
            gets a 401 until it is given the new one.
          </span>
          <button
            type="button"
            className="danger"
            onClick={() => void generate()}
          >
            Replace token
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
            which the app never reads back, so copy it now. The lines below hold
            it too, and go when you leave this tab or press Done.
          </p>
        </div>
      )}

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
  /** Why Test is disabled, or null if it can run. */
  testBlock: string | null;
  onChange: (next: StatIntegration) => void;
  onDuplicates: (messages: string[]) => void;
}) {
  const [testing, setTesting] = useState(false);
  const [test, setTest] = useState<TestState>(null);
  async function runTest() {
    setTesting(true);
    setTest(null);
    try {
      setTest({ result: await testIntegration(integration.id) });
    } catch (e) {
      setTest({ error: String(e) });
    } finally {
      setTesting(false);
    }
  }
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

/** The fields that belong to one kind. A new kind adds its case here. */
function KindFields({
  integration,
  idProblem,
  errors,
  secrets,
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
    default: {
      const unhandled: never = integration;
      return unhandled;
    }
  }
}

/** One integration: its id and label, then its kind's fields, with the controls to move or remove it. */
function Integration({
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
  /** Header names listed twice in this integration's source. */
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
                (kind === "stat" && cards >= MAX_CARD_SOURCES)
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
        scripts, cron and Home Assistant; stat cards poll a command or a URL. Up
        to {MAX_INTEGRATIONS} integrations, and {MAX_CARD_SOURCES} that show a
        card.
      </p>
      {list.length === 0 && (
        <p className="muted">No integrations yet. Add one above.</p>
      )}

      {list.map((integration, i) => (
        <Integration
          key={keyOf(i)}
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

      {anyPush && (
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
          {lan && (
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
