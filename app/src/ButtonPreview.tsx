import { type CSSProperties, useState } from "react";
import type { ButtonConfig } from "./config";
import type { ButtonLayout, Rect, TextBox, TileState } from "./preview";

function Text({ box, width }: { box: TextBox; width: number }) {
  return (
    <div
      className="dev-text"
      style={{
        width,
        height: box.height,
        fontSize: box.size,
        lineHeight: `${box.lineHeight}px`,
      }}
    >
      {box.lines.map((line, i) => (
        // biome-ignore lint/suspicious/noArrayIndexKey: lines are positional
        <div className="dev-line" key={i}>
          {line}
        </div>
      ))}
    </div>
  );
}

/** One button as the device draws it, at the device's pixel size (the stage scales it). */
export default function ButtonPreview({
  button,
  index,
  rect,
  layout,
  tile,
  iconSrc,
  selected,
  invalid,
  dragging,
}: {
  button: ButtonConfig;
  index: number;
  rect: Rect;
  layout: ButtonLayout;
  /** The tile's state, or null if the button has no tile. */
  tile: TileState | null;
  iconSrc: string | null;
  selected: boolean;
  invalid: boolean;
  dragging: boolean;
}) {
  const style: CSSProperties = {
    left: rect.x,
    top: rect.y,
    width: rect.w,
    height: rect.h,
    color: button.fg,
    // The configured color shows on a plain button, and on a tile only while it is ok.
    background: tile === null || tile === "ok" ? button.bg : undefined,
  };
  const classes = [
    "dev-button",
    tile === "unknown" && "tile-unknown",
    tile === "bad" && "tile-bad",
    selected && "selected",
    invalid && "invalid",
    dragging && "dragging",
  ]
    .filter(Boolean)
    .join(" ");
  // The file that failed to load, so a different icon later still shows.
  const [failed, setFailed] = useState<string | null>(null);
  const width = rect.w - 16;
  return (
    // A div, not a button: the stage drags it, and its lines are block content.
    // biome-ignore lint/a11y/useSemanticElements: see above
    <div
      className={classes}
      style={style}
      data-index={index}
      role="button"
      tabIndex={0}
      aria-label={`${button.id}, ${button.label}`}
      aria-pressed={selected}
    >
      {iconSrc && failed !== iconSrc && layout.icon !== null && (
        <img
          className="dev-icon"
          src={iconSrc}
          alt=""
          draggable={false}
          style={
            iconSrc.endsWith(".svg")
              ? // The device draws an SVG to fill its box, so the preview does too.
                {
                  width: layout.icon,
                  height: layout.icon,
                  objectFit: "contain",
                }
              : { maxWidth: layout.icon, maxHeight: layout.icon }
          }
          onError={() => setFailed(iconSrc)}
        />
      )}
      {layout.label && <Text box={layout.label} width={width} />}
      {layout.value && <Text box={layout.value} width={width} />}
    </div>
  );
}
