import { useEffect, useState } from "react";
import { convertStatusBar, type GpuList, listGpus } from "./api";
import type {
  Metric,
  NetUnit,
  StatusBarConfig,
  StatusBarDefaults,
  TempUnit,
} from "./config";
import { pruneBar, withGpu, withSteps } from "./edit";
import { numberErrors, type StatusBarErrors } from "./errors";
import { ColorField, Field, NumberField, Problems } from "./fields";

type Props = {
  /** The draft's `[status_bar]`. Absent: every setting is its default. */
  bar: StatusBarConfig | undefined;
  /** The steps a metric without a key gets, for each temperature unit. */
  defaults: StatusBarDefaults;
  /** Rust's complaints from the last save, by field. */
  errors: StatusBarErrors;
  onChange: (bar: StatusBarConfig | undefined) => void;
};

const TEMP_UNITS: { id: TempUnit; label: string; example: string }[] = [
  { id: "c", label: "°C", example: "64°C" },
  { id: "f", label: "°F", example: "147°F" },
];

const NET_UNITS: {
  id: NetUnit;
  label: string;
  name: string;
  example: string;
}[] = [
  { id: "si", label: "MB/s", name: "SI", example: "1.2M" },
  { id: "binary", label: "MiB/s", name: "Binary", example: "1.1Mi" },
  { id: "bits", label: "Mb/s", name: "Bits", example: "9.6Mb" },
];

const METRICS: { id: Metric; name: string; unit: "%" | "temp" | "net" }[] = [
  { id: "cpu", name: "CPU", unit: "%" },
  { id: "mem", name: "RAM", unit: "%" },
  { id: "gpu", name: "GPU", unit: "%" },
  { id: "cpu_temp", name: "CPU temperature", unit: "temp" },
  { id: "gpu_temp", name: "GPU temperature", unit: "temp" },
  { id: "net_rx", name: "Download", unit: "net" },
  { id: "net_tx", name: "Upload", unit: "net" },
];

const MAX_STEPS = 3;
const AMBER = "#ffb300";

