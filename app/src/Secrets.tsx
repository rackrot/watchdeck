import { type FormEvent, useRef, useState } from "react";
import { setSecret } from "./api";

/**
 * Secret names, and a write-only form to set or replace one. The value lives only in an
 * uncontrolled input, and is read once at submit and cleared before the request goes out. Nothing
 * returns a value, so there is nothing to show.
 */
export default function Secrets({
  names,
  onChanged,
}: {
  names: string[];
  /** Called after a secret is saved, so the list can be read again. */
  onChanged: () => void;
}) {
  const [name, setName] = useState("");
  const [error, setError] = useState("");
  const [notice, setNotice] = useState("");
  const value = useRef<HTMLInputElement>(null);

  async function submit(e: FormEvent) {
    e.preventDefault();
    const input = value.current;
    if (!input) {
      return;
    }
    const secret = input.value;
    input.value = "";
    setError("");
    setNotice("");
    try {
      await setSecret(name, secret);
      setNotice(`Saved ${name}. Its value is never shown again.`);
      onChanged();
    } catch (err) {
      setError(String(err));
    }
  }

  return (
    <div className="page secrets">
      <section>
        <h3>Secrets</h3>
        <p className="muted">
          A secret is a header value, such as a token, that an HTTP action sends
          without it appearing in <span className="mono">config.toml</span>.
          Pick one by name in a button's secret headers. Values are stored in{" "}
          <span className="mono">secrets.toml</span> and cannot be read back
          here.
        </p>
        {names.length === 0 ? (
          <p className="muted">No secrets yet.</p>
        ) : (
          <ul className="name-list">
            {names.map((n) => (
              <li key={n}>
                <span className="mono">{n}</span>
                <button
                  type="button"
                  className="quiet"
                  aria-label={`Replace secret ${n}`}
                  onClick={() => setName(n)}
                >
                  Replace
                </button>
              </li>
            ))}
          </ul>
        )}
      </section>
      <section>
        <h3>Set or replace</h3>
        <form className="form-grid" onSubmit={submit} autoComplete="off">
          <label className="field">
            <span className="field-label">
              Name<span className="field-hint">letters, digits, _ -</span>
            </span>
            <input
              className="mono"
              value={name}
              spellCheck={false}
              onChange={(e) => setName(e.currentTarget.value)}
            />
          </label>
          <label className="field">
            <span className="field-label">
              Value<span className="field-hint">write-only</span>
            </span>
            <input
              className="mono"
              ref={value}
              type="password"
              autoComplete="new-password"
              spellCheck={false}
            />
          </label>
          <div className="field wide">
            <button type="submit" className="primary" disabled={name === ""}>
              Save secret
            </button>
          </div>
        </form>
        {notice && <p className="ok-note">{notice}</p>}
        {error && (
          <p className="problems" role="alert">
            {error}
          </p>
        )}
        <p className="muted hint">
          There is no way to delete a secret from this window yet. Remove its
          line from <span className="mono">secrets.toml</span> by hand.
        </p>
      </section>
    </div>
  );
}
