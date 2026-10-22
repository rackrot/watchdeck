import { useCallback, useEffect, useState } from "react";
import ActionForm from "./ActionForm";
import { type ActionResult, testAction } from "./api";
import { type ButtonConfig, newShell } from "./config";
import type { ActionField, FieldErrors } from "./errors";
import { Field, FontSelect, NumberField, Problems } from "./fields";
import { DEFAULT_FONT, resolveFonts } from "./fonts";

const MAX_LABEL_BYTES = 47;

type Props = {
  button: ButtonConfig;
  /** The grid's font id (`layout.font`), which a button without its own font inherits. */
  gridFont: string | undefined;
  /** The pages, as `[[page]]` ids and the names their tabs show. The select shows with 2 or more. */
  pages: { id: string; name: string }[];
  /** The id of the page the button is on. */
  pageId: string;
  /** The id of the button in this button's cell on page `id`, if there is one. */
  occupant: (id: string) => string | undefined;
  onChange: (button: ButtonConfig) => void;
  onDelete: () => void;
  /** Rust's complaints about this button, from the last save. */
  errors: FieldErrors;
  secrets: string[];
  /** Icon files the picker offers, besides none. */
  icons: string[];
  iconUrl: (name: string) => string | null;
  onImportIcon: () => void;
  iconError: string;
  /** Why Test is disabled (the draft differs from the saved button), or null if it can run. */
  testBlock: string | null;
  /** Problems the form can see itself, which keep a save from going out. */
  onLocalProblems: (messages: string[]) => void;
};

