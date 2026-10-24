import { useEffect, useRef, useState } from "react";
import { type Action, METHODS, newHttp, newShell } from "./config";
import { duplicateNames, type Row, recordToRows, rowsToRecord } from "./edit";
import type { ActionField } from "./errors";
import { Field, NumberField, Problems } from "./fields";

type Problem = string[] | undefined;

/** The host and port of `url`, or null while it doesn't parse. */
function hostOf(url: string): string | null {
  try {
    return new URL(url).host;
  } catch {
    return null;
  }
}

/** A list of one-line values, such as a command's arguments. */
function ArgvRows({
  argv,
  onChange,
}: {
  argv: string[];
  onChange: (argv: string[]) => void;
}) {
  return (
    <div className="rows">
      {argv.map((arg, i) => (
        // An argument has no identity but its place.
        // biome-ignore lint/suspicious/noArrayIndexKey: see above
        <div className="row-pair single" key={i}>
          <input
            className="mono"
            aria-label={i === 0 ? "Program" : `Argument ${i}`}
            placeholder={i === 0 ? "program" : `argument ${i}`}
            value={arg}
            onChange={(e) =>
              onChange(
                argv.map((a, j) => (j === i ? e.currentTarget.value : a)),
              )
            }
          />
          <button
            type="button"
            className="quiet"
            aria-label={`Remove ${i === 0 ? "program" : `argument ${i}`}`}
            onClick={() => onChange(argv.filter((_, j) => j !== i))}
          >
            Remove
          </button>
        </div>
      ))}
      <button
        type="button"
        className="quiet"
        onClick={() => onChange([...argv, ""])}
      >
        Add argument
      </button>
    </div>
  );
}

/**
 * Name and value rows for a header table. The rows keep their blanks and their order while the
 * table, which the config holds, can't.
 */
function PairRows({
  rows,
  onChange,
  pick,
  names,
}: {
  rows: Row[];
  onChange: (rows: Row[]) => void;
  /** Choices for the value, in place of free text. */
  pick?: string[];
  names: [string, string];
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
            aria-label={names[0]}
            placeholder={names[0]}
            value={row.name}
            onChange={(e) => set(i, { name: e.currentTarget.value })}
          />
          {pick ? (
            <select
              className="mono"
              aria-label={names[1]}
              value={row.value}
              onChange={(e) => set(i, { value: e.currentTarget.value })}
            >
              <option value="">(choose a secret)</option>
              {[...new Set([...pick, row.value])]
                .filter((name) => name !== "")
                .map((name) => (
                  <option key={name} value={name}>
                    {pick.includes(name) ? name : `${name} (not set)`}
                  </option>
                ))}
            </select>
          ) : (
            <input
              className="mono"
              aria-label={names[1]}
              placeholder={names[1]}
              value={row.value}
              onChange={(e) => set(i, { value: e.currentTarget.value })}
            />
          )}
          <button
            type="button"
            className="quiet"
            aria-label={`Remove ${names[0].toLowerCase()} row ${i + 1}`}
            onClick={() => onChange(rows.filter((_, j) => j !== i))}
          >
            Remove
          </button>
        </div>
      ))}
      <button
        type="button"
        className="quiet"
        onClick={() => onChange([...rows, { name: "", value: "" }])}
      >
        Add
      </button>
      <Problems
        list={duplicateNames(rows).map(
          (name) => `${name === "" ? "A blank name" : name} is listed twice`,
        )}
      />
    </div>
  );
}

/**
 * The secret headers of a request: header name to the name of a secret, which is never shown. An
 * HTTP action and a prometheus integration both use it. The caller keeps `rows`, since a table
 * can't hold the blank or repeated names a row can.
 */
export function SecretHeaderRows({
  rows,
  secrets,
  problems,
  onChange,
}: {
  rows: Row[];
  /** The names `list_secrets` returned. */
  secrets: string[];
  problems: Problem;
  onChange: (rows: Row[]) => void;
}) {
  return (
    <fieldset className="field wide">
      <legend className="field-label">
        Secret headers
        <span className="field-hint">
          the value comes from a secret, never shown
        </span>
      </legend>
      <PairRows
        rows={rows}
        names={["Header", "Secret"]}
        pick={secrets}
        onChange={onChange}
      />
      <Problems list={problems} />
    </fieldset>
  );
}

