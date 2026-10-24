import { useEffect, useState } from "react";
import type { FeedCard, FeedNote } from "./api";
import { DEFAULT_FONT, fontInfo, slotFont } from "./fonts";
import { useFeed, useWidth } from "./hooks";
import { dotLines, textWidth } from "./lvtext";
import {
  FEED_AGE_W,
  FEED_AREA_H,
  FEED_CARD_GAP,
  FEED_CARD_H,
  FEED_CARD_W,
  FEED_CARDS_PER_ROW,
  FEED_EDGE_W,
  FEED_META_GAP,
  FEED_MORE_H,
  FEED_NOTE_H,
  FEED_PAD,
  FEED_ROW_H,
  FEED_ROW_PAD,
  FEED_ROW_W,
  FEED_SIDE,
  FEED_SRC_W,
  FEED_TITLE_W,
  feedAge,
  feedNoteRows,
  feedNotesTop,
  SCREEN_W,
} from "./preview";

const unixNow = () => Math.floor(Date.now() / 1000);

/** The time in Unix seconds, read again every `ms`, so the ages move as a board's do. */
function useNow(ms: number): number {
  const [now, setNow] = useState(unixNow);
  useEffect(() => {
    const timer = setInterval(() => setNow(unixNow()), ms);
    return () => clearInterval(timer);
  }, [ms]);
  return now;
}

/** One line of `text` in the slot, cut with dots as a fixed-size LVGL label cuts it. */
function line(text: string, slot: 24 | 32, width: number): string {
  const font = slotFont(DEFAULT_FONT, slot);
  return dotLines(text, font, width, font.lineHeight)[0] ?? "";
}

function Card({ card, index }: { card: FeedCard; index: number }) {
  const width = FEED_CARD_W - 2 * FEED_ROW_PAD;
  // The value is 32 pt unless that is wider than the card, then 24 pt (add_card).
  const slot =
    textWidth(slotFont(DEFAULT_FONT, 32), card.value) > width ? 24 : 32;
  const state =
    card.state === "ok" || card.state === "bad" ? card.state : "unknown";
  return (
    <div
      className={`feed-card ${state}`}
      style={{
        left:
          FEED_SIDE +
          (index % FEED_CARDS_PER_ROW) * (FEED_CARD_W + FEED_CARD_GAP),
        top:
          FEED_PAD +
          Math.floor(index / FEED_CARDS_PER_ROW) *
            (FEED_CARD_H + FEED_CARD_GAP),
        width: FEED_CARD_W,
        height: FEED_CARD_H,
      }}
    >
      <div
        className="feed-card-label"
        style={{ top: 14, left: FEED_ROW_PAD, width }}
      >
        {line(card.label, 24, width)}
      </div>
      <div
        className="feed-card-value"
        style={{
          top: 14 + slotFont(DEFAULT_FONT, 24).lineHeight + 8,
          left: FEED_ROW_PAD,
          width,
          fontSize: slot,
        }}
      >
        {line(card.value, slot, width)}
      </div>
    </div>
  );
}

function Row({ note, top, now }: { note: FeedNote; top: number; now: number }) {
  const ageX = FEED_ROW_W - FEED_EDGE_W - FEED_ROW_PAD - FEED_AGE_W;
  return (
    <div
      className={`feed-row ${note.level}`}
      style={{ left: FEED_SIDE, top, width: FEED_ROW_W, height: FEED_ROW_H }}
    >
      <div
        className="feed-title"
        style={{ left: FEED_ROW_PAD, width: FEED_TITLE_W }}
      >
        {line(note.title, 24, FEED_TITLE_W)}
      </div>
      <div
        className="feed-meta"
        style={{ left: ageX - FEED_META_GAP - FEED_SRC_W, width: FEED_SRC_W }}
      >
        {note.src}
      </div>
      <div className="feed-meta" style={{ left: ageX, width: FEED_AGE_W }}>
        {feedAge(note.at, now)}
      </div>
    </div>
  );
}

/**
 * The -1 page as a board draws it, from the daemon's live snapshot (`feed-changed`): up to 8
 * cards, then the note rows that fit and a "+N more" line, or "Nothing here yet". It shows what
 * is saved and running, not the draft.
 */
export default function FeedPreview() {
  const feed = useFeed();
  const now = useNow(10_000);
  const [stage, stageWidth] = useWidth();
  const scale = Math.min(1, stageWidth / SCREEN_W);
  const { rows, more } = feedNoteRows(feed.cards.length, feed.notes.length);
  const top = feedNotesTop(feed.cards.length);
  const family = fontInfo(DEFAULT_FONT).family;

  return (
    <div className="feed-preview">
      <h4>On the board</h4>
      <div
        className="stage"
        ref={stage}
        style={{ height: FEED_AREA_H * scale }}
      >
        <div
          className="screen feed-screen"
          style={{
            width: SCREEN_W,
            height: FEED_AREA_H,
            transform: `scale(${scale})`,
            fontFamily: family,
          }}
          role="img"
          aria-label={`Feed page preview: ${feed.cards.length} cards, ${feed.notes.length} notes`}
        >
          {feed.cards.map((card, i) => (
            <Card key={card.id} card={card} index={i} />
          ))}
          {feed.notes.slice(0, rows).map((note, i) => (
            <Row
              key={note.id}
              note={note}
              top={top + i * FEED_NOTE_H}
              now={now}
            />
          ))}
          {more > 0 && (
            <div
              className="feed-more"
              style={{ top: top + rows * FEED_NOTE_H, height: FEED_MORE_H }}
            >
              +{more} more
            </div>
          )}
          {feed.cards.length === 0 && feed.notes.length === 0 && (
            <div className="feed-empty">Nothing here yet</div>
          )}
        </div>
      </div>
      <p className="muted hint">
        {feed.sources === 0
          ? "No integration is saved, so a board has no feed page."
          : "Live from the daemon: a push or a poll shows here as it reaches the boards."}
      </p>
    </div>
  );
}