function ColorField({
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

export default function ButtonForm({
  button,
  gridFont,
  pages,
  pageId,
  occupant,
  onChange,
  onDelete,
  errors,
  secrets,
  icons,
  iconUrl,
  onImportIcon,
  iconError,
  testBlock,
  onLocalProblems,
}: Props) {
  const [dupes, setDupes] = useState<{ action: string[]; source: string[] }>({
    action: [],
    source: [],
  });
  const onActionDupes = useCallback(
    (action: string[]) => setDupes((d) => ({ ...d, action })),
    [],
  );
  const onSourceDupes = useCallback(
    (source: string[]) => setDupes((d) => ({ ...d, source })),
    [],
  );
  useEffect(() => {
    onLocalProblems([...dupes.action, ...dupes.source]);
    return () => onLocalProblems([]);
  }, [dupes, onLocalProblems]);
  // Why a move to another page was refused, until the next try.
  const [moveRefused, setMoveRefused] = useState("");
  function moveTo(id: string) {
    const other = occupant(id);
    if (other !== undefined) {
      const name = pages.find((p) => p.id === id)?.name ?? id;
      setMoveRefused(
        `Not moved: column ${button.pos[0]}, row ${button.pos[1]} on ${name} is taken by ${other}. Move one of them first.`,
      );
      return;
    }
    setMoveRefused("");
    set({ page: id });
  }
  const [testing, setTesting] = useState(false);
  const [test, setTest] = useState<
    { result: ActionResult } | { error: string } | null
  >(null);
  async function runTest() {
    setTesting(true);
    setTest(null);
    try {
      setTest({ result: await testAction(button.id) });
    } catch (e) {
      setTest({ error: String(e) });
    } finally {
      setTesting(false);
    }
  }
  const set = (part: Partial<ButtonConfig>) => onChange({ ...button, ...part });
  const actionProblems = (field: ActionField) => errors[`action.${field}`];
  const tile = button.tile;
  const labelBytes = new TextEncoder().encode(button.label).length;
  const iconSrc = button.icon ? iconUrl(button.icon) : null;
  // The tile value's picker inherits the button's font, which itself may be inherited.
  const buttonFont = resolveFonts(
    { font: gridFont },
    { font: button.font },
  ).label;

  return (
    <div className="button-form">
      <div className="panel-head">
        <h3>
          Button <span className="mono id">{button.id}</span>
        </h3>
        <button type="button" className="danger" onClick={onDelete}>
          Delete button
        </button>
      </div>
      <Problems list={errors.button} />
      {/* With fewer than 2 pages there is no select, so a complaint about `page` shows here. */}
      {pages.length < 2 && <Problems list={errors.page} />}

      <section>
        <h4>Look</h4>
        <div className="form-grid">
          <Field label="Id" hint="letters, digits, _ - ." problems={errors.id}>
            <input
              className="mono"
              value={button.id}
              spellCheck={false}
              onChange={(e) => set({ id: e.currentTarget.value })}
            />
          </Field>
          <Field
            label="Label"
            hint={`${labelBytes}/${MAX_LABEL_BYTES}`}
            problems={errors.label}
          >
            <input
              value={button.label}
              onChange={(e) => set({ label: e.currentTarget.value })}
            />
          </Field>
          <Field label="Column" problems={errors.pos}>
            <NumberField
              value={button.pos[0]}
              onChange={(col) => set({ pos: [col, button.pos[1]] })}
            />
          </Field>
          <Field label="Row">
            <NumberField
              value={button.pos[1]}
              onChange={(row) => set({ pos: [button.pos[0], row] })}
            />
          </Field>
          {pages.length >= 2 && (
            <Field
              label="Page"
              problems={[
                ...(errors.page ?? []),
                ...(moveRefused ? [moveRefused] : []),
              ]}
            >
              <select
                value={pageId}
                onChange={(e) => moveTo(e.currentTarget.value)}
              >
                {pages.map((p) => (
                  <option key={p.id} value={p.id}>
                    {p.name}
                  </option>
                ))}
              </select>
            </Field>
          )}
          <ColorField
            label="Background"
            value={button.bg}
            problems={errors.bg}
            onChange={(bg) => set({ bg })}
          />
          <ColorField
            label="Text"
            value={button.fg}
            problems={errors.fg}
            onChange={(fg) => set({ fg })}
          />
          <Field label="Font" problems={errors.font}>
            <FontSelect
              value={button.font}
              inherit={{
                label: "Grid font",
                font: gridFont ?? DEFAULT_FONT,
              }}
              onChange={(font) => set({ font })}
            />
          </Field>
          <Field label="Icon" problems={errors.icon}>
            <span className="icon-pick">
              <select
                className="mono"
                value={button.icon ?? ""}
                onChange={(e) =>
                  set({ icon: e.currentTarget.value || undefined })
                }
              >
                <option value="">(none)</option>
                {icons.map((name) => (
                  <option key={name}>{name}</option>
                ))}
              </select>
              {iconSrc && <img className="thumb" src={iconSrc} alt="" />}
              <button type="button" onClick={onImportIcon}>
                Import…
              </button>
            </span>
          </Field>
        </div>
        {iconError && (
          <p className="problems" role="alert">
            {iconError}
          </p>
        )}
      </section>

      <section>
        <h4>On press</h4>
        <ActionForm
          action={button.action}
          secrets={secrets}
          problems={actionProblems}
          onDuplicates={onActionDupes}
          onChange={(action) => set({ action })}
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
        {test && "error" in test && (
          <p className="problems" role="alert">
            {test.error}
          </p>
        )}
        {test && "result" in test && (
          <div role="status">
            <span className={`chip ${test.result.ok ? "ok" : "bad"}`}>
              {test.result.ok ? "ok" : "failed"}: {test.result.summary}
              {test.result.ms > 0 && ` (${test.result.ms} ms)`}
            </span>
            {test.result.text && <pre className="mono">{test.result.text}</pre>}
          </div>
        )}
      </section>

      <section>
        <h4>Live tile</h4>
        <label className="check">
          <input
            type="checkbox"
            checked={tile !== undefined}
            onChange={(e) =>
              set({
                tile: e.currentTarget.checked
                  ? { every_s: 15, source: newShell() }
                  : undefined,
              })
            }
          />
          Show a value polled from a source while a device is connected
        </label>
        {tile && (
          <>
            <div className="form-grid">
              <Field
                label="Poll every"
                hint="seconds, 2 to 86400"
                problems={errors["tile.every_s"]}
              >
                <NumberField
                  value={tile.every_s}
                  onChange={(every_s) => set({ tile: { ...tile, every_s } })}
                />
              </Field>
              <Field
                label="JSON pointer"
                hint="optional, like /state"
                problems={errors["tile.json_pointer"]}
              >
                <input
                  className="mono"
                  value={tile.json_pointer ?? ""}
                  spellCheck={false}
                  onChange={(e) =>
                    set({
                      tile: {
                        ...tile,
                        json_pointer: e.currentTarget.value || undefined,
                      },
                    })
                  }
                />
              </Field>
              <Field label="Value font" problems={errors["tile.font"]}>
                <FontSelect
                  value={tile.font}
                  inherit={{ label: "Button font", font: buttonFont }}
                  onChange={(font) => set({ tile: { ...tile, font } })}
                />
              </Field>
            </div>
            <h5>Source</h5>
            <ActionForm
              action={tile.source}
              secrets={secrets}
              problems={(field) => errors[`tile.source.${field}`]}
              onDuplicates={onSourceDupes}
              onChange={(source) => set({ tile: { ...tile, source } })}
            />
          </>
        )}
      </section>
    </div>
  );
}