/** The form for a shell or HTTP action. A button and a tile source both use it. */
export default function ActionForm({
  action,
  onChange,
  problems,
  secrets,
  onDuplicates,
}: {
  action: Action;
  onChange: (action: Action) => void;
  /** Rust's complaints about each field of this action. */
  problems: (field: ActionField) => Problem;
  /** The names `list_secrets` returned. */
  secrets: string[];
  /** Reports header names listed twice, which a table can't hold, so a save can wait. */
  onDuplicates: (messages: string[]) => void;
}) {
  const [headers, setHeaders] = useState<Row[]>(
    action.kind === "http" ? recordToRows(action.headers) : [],
  );
  const [secretRows, setSecretRows] = useState<Row[]>(
    action.kind === "http" ? recordToRows(action.secret_headers) : [],
  );

  // The host of the last URL that parsed, so a URL typed through a broken state still counts as
  // a change of host.
  const lastHost = useRef<string | null>(null);
  if (action.kind === "http") {
    lastHost.current = hostOf(action.url) ?? lastHost.current;
  }

  useEffect(() => {
    const messages = [
      ...duplicateNames(headers).map(
        (n) => `Header ${n || "(blank)"} is listed twice`,
      ),
      ...duplicateNames(secretRows).map(
        (n) => `Secret header ${n || "(blank)"} is listed twice`,
      ),
    ];
    onDuplicates(messages);
    return () => onDuplicates([]);
  }, [headers, secretRows, onDuplicates]);

  const timeout = (
    <Field
      label="Timeout"
      hint="seconds, 1 to 600"
      problems={problems("timeout_s")}
    >
      <NumberField
        value={action.timeout_s}
        onChange={(timeout_s) => onChange({ ...action, timeout_s })}
      />
    </Field>
  );

  const switchKind = (kind: string) => {
    const next = kind === "http" ? newHttp() : newShell();
    setHeaders([]);
    setSecretRows([]);
    onChange({ ...next, timeout_s: action.timeout_s });
  };

  if (action.kind === "shell") {
    return (
      <div className="form-grid">
        <KindField kind="shell" onChange={switchKind} />
        {timeout}
        <fieldset className="field wide">
          <legend className="field-label">
            Command
            <span className="field-hint">
              one argument per row, no shell, run in your home directory
            </span>
          </legend>
          <ArgvRows
            argv={action.argv}
            onChange={(argv) => onChange({ ...action, argv })}
          />
          <Problems list={problems("argv")} />
        </fieldset>
      </div>
    );
  }

  const setHttp = (part: Partial<typeof action>) =>
    onChange({ ...action, ...part });
  return (
    <div className="form-grid">
      <KindField kind="http" onChange={switchKind} />
      {timeout}
      <Field label="Method" problems={problems("method")}>
        <select
          className="mono"
          value={action.method}
          onChange={(e) => setHttp({ method: e.currentTarget.value })}
        >
          {METHODS.map((m) => (
            <option key={m}>{m}</option>
          ))}
        </select>
      </Field>
      <Field label="URL" wide problems={problems("url")}>
        <input
          className="mono"
          value={action.url}
          spellCheck={false}
          onChange={(e) => {
            const url = e.currentTarget.value;
            const host = hostOf(url);
            // Skipping verification was agreed for one host, not for whatever the URL becomes.
            const moved =
              host !== null &&
              lastHost.current !== null &&
              host !== lastHost.current;
            setHttp(moved ? { url, insecure_tls: undefined } : { url });
          }}
        />
      </Field>
      <Field label="Body" hint="optional" wide problems={problems("body")}>
        <textarea
          className="mono"
          rows={2}
          spellCheck={false}
          value={action.body ?? ""}
          onChange={(e) =>
            setHttp({ body: e.currentTarget.value || undefined })
          }
        />
      </Field>
      <fieldset className="field wide">
        <legend className="field-label">
          Headers <span className="field-hint">sent as written</span>
        </legend>
        <PairRows
          rows={headers}
          names={["Header", "Value"]}
          onChange={(rows) => {
            setHeaders(rows);
            setHttp({ headers: rowsToRecord(rows) });
          }}
        />
        <Problems list={problems("headers")} />
      </fieldset>
      <SecretHeaderRows
        rows={secretRows}
        secrets={secrets}
        problems={problems("secret_headers")}
        onChange={(rows) => {
          setSecretRows(rows);
          setHttp({ secret_headers: rowsToRecord(rows) });
        }}
      />
      <div className="field wide">
        <label className="check">
          <input
            type="checkbox"
            checked={action.insecure_http ?? false}
            onChange={(e) =>
              setHttp({ insecure_http: e.currentTarget.checked || undefined })
            }
          />
          Allow secret headers over plain http://
        </label>
        {action.insecure_http && (
          <p className="warning" role="note">
            Anyone on the network path can read these secrets. Use https://
            unless the host is on a network you trust.
          </p>
        )}
        <Problems list={problems("insecure_http")} />
      </div>
      <div className="field wide">
        <label className="check">
          <input
            type="checkbox"
            checked={action.insecure_tls ?? false}
            onChange={(e) =>
              setHttp({ insecure_tls: e.currentTarget.checked || undefined })
            }
          />
          Skip certificate verification (self-signed https://)
        </label>
        {action.insecure_tls && (
          <p className="warning" role="note">
            Anyone on the network can intercept this request, and read or change
            what it sends and gets back, including secret headers. Use it only
            for a service you trust on a network you trust.
          </p>
        )}
        <Problems list={problems("insecure_tls")} />
      </div>
    </div>
  );
}

function KindField({
  kind,
  onChange,
}: {
  kind: Action["kind"];
  onChange: (kind: string) => void;
}) {
  return (
    <Field label="Action">
      <select value={kind} onChange={(e) => onChange(e.currentTarget.value)}>
        <option value="shell">Shell command</option>
        <option value="http">HTTP request</option>
      </select>
    </Field>
  );
}