export default function StatusBarForm({
  bar,
  defaults,
  errors,
  onChange,
}: Props) {
  const [gpus, setGpus] = useState<GpuList | null>(null);
  const [gpuError, setGpuError] = useState("");
  // What the last unit switch did, until the next one.
  const [notice, setNotice] = useState("");
  const [convertError, setConvertError] = useState("");
  const [converting, setConverting] = useState(false);

  useEffect(() => {
    listGpus().then(setGpus, (e) => setGpuError(String(e)));
  }, []);

  // A unit the file got wrong is refused on save; until then the form shows the default.
  const temp: TempUnit = TEMP_UNITS.find((u) => u.id === bar?.temp_unit)
    ? (bar?.temp_unit as TempUnit)
    : "c";
  const net: NetUnit = NET_UNITS.find((u) => u.id === bar?.net_unit)
    ? (bar?.net_unit as NetUnit)
    : "si";
  const tempLabel = TEMP_UNITS.find((u) => u.id === temp)?.label ?? temp;
  const netLabel = NET_UNITS.find((u) => u.id === net)?.label ?? net;

  async function switchUnits(nextTemp: TempUnit, nextNet: NetUnit) {
    // Rust would answer a step that isn't a whole number with a decoding error.
    const bad = numberErrors({
      version: 1,
      layout: { cols: 1, rows: 1 },
      button: [],
      status_bar: bar,
    }).statusBar;
    if (Object.keys(bad).length > 0) {
      setNotice("");
      setConvertError(
        "Fix the steps first: every step's At must be a whole number.",
      );
      return;
    }
    setConverting(true);
    setConvertError("");
    try {
      const next = await convertStatusBar(bar ?? {}, nextTemp, nextNet);
      onChange(pruneBar(next));
      // Only a metric counted in the unit that changed has steps to convert.
      const tempChanged = nextTemp !== temp;
      const converted = METRICS.some(
        (m) =>
          m.unit === (tempChanged ? "temp" : "net") &&
          (bar?.thresholds?.[m.id]?.length ?? 0) > 0,
      );
      const to = tempChanged
        ? TEMP_UNITS.find((u) => u.id === nextTemp)?.label
        : NET_UNITS.find((u) => u.id === nextNet)?.label;
      setNotice(
        converted
          ? `Thresholds converted to ${to}.`
          : `No thresholds of your own to convert. The defaults are now in ${to}.`,
      );
    } catch (e) {
      setConvertError(String(e));
    } finally {
      setConverting(false);
    }
  }

  function unitOf(metric: (typeof METRICS)[number]): string {
    return metric.unit === "temp"
      ? tempLabel
      : metric.unit === "net"
        ? netLabel
        : "%";
  }

  /** A first step for a metric with none: its default's `at` for a temperature, else a common one. */
  function firstAt(metric: (typeof METRICS)[number]): number {
    if (metric.unit === "temp") {
      return defaults[temp][metric.id][0]?.at ?? 85;
    }
    return metric.unit === "net" ? 1 : 90;
  }

  const gpu = bar?.gpu ?? "auto";
  const listed = gpus?.gpus.some((g) => g.id === gpu) ?? false;
  const autoName = gpus?.gpus.find((g) => g.id === gpus.auto)?.name;

  return (
    <div className="button-form status-bar-form">
      <div className="panel-head">
        <h3>Status bar</h3>
      </div>
      <Problems list={errors.bar} />

      {/* Disabled during a unit switch, so an edit made meanwhile isn't overwritten by its result. */}
      <fieldset className="bare" disabled={converting}>
        <section>
          <h4>Units</h4>
          <div className="form-grid">
            <Field label="Temperature" problems={errors.temp_unit}>
              <select
                value={temp}
                disabled={converting}
                onChange={(e) =>
                  void switchUnits(e.currentTarget.value as TempUnit, net)
                }
              >
                {TEMP_UNITS.map((u) => (
                  <option key={u.id} value={u.id}>
                    {u.label} (shows {u.example})
                  </option>
                ))}
              </select>
            </Field>
            <Field label="Network rates" problems={errors.net_unit}>
              <select
                value={net}
                disabled={converting}
                onChange={(e) =>
                  void switchUnits(temp, e.currentTarget.value as NetUnit)
                }
              >
                {NET_UNITS.map((u) => (
                  <option key={u.id} value={u.id}>
                    {u.name} (shows {u.example})
                  </option>
                ))}
              </select>
            </Field>
          </div>
          {notice && (
            <p className="muted hint" role="status">
              {notice}
            </p>
          )}
          {convertError && (
            <p className="problems" role="alert">
              {convertError}
            </p>
          )}
        </section>

        <section>
          <h4>GPU</h4>
          <Field label="GPU shown" problems={errors.gpu}>
            <select
              value={gpu}
              onChange={(e) => onChange(withGpu(bar, e.currentTarget.value))}
            >
              <option value="auto">
                Auto (currently: {gpus ? (autoName ?? "none readable") : "…"})
              </option>
              <option value="off">Off</option>
              {gpus?.gpus.map((g) => (
                <option key={g.id} value={g.id} disabled={!g.readable}>
                  {g.name} ({g.id}
                  {g.readable
                    ? ""
                    : `, can't be read${g.driver ? ` with the ${g.driver} driver` : ""}`}
                  )
                </option>
              ))}
              {!listed && gpu !== "auto" && gpu !== "off" && (
                <option value={gpu}>
                  {gpu}
                  {gpus ? " (not found)" : ""}
                </option>
              )}
            </select>
          </Field>
          {gpuError && (
            <p className="problems" role="alert">
              {gpuError}
            </p>
          )}
        </section>

        <section>
          <h4>Colors past a threshold</h4>
          <p className="muted hint">
            A value turns the color of the highest step it has reached. Steps
            are in the units above.
          </p>
          {METRICS.map((metric) => {
            const own = bar?.thresholds?.[metric.id];
            const steps = own ?? defaults[temp][metric.id];
            const unit = unitOf(metric);
            const problems = errors[`thresholds.${metric.id}`];
            return (
              <div className="metric-row" key={metric.id}>
                <div className="metric-head">
                  <strong>{metric.name}</strong>
                  <span className="muted">steps in {unit}</span>
                  {own === undefined && <span className="chip">defaults</span>}
                </div>
                {steps.length === 0 && (
                  <span className="muted">Never colored.</span>
                )}
                {steps.map((step, i) => (
                  // biome-ignore lint/suspicious/noArrayIndexKey: steps have no id, and rows are edited in place
                  <div className="step-row" key={i}>
                    {own === undefined ? (
                      <span className="mono">
                        {step.at} {unit}{" "}
                        <span
                          className="swatch"
                          style={{ background: step.color }}
                        />{" "}
                        {step.color}
                      </span>
                    ) : (
                      <>
                        <Field
                          label={`At (${unit}), ${metric.name}, step ${i + 1}`}
                        >
                          <NumberField
                            value={step.at}
                            onChange={(at) =>
                              onChange(
                                withSteps(
                                  bar,
                                  metric.id,
                                  own.map((s, j) =>
                                    j === i ? { ...s, at } : s,
                                  ),
                                ),
                              )
                            }
                          />
                        </Field>
                        <ColorField
                          label={`Color, ${metric.name}, step ${i + 1}`}
                          value={step.color}
                          onChange={(color) =>
                            onChange(
                              withSteps(
                                bar,
                                metric.id,
                                own.map((s, j) =>
                                  j === i ? { ...s, color } : s,
                                ),
                              ),
                            )
                          }
                        />
                        <button
                          type="button"
                          aria-label={`Remove ${metric.name} step ${i + 1}`}
                          onClick={() =>
                            onChange(
                              withSteps(
                                bar,
                                metric.id,
                                own.filter((_, j) => j !== i),
                              ),
                            )
                          }
                        >
                          Remove
                        </button>
                      </>
                    )}
                  </div>
                ))}
                <div className="outside">
                  {own === undefined ? (
                    <button
                      type="button"
                      aria-label={`Edit ${metric.name} steps`}
                      onClick={() =>
                        onChange(
                          withSteps(
                            bar,
                            metric.id,
                            steps.map((s) => ({ ...s })),
                          ),
                        )
                      }
                    >
                      Edit steps
                    </button>
                  ) : (
                    <>
                      <button
                        type="button"
                        aria-label={`Add ${metric.name} step`}
                        disabled={own.length >= MAX_STEPS}
                        onClick={() =>
                          onChange(
                            withSteps(bar, metric.id, [
                              ...own,
                              {
                                at:
                                  own.length > 0
                                    ? own[own.length - 1].at + 1
                                    : firstAt(metric),
                                color: AMBER,
                              },
                            ]),
                          )
                        }
                      >
                        Add step
                      </button>
                      <button
                        type="button"
                        aria-label={`Use default ${metric.name} steps`}
                        onClick={() =>
                          onChange(withSteps(bar, metric.id, undefined))
                        }
                      >
                        Use defaults
                      </button>
                    </>
                  )}
                </div>
                <Problems list={problems} />
              </div>
            );
          })}
        </section>
      </fieldset>
    </div>
  );
}
