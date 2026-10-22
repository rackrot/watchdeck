import {
  type KeyboardEvent,
  type PointerEvent,
  type ReactNode,
  useEffect,
  useRef,
  useState,
} from "react";
import type { TileReading } from "./api";
import ButtonPreview from "./ButtonPreview";
import type { Config } from "./config";
import { canon, moveButton } from "./edit";
import type { FieldErrors } from "./errors";
import { Field, NumberField, Problems } from "./fields";
import {
  cellAt,
  cellRect,
  cellSize,
  GRID_H,
  layoutButton,
  MAX_COLS,
  MAX_ROWS,
  SCREEN_W,
  type TileState,
} from "./preview";

/** A press that moves less than this many screen pixels is a click, not a drag. */
const DRAG_SLOP = 6;

type Drag = { index: number; moved: boolean; over: [number, number] | null };

type Props = {
  config: Config;
  selected: number | null;
  /** Rust's complaints, by button id. */
  errors: Map<string, FieldErrors>;
  gridProblems: string[];
  readings: TileReading[];
  iconUrl: (name: string) => string | null;
  onSelect: (index: number | null) => void;
  onChange: (config: Config) => void;
  onAdd: (pos: [number, number]) => void;
};

/** The width of an element, kept current. */
function useWidth() {
  const ref = useRef<HTMLDivElement>(null);
  const [width, setWidth] = useState(SCREEN_W);
  useEffect(() => {
    const el = ref.current;
    if (!el) {
      return;
    }
    const watch = new ResizeObserver(() => setWidth(el.clientWidth));
    watch.observe(el);
    setWidth(el.clientWidth);
    return () => watch.disconnect();
  }, []);
  return [ref, width] as const;
}

