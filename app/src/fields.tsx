import { type ReactNode, useEffect, useState } from "react";
import type { ActionResult } from "./api";
import { parseWhole } from "./edit";
import { DEFAULT_FONT, FONTS, fontInfo } from "./fonts";

/** Rust's complaints about one field, under it. */
export function Problems({ list }: { list?: string[] }) {
  if (!list || list.length === 0) {
    return null;
  }
  return (
    <ul className="problems" role="alert">
      {list.map((message) => (
        <li key={message}>{message}</li>
      ))}
    </ul>
  );
}

/** A label around one control, with the field's problems under it. */
export function Field({
  label,
  hint,
  problems,
  wide,
  children,
}: {
  label: string;
  hint?: string;
  problems?: string[];
  wide?: boolean;
  children: ReactNode;
}) {
  return (
    // biome-ignore lint/a11y/noLabelWithoutControl: the control is `children`
    <label className={wide ? "field wide" : "field"}>
      <span className="field-label">
        {label}
        {hint && <span className="field-hint">{hint}</span>}
      </span>
      {children}
      <Problems list={problems} />
    </label>
  );
}

/** What a Test button last got: the daemon's result, or why the call failed. Null before any. */
export type TestState = { result: ActionResult } | { error: string } | null;

/** The outcome of a Test under its button: a chip with the summary, and the output if there is any. */
export function TestOutcome({ test }: { test: TestState }) {
  if (!test) {
    return null;
  }
  if ("error" in test) {
    return (
      <p className="problems" role="alert">
        {test.error}
      </p>
    );
  }
  const { result } = test;
  return (
    <div role="status">
      <span className={`chip ${result.ok ? "ok" : "bad"}`}>
        {result.ok ? "ok" : "failed"}: {result.summary}
        {result.ms > 0 && ` (${result.ms} ms)`}
      </span>
      {result.text && <pre className="mono">{result.text}</pre>}
    </div>
  );
}

/** A `#rrggbb` field: the browser's picker, and the hex text beside it. */
export function ColorField({
  label,
  value,
  onChange,
  problems,
}: {
  label: string;
  value: string;
  onChange: (value: string) => void;
  problems?: string[];
}) {
  return (
    <Field label={label} problems={problems}>
      <span className="color">
        <input
          type="color"
          aria-label={`${label} picker`}
          value={/^#[0-9a-fA-F]{6}$/.test(value) ? value : "#000000"}
          onChange={(e) => onChange(e.currentTarget.value)}
        />
        <input
          className="mono"
          aria-label={`${label} hex`}
          value={value}
          spellCheck={false}
          onChange={(e) => onChange(e.currentTarget.value)}
        />
      </span>
    </Field>
  );
}

/**
 * A font picker with a sample in the chosen font beside it (WebKitGTK doesn't style `<option>`).
 * With `inherit`, an extra first option means "no font of its own", labelled with the font it
 * inherits, and `value` is undefined while it is chosen.
 */
export function FontSelect({
  value,
  onChange,
  inherit,
}: {
  value: string | undefined;
  onChange: (font: string | undefined) => void;
  inherit?: { label: string; font: string };
}) {
  const shown = value ?? inherit?.font ?? DEFAULT_FONT;
  return (
    <span className="font-pick">
      <select
        value={value ?? ""}
        onChange={(e) => onChange(e.currentTarget.value || undefined)}
      >
        {inherit && (
          <option value="">
            {inherit.label} ({fontInfo(inherit.font).name})
          </option>
        )}
        {value && !FONTS.some((f) => f.id === value) && (
          <option value={value}>{value} (unknown)</option>
        )}
        {FONTS.map((f) => (
          <option key={f.id} value={f.id}>
            {f.name}
          </option>
        ))}
      </select>
      <span
        className="font-sample"
        style={{ fontFamily: fontInfo(shown).family }}
        aria-hidden="true"
      >
        Aa 0123
      </span>
    </span>
  );
}

/**
 * A whole number. It keeps what was typed, and hands up NaN for anything else, which the save
 * reports. A change from outside (a drag, a reload) replaces the text.
 */
export function NumberField({
  value,
  onChange,
}: {
  value: number;
  onChange: (value: number) => void;
}) {
  const [text, setText] = useState(String(value));
  useEffect(() => {
    setText((typed) => {
      const parsed = parseWhole(typed);
      return Object.is(parsed, value) ? typed : String(value);
    });
  }, [value]);
  return (
    <input
      className="mono"
      inputMode="numeric"
      value={text}
      aria-invalid={Number.isNaN(value) || undefined}
      onChange={(e) => {
        setText(e.currentTarget.value);
        onChange(parseWhole(e.currentTarget.value));
      }}
    />
  );
}
