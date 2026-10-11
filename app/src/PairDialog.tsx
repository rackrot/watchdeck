import { listen } from "@tauri-apps/api/event";
import { type FormEvent, useEffect, useRef, useState } from "react";
import {
  cancelPairing,
  confirmPairing,
  type PairingState,
  pairingState,
  startPairing,
} from "./api";

/**
 * Opens the pairing port and shows its progress. Closing the dialog, by button or Escape, closes
 * the port. With `resume` it shows a port that is already open, as after a page reload.
 */
export default function PairDialog({
  resume,
  onClose,
}: {
  resume: boolean;
  onClose: () => void;
}) {
  const dialog = useRef<HTMLDialogElement>(null);
  const input = useRef<HTMLInputElement>(null);
  const [state, setState] = useState<PairingState | null>(null);
  const [code, setCode] = useState("");
  const [error, setError] = useState("");

  useEffect(() => {
    if (dialog.current && !dialog.current.open) {
      dialog.current.showModal();
    }
    let live = true;
    let events = 0;
    // A new state supersedes the error of the last action.
    const show = (next: PairingState) => {
      setState(next);
      setError("");
    };
    const unlisten = listen<PairingState>("pairing", (e) => {
      events++;
      show(e.payload);
    });
    // Open the port only once the listener is in place, then read the state in case an event
    // came first. An event that lands during the read is newer, so it wins.
    unlisten
      .then(async () => {
        if (!live) {
          return;
        }
        if (!resume) {
          await startPairing();
        }
        const seen = events;
        const now = await pairingState();
        if (live && events === seen) {
          show(now);
        }
      })
      .catch((e) => {
        if (live) {
          setError(String(e));
        }
      });
    return () => {
      live = false;
      unlisten.then((stop) => stop());
    };
  }, [resume]);

  useEffect(() => {
    if (state?.state === "confirm") {
      input.current?.focus();
    }
  }, [state?.state]);

  function closed() {
    // The dialog is gone by the time this settles, so a failure can only be logged. The daemon
    // still closes the window on its own deadline.
    cancelPairing().catch((e) => console.error("cancel_pairing failed:", e));
    onClose();
  }

  async function submit(e: FormEvent) {
    e.preventDefault();
    setError("");
    try {
      await confirmPairing(code.replace(/\s/g, ""));
    } catch (err) {
      setError(String(err));
    }
  }

  const open =
    state === null || state.state === "waiting" || state.state === "confirm";
  return (
    <dialog ref={dialog} onClose={closed}>
      <h2>Pair a device</h2>
      {state?.state === "waiting" && (
        <p>
          On the device, tap this computer in the list. Pairing stays open for 2
          minutes.
        </p>
      )}
      {state?.state === "confirm" && (
        <form onSubmit={submit}>
          <p>
            Type the 6-digit code shown on {state.device} ({state.peer}).
          </p>
          <div className="row">
            <input
              ref={input}
              inputMode="numeric"
              autoComplete="off"
              maxLength={7}
              aria-label="Pairing code"
              value={code}
              onChange={(e) => setCode(e.currentTarget.value)}
            />
            <button type="submit">Pair</button>
          </div>
        </form>
      )}
      {state?.state === "paired" && (
        <p>Paired {state.device}. Tap Yes on the device to finish.</p>
      )}
      {state?.state === "failed" && <p role="alert">{state.reason}</p>}
      {state?.state === "closed" && <p>Pairing closed.</p>}
      {/* Once the window ends, a late error from a second submit is stale. */}
      {error && open && <p role="alert">{error}</p>}
      <button type="button" onClick={() => dialog.current?.close()}>
        {open ? "Cancel" : "Close"}
      </button>
    </dialog>
  );
}
