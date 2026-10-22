import { type ReactNode, useEffect, useState } from "react";
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
