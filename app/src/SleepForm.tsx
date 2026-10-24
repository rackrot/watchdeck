import type { CSSProperties, ReactNode } from "react";
import { SLEEP_DEFAULTS, type SleepConfig } from "./config";
import { withSleep } from "./edit";
import type { SleepErrors } from "./errors";
import { ColorField, Field, NumberField, Problems } from "./fields";
import { fontInfo } from "./fonts";
import { useWidth } from "./hooks";
import {
  SLEEP_AREA_H,
  SLEEP_AREA_W,
  SLEEP_ELEMENTS,
  SLEEP_STRIP_H,
  type SleepElement,
  sleepLayout,
  sleepTextAlign,
} from "./preview";

type Props = {
  /** The draft's `[sleep]`. Absent: every setting is the device's default. */
  sleep: SleepConfig | undefined;
  /** Rust's complaints from the last save, by field. */
  errors: SleepErrors;
  /** The files in `screens/`. */
  images: string[];
  imageError: string;
  screenUrl: (name: string) => string | null;
  onImport: () => void;
  onChange: (sleep: SleepConfig | undefined) => void;
};

const SHOW: { id: SleepElement; label: string }[] = [
  { id: "clock", label: "Clock" },
  { id: "date", label: "Date" },
  { id: "host", label: "Desktop's name" },
  { id: "text", label: "Text" },
];

/** The 3 x 3 pad: the middle and the four corners are positions, the rest are blank. */
const PAD: (string | null)[] = [
  "top_left",
  null,
  "top_right",
  null,
  "center",
  null,
  "bottom_left",
  null,
  "bottom_right",
];

const SPINNERS = ["arc", "dots", "bar", "pulse", "none"] as const;

/** What the preview shows where the device shows live data. */
const SAMPLE: Record<Exclude<SleepElement, "text">, string> = {
  clock: "14:32",
  date: "Wed 22 Oct",
  host: "devbox",
};

const FIT_CHOICES = [
  { id: "cover", label: "Cover (fill and crop)" },
  { id: "contain", label: "Contain (show it whole)" },
];

// A slider replaces On and Off if the P0 PWM probe passes (03-sleep.md, D9). Until then the
// firmware only tells 0 (light off) from anything else.
const BACKLIGHT_CHOICES = [
  { id: "100", label: "On" },
  { id: "0", label: "Off" },
];

/** A row of buttons of which one is chosen. */
function Choice({
  label,
  value,
  choices,
  problems,
  onChange,
}: {
  label: string;
  value: string;
  choices: { id: string; label: string }[];
  problems?: string[];
  onChange: (id: string) => void;
}) {
  return (
    <div className="field">
      <span className="field-label">{label}</span>
      <fieldset className="outside bare" aria-label={label}>
        {choices.map((c) => (
          <button
            type="button"
            key={c.id}
            className={value === c.id ? "chip selected" : "chip"}
            aria-pressed={value === c.id}
            onClick={() => onChange(c.id)}
          >
            {c.label}
          </button>
        ))}
      </fieldset>
      <Problems list={problems} />
    </div>
  );
}

/** One spinner style, animated in CSS. `size` is in screen pixels. */
function Spinner({
  kind,
  color,
  size,
}: {
  kind: string;
  color: string;
  size?: number;
}): ReactNode {
  if (kind === "none") {
    return <span className="muted">none</span>;
  }
  const style = {
    "--spin": color,
    ...(size && { "--size": `${size}px` }),
  } as CSSProperties;
  return (
    <span className={`spin spin-${kind}`} style={style} aria-hidden="true">
      {kind === "dots" && (
        <>
          <i />
          <i />
          <i />
        </>
      )}
      {kind === "bar" && <i />}
    </span>
  );
}

