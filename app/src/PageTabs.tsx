import { useState } from "react";
import type { Config } from "./config";
import {
  addPage,
  buttonsOnPage,
  MAX_PAGES,
  movePage,
  pageCount,
  pageName,
  removePage,
  renamePage,
} from "./edit";
import { Field, Problems } from "./fields";

const MAX_LABEL = 24;

type Props = {
  config: Config;
  /** The page shown, from 0. */
  page: number;
  /** Rust's complaints about pages, by page id. */
  errors: Map<string, string[]>;
  /** The new config, and the page to show after it. */
  onChange: (config: Config, page: number) => void;
  onSelect: (page: number) => void;
  /** The Feed tab shows once the config has an integration or the user adds one. */
  feedShown: boolean;
  /** The Feed tab is the one open, so no page is. */
  feedOpen: boolean;
  /** An integration has a complaint from the last save. */
  feedBad: boolean;
  /** Opens the Feed tab, and makes it appear if it wasn't there. */
  onFeed: () => void;
};

/**
 * The tabs above the grid editor: the Feed tab first when there is one, then the pages. Pick a
 * page, add one (up to 4), name it, move it left or right, delete it. With one page and no feed
 * there are no tabs, only the buttons that add a page and a feed.
 */
export default function PageTabs({
  config,
  page,
  errors,
  onChange,
  onSelect,
  feedShown,
  feedOpen,
  feedBad,
  onFeed,
}: Props) {
  const [confirming, setConfirming] = useState(false);
  const count = pageCount(config);
  const pages = config.page ?? [];
  const full = count >= MAX_PAGES;
  const add = () => onChange(addPage(config), count);
  const addFeed = !feedShown && (
    <button type="button" onClick={onFeed}>
      Add feed
    </button>
  );

  if (count < 2 && !feedShown) {
    return (
      <div className="page-tabs">
        <div className="tab-row">
          <button type="button" onClick={add}>
            Add page
          </button>
          {addFeed}
          <span className="muted hint">
            Pages give the device more buttons. Swipe on it to change page. A
            feed shows notifications and values one swipe to the right of page
            1.
          </span>
        </div>
        <Problems list={[...errors.values()].flat()} />
      </div>
    );
  }

  const current = feedOpen || count < 2 ? undefined : pages[page];
  const doomed = buttonsOnPage(config, page);
  const problems = [...errors].flatMap(([id, list]) => {
    const owner = pages.find((p) => p.id === id);
    return list.map((m) => `${owner ? pageName(owner) : id}: ${m}`);
  });

  function go(next: number) {
    setConfirming(false);
    onSelect(next);
  }

  return (
    <div className="page-tabs">
      <div className="tab-row" role="tablist" aria-label="Pages">
        {feedShown && (
          <button
            type="button"
            role="tab"
            aria-selected={feedOpen}
            className={["tab", feedOpen && "selected", feedBad && "bad"]
              .filter(Boolean)
              .join(" ")}
            onClick={() => {
              setConfirming(false);
              onFeed();
            }}
          >
            Feed
          </button>
        )}
        {count < 2 ? (
          <button
            type="button"
            role="tab"
            aria-selected={!feedOpen}
            className={["tab", !feedOpen && "selected"]
              .filter(Boolean)
              .join(" ")}
            onClick={() => go(0)}
          >
            Buttons
          </button>
        ) : (
          pages.map((p, i) => (
            <button
              type="button"
              role="tab"
              aria-selected={!feedOpen && i === page}
              className={[
                "tab",
                !feedOpen && i === page && "selected",
                errors.has(p.id) && "bad",
              ]
                .filter(Boolean)
                .join(" ")}
              key={p.id}
              onClick={() => go(i)}
            >
              {pageName(p)}
            </button>
          ))
        )}
      </div>
      <div className="tab-row">
        <button type="button" disabled={full} onClick={add}>
          {full ? `Add page (at most ${MAX_PAGES})` : "Add page"}
        </button>
        {addFeed}
      </div>

      {current && (
        <div className="page-actions">
          <Field
            label="Page name"
            hint={`${(current.label ?? "").length}/${MAX_LABEL}`}
          >
            <input
              value={current.label ?? ""}
              placeholder={current.id}
              maxLength={MAX_LABEL}
              spellCheck={false}
              onChange={(e) =>
                onChange(renamePage(config, page, e.currentTarget.value), page)
              }
            />
          </Field>
          <button
            type="button"
            disabled={page === 0}
            aria-label="Move page left"
            onClick={() => {
              setConfirming(false);
              onChange(movePage(config, page, -1), page - 1);
            }}
          >
            Move left
          </button>
          <button
            type="button"
            disabled={page === count - 1}
            aria-label="Move page right"
            onClick={() => {
              setConfirming(false);
              onChange(movePage(config, page, 1), page + 1);
            }}
          >
            Move right
          </button>
          <button
            type="button"
            className="danger"
            onClick={() => setConfirming(true)}
          >
            Delete page
          </button>
        </div>
      )}
      <Problems list={problems} />

      {confirming && current && (
        <div
          className="banner stale"
          role="alertdialog"
          aria-label="Confirm deleting the page"
        >
          <span>
            Delete page <strong>{pageName(current)}</strong>?{" "}
            {doomed.length === 0 ? (
              "It has no buttons."
            ) : (
              <>
                This also removes{" "}
                {doomed.length === 1 ? "its button" : "its buttons"}{" "}
                <span className="mono">
                  {doomed.map((b) => b.id).join(", ")}
                </span>
                .
              </>
            )}
          </span>
          <button
            type="button"
            className="danger"
            onClick={() => {
              setConfirming(false);
              onChange(removePage(config, page), Math.max(0, page - 1));
            }}
          >
            Delete page
          </button>
          <button type="button" onClick={() => setConfirming(false)}>
            Cancel
          </button>
        </div>
      )}
    </div>
  );
}