export default function GridEditor({
  config,
  selected,
  errors,
  gridProblems,
  readings,
  iconUrl,
  onSelect,
  onChange,
  onAdd,
}: Props) {
  const [stage, stageWidth] = useWidth();
  const screen = useRef<HTMLDivElement>(null);
  const press = useRef({ x: 0, y: 0 });
  const [drag, setDrag] = useState<Drag | null>(null);

  const { cols, rows } = config.layout;
  const gridOk = cols >= 1 && rows >= 1 && cols <= MAX_COLS && rows <= MAX_ROWS;
  const scale = Math.min(1, stageWidth / SCREEN_W);
  const cell = gridOk ? cellSize(cols, rows) : null;
  const inside = (pos: [number, number]) => pos[0] < cols && pos[1] < rows;
  const taken = new Set(
    config.button.filter((b) => inside(b.pos)).map((b) => `${b.pos}`),
  );
  const outside = config.button
    .map((b, index) => ({ b, index }))
    .filter(({ b }) => !inside(b.pos));

  function toGrid(e: PointerEvent): [number, number] {
    const box = screen.current?.getBoundingClientRect();
    return box
      ? [(e.clientX - box.left) / scale, (e.clientY - box.top) / scale]
      : [0, 0];
  }

  function down(e: PointerEvent<HTMLDivElement>) {
    const el = (e.target as HTMLElement).closest<HTMLElement>("[data-index]");
    if (!el) {
      return;
    }
    e.currentTarget.setPointerCapture(e.pointerId);
    press.current = { x: e.clientX, y: e.clientY };
    setDrag({ index: Number(el.dataset.index), moved: false, over: null });
  }

  function move(e: PointerEvent) {
    if (!drag) {
      return;
    }
    const moved =
      drag.moved ||
      Math.hypot(e.clientX - press.current.x, e.clientY - press.current.y) >
        DRAG_SLOP;
    const [x, y] = toGrid(e);
    setDrag({ ...drag, moved, over: moved ? cellAt(x, y, cols, rows) : null });
  }

  function up() {
    if (!drag) {
      return;
    }
    if (!drag.moved) {
      onSelect(drag.index);
    } else if (drag.over) {
      onChange(moveButton(config, drag.index, drag.over));
      onSelect(drag.index);
    }
    setDrag(null);
  }

  function key(e: KeyboardEvent<HTMLDivElement>) {
    const el = (e.target as HTMLElement).closest<HTMLElement>("[data-index]");
    if (el && (e.key === "Enter" || e.key === " ")) {
      e.preventDefault();
      onSelect(Number(el.dataset.index));
    }
  }

  const cells: ReactNode[] = [];
  if (cell) {
    for (let row = 0; row < rows; row++) {
      for (let col = 0; col < cols; col++) {
        const rect = cellRect(col, row, cell);
        const box = {
          left: rect.x,
          top: rect.y,
          width: rect.w,
          height: rect.h,
        };
        const target = drag?.over?.[0] === col && drag.over[1] === row;
        cells.push(
          <div key={`${col},${row}`}>
            {!taken.has(`${[col, row]}`) && (
              <button
                type="button"
                className="dev-empty"
                style={box}
                aria-label={`Add a button at column ${col}, row ${row}`}
                onClick={() => onAdd([col, row])}
              >
                +
              </button>
            )}
            {target && <div className="drop-target" style={box} />}
          </div>,
        );
      }
    }
  }

  function reading(index: number): TileReading | undefined {
    const b = config.button[index];
    return readings.find(
      (r) => r.btn === b.id && canon(r.tile) === canon(b.tile),
    );
  }

  return (
    <section className="grid-editor">
      <div className="panel-head">
        <h3>Grid</h3>
        <div className="size-controls">
          <Field label="Columns" hint="1 to 8">
            <NumberField
              value={cols}
              onChange={(n) =>
                onChange({ ...config, layout: { ...config.layout, cols: n } })
              }
            />
          </Field>
          <Field label="Rows" hint="1 to 5">
            <NumberField
              value={rows}
              onChange={(n) =>
                onChange({ ...config, layout: { ...config.layout, rows: n } })
              }
            />
          </Field>
        </div>
      </div>
      <Problems list={gridProblems} />

      <div className="stage" ref={stage} style={{ height: GRID_H * scale }}>
        {/* The device's pixels, scaled to fit. Wrapping is computed for these, not for the scale. */}
        {/* biome-ignore lint/a11y/useSemanticElements: a fieldset can't be scaled and dragged on */}
        <div
          className="screen"
          ref={screen}
          style={{
            width: SCREEN_W,
            height: GRID_H,
            transform: `scale(${scale})`,
          }}
          onPointerDown={down}
          onPointerMove={move}
          onPointerUp={up}
          onPointerCancel={() => setDrag(null)}
          onKeyDown={key}
          role="group"
          aria-label="Device grid"
        >
          {cells}
          {cell &&
            config.button.map((b, index) => {
              if (!inside(b.pos)) {
                return null;
              }
              const found = reading(index);
              const tile: TileState | null = b.tile
                ? (found?.state ?? "unknown")
                : null;
              return (
                <ButtonPreview
                  // biome-ignore lint/suspicious/noArrayIndexKey: ids are editable, places are not
                  key={index}
                  button={b}
                  index={index}
                  rect={cellRect(b.pos[0], b.pos[1], cell)}
                  layout={layoutButton(
                    {
                      label: b.label,
                      hasIcon: b.icon !== undefined,
                      isTile: b.tile !== undefined,
                    },
                    cols,
                    rows,
                    found?.text,
                  )}
                  tile={tile}
                  iconSrc={b.icon ? iconUrl(b.icon) : null}
                  selected={selected === index}
                  invalid={errors.has(b.id)}
                  dragging={drag?.index === index && drag.moved}
                />
              );
            })}
        </div>
      </div>

      {!gridOk && (
        <p className="muted">
          Columns run 1 to {MAX_COLS} and rows 1 to {MAX_ROWS}. Fix the size to
          see the grid.
        </p>
      )}
      {(gridOk ? outside : config.button.map((b, index) => ({ b, index })))
        .length > 0 && (
        <div className="outside">
          <span className="muted">
            {gridOk ? "Outside the grid:" : "Buttons:"}
          </span>
          {(gridOk
            ? outside
            : config.button.map((b, index) => ({ b, index }))
          ).map(({ b, index }) => (
            <button
              type="button"
              className={[
                "chip",
                selected === index && "selected",
                errors.has(b.id) && "bad",
              ]
                .filter(Boolean)
                .join(" ")}
              key={index}
              onClick={() => onSelect(index)}
            >
              <span className="mono">{b.id}</span> at {b.pos[0]},{b.pos[1]}
              {errors.has(b.id) && " (has problems)"}
            </button>
          ))}
        </div>
      )}
      <p className="muted hint">
        Click a cell to add a button, a button to edit it, or drag a button to
        move it. Dropping on another button swaps them.
      </p>
    </section>
  );
}