/** The whole screen at the panel's pixels, scaled to fit, with the strip marked "while offline". */
function SleepPreview({
  sleep,
  imageUrl,
}: {
  sleep: SleepConfig | undefined;
  imageUrl: string | null;
}) {
  const [stage, stageWidth] = useWidth();
  const scale = Math.min(1, stageWidth / SLEEP_AREA_W);
  const bg = sleep?.bg ?? SLEEP_DEFAULTS.bg;
  const fg = sleep?.fg ?? SLEEP_DEFAULTS.fg;
  const anchor = sleep?.anchor ?? SLEEP_DEFAULTS.anchor;
  const spinner = sleep?.spinner ?? SLEEP_DEFAULTS.spinner;
  const spinnerColor = sleep?.spinner_color ?? SLEEP_DEFAULTS.spinner_color;
  const show: readonly string[] = sleep?.show ?? SLEEP_DEFAULTS.show;
  const text = sleep?.text ?? "";
  // As the device: the text needs words, and the clock, date and name are shown once it knows them.
  const shown = SLEEP_ELEMENTS.map(
    (el) => show.includes(el) && (el !== "text" || text !== ""),
  );
  const boxes = sleepLayout(shown, anchor);
  const family = fontInfo(undefined).family;

  return (
    <div
      className="stage"
      ref={stage}
      style={{ height: (SLEEP_AREA_H + SLEEP_STRIP_H) * scale }}
    >
      <div
        className="screen sleep-screen"
        style={{
          width: SLEEP_AREA_W,
          height: SLEEP_AREA_H + SLEEP_STRIP_H,
          transform: `scale(${scale})`,
          background: bg,
        }}
        role="img"
        aria-label="Sleep screen preview"
      >
        {imageUrl ? (
          <img
            className="sleep-image"
            src={imageUrl}
            alt=""
            style={{
              width: SLEEP_AREA_W,
              height: SLEEP_AREA_H,
              objectFit: sleep?.fit === "contain" ? "contain" : "cover",
            }}
          />
        ) : (
          // The logo goes here; until it does the device draws a rounded square in the text color.
          <div
            className="sleep-logo"
            style={{ background: fg, left: (SLEEP_AREA_W - 240) / 2 }}
          />
        )}
        {SLEEP_ELEMENTS.map((el, i) => {
          const box = boxes[i];
          if (!box) {
            return null;
          }
          return (
            <div
              key={el}
              className="sleep-el"
              style={{
                left: box.x,
                top: box.y,
                width: box.w,
                height: box.h,
                lineHeight: `${box.h}px`,
                fontFamily: family,
                fontSize: el === "clock" ? 48 : 32,
                textAlign: sleepTextAlign(anchor),
                color: fg,
              }}
            >
              {el === "text" ? text : SAMPLE[el]}
            </div>
          );
        })}
        <div
          className="sleep-strip"
          style={{ top: SLEEP_AREA_H, height: SLEEP_STRIP_H }}
        >
          <span className="sleep-offline">while offline</span>
          <Spinner kind={spinner} color={spinnerColor} size={48} />
          <span style={{ color: fg, fontFamily: family, fontSize: 32 }}>
            searching for devbox
          </span>
        </div>
      </div>
    </div>
  );
}

export default function SleepForm({
  sleep,
  errors,
  images,
  imageError,
  screenUrl,
  onImport,
  onChange,
}: Props) {
  const set = (patch: Parameters<typeof withSleep>[1]) =>
    onChange(withSleep(sleep, patch));

  const show: readonly string[] = sleep?.show ?? SLEEP_DEFAULTS.show;
  const anchor = sleep?.anchor ?? SLEEP_DEFAULTS.anchor;
  const spinner = sleep?.spinner ?? SLEEP_DEFAULTS.spinner;
  const spinnerColor = sleep?.spinner_color ?? SLEEP_DEFAULTS.spinner_color;
  // The picker offers the files in screens/, and the config's own image even if its file is gone.
  const files =
    sleep?.image && !images.includes(sleep.image)
      ? [...images, sleep.image].sort()
      : images;
  const imageUrl = sleep?.image ? screenUrl(sleep.image) : null;

  return (
    <div className="button-form sleep-form">
      <div className="panel-head">
        <h3>Sleep screen</h3>
      </div>
      <Problems list={errors.sleep} />

      <SleepPreview sleep={sleep} imageUrl={imageUrl} />

      <section>
        <h4>Picture</h4>
        <div className="form-grid">
          <Field label="Image" problems={errors.image}>
            <span className="icon-pick">
              <select
                value={sleep?.image ?? ""}
                onChange={(e) =>
                  set({ image: e.currentTarget.value || undefined })
                }
              >
                <option value="">(none: the logo)</option>
                {files.map((name) => (
                  <option key={name} value={name}>
                    {name}
                  </option>
                ))}
              </select>
              <button type="button" onClick={onImport}>
                Import…
              </button>
              {imageUrl && <img className="thumb" src={imageUrl} alt="" />}
            </span>
          </Field>
          <Choice
            label="Fit"
            value={sleep?.fit ?? SLEEP_DEFAULTS.fit}
            choices={FIT_CHOICES}
            problems={errors.fit}
            onChange={(fit) => set({ fit })}
          />
          <ColorField
            label="Background"
            value={sleep?.bg ?? SLEEP_DEFAULTS.bg}
            problems={errors.bg}
            onChange={(bg) => set({ bg })}
          />
          <ColorField
            label="Text color"
            value={sleep?.fg ?? SLEEP_DEFAULTS.fg}
            problems={errors.fg}
            onChange={(fg) => set({ fg })}
          />
        </div>
        {imageError && (
          <p className="problems" role="alert">
            {imageError}
          </p>
        )}
      </section>

      <section>
        <h4>Text</h4>
        <div className="form-grid">
          <div className="field">
            <span className="field-label">Show</span>
            <fieldset className="outside bare" aria-label="Show">
              {SHOW.map((s) => (
                <label className="check" key={s.id}>
                  <input
                    type="checkbox"
                    checked={show.includes(s.id)}
                    onChange={(e) =>
                      set({
                        // Kept in the order the device stacks them, whatever order they were ticked in.
                        show: SLEEP_ELEMENTS.filter((el) =>
                          el === s.id
                            ? e.currentTarget.checked
                            : show.includes(el),
                        ),
                      })
                    }
                  />
                  {s.label}
                </label>
              ))}
            </fieldset>
            <Problems list={errors.show} />
          </div>
          <Field label="Text" hint="up to 47 characters" problems={errors.text}>
            <input
              value={sleep?.text ?? ""}
              spellCheck={false}
              onChange={(e) =>
                set({ text: e.currentTarget.value || undefined })
              }
            />
          </Field>
          <div className="field">
            <span className="field-label">Position</span>
            <fieldset className="anchor-pad bare" aria-label="Position">
              {PAD.map((id, i) =>
                id === null ? (
                  // biome-ignore lint/suspicious/noArrayIndexKey: the pad's blanks have no id
                  <span key={i} />
                ) : (
                  <button
                    type="button"
                    key={id}
                    className={anchor === id ? "chip selected" : "chip"}
                    aria-pressed={anchor === id}
                    aria-label={id.replace("_", " ")}
                    onClick={() => set({ anchor: id })}
                  />
                ),
              )}
            </fieldset>
            <Problems list={errors.anchor} />
          </div>
        </div>
      </section>

      <section>
        <h4>While the link is down</h4>
        <div className="field">
          <span className="field-label">Spinner</span>
          <fieldset className="outside bare" aria-label="Spinner">
            {SPINNERS.map((id) => (
              <button
                type="button"
                key={id}
                className={spinner === id ? "chip selected" : "chip"}
                aria-pressed={spinner === id}
                aria-label={`${id} spinner`}
                onClick={() => set({ spinner: id })}
              >
                <span className="spin-swatch">
                  <Spinner kind={id} color={spinnerColor} />
                </span>
                {id}
              </button>
            ))}
          </fieldset>
          <Problems list={errors.spinner} />
        </div>
        <div className="form-grid">
          <ColorField
            label="Spinner color"
            value={spinnerColor}
            problems={errors.spinner_color}
            onChange={(spinner_color) => set({ spinner_color })}
          />
          <Field
            label="Sleep after the link is down for (seconds)"
            hint="0 = never"
            problems={errors.offline_after_s}
          >
            <NumberField
              value={sleep?.offline_after_s ?? SLEEP_DEFAULTS.offline_after_s}
              onChange={(offline_after_s) => set({ offline_after_s })}
            />
          </Field>
          <Choice
            label="Backlight while asleep"
            value={
              (sleep?.backlight ?? SLEEP_DEFAULTS.backlight) === 0 ? "0" : "100"
            }
            choices={BACKLIGHT_CHOICES}
            problems={errors.backlight}
            onChange={(id) => set({ backlight: Number(id) })}
          />
        </div>
      </section>
    </div>
  );
}
