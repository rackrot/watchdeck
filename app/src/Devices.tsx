import { useEffect, useState } from "react";
import { forgetDevice, pairingState } from "./api";
import { useDevices } from "./hooks";
import PairDialog from "./PairDialog";

export default function Devices({
  active,
  onResume,
}: {
  /** Whether this page is the one shown. A modal dialog can only open on a shown page. */
  active: boolean;
  /** Asks for this page to be shown, for a pairing that was left open. */
  onResume: () => void;
}) {
  const [devices, listError] = useDevices();
  // "start" opens the pairing port; "resume" shows one an earlier page load left open.
  const [pairing, setPairing] = useState<"start" | "resume" | null>(null);
  const [actionError, setError] = useState("");
  const error = actionError || listError;

  useEffect(() => {
    pairingState().then(
      (s) => {
        if (s.state === "waiting" || s.state === "confirm") {
          setPairing((p) => p ?? "resume");
        }
      },
      (e) => setError(String(e)),
    );
  }, []);

  useEffect(() => {
    if (pairing === "resume") {
      onResume();
    }
  }, [pairing, onResume]);

  async function forget(device: string) {
    setError("");
    try {
      await forgetDevice(device);
    } catch (e) {
      setError(String(e));
    }
  }

  return (
    <div className="page devices">
      <section>
        <h3>Devices</h3>
        {devices.length === 0 ? (
          <p className="muted">No paired devices.</p>
        ) : (
          <ul className="name-list">
            {devices.map((d) => (
              <li key={d.id}>
                <span
                  className={d.connected ? "dot ok" : "dot offline"}
                  aria-hidden="true"
                />
                <span className="mono grow">{d.id}</span>
                <span className="muted">
                  {d.connected ? "connected" : "offline"}
                </span>
                <button
                  type="button"
                  className="quiet"
                  aria-label={`Forget ${d.id}`}
                  onClick={() => forget(d.id)}
                >
                  Forget
                </button>
              </li>
            ))}
          </ul>
        )}
        {devices
          .filter((d) => d.connected && !d.caps.includes("fonts"))
          .map((d) => (
            <p key={d.id} className="muted">
              <span className="mono">{d.id}</span> draws every font as
              Montserrat. Update its firmware.
            </p>
          ))}
        {devices
          .filter((d) => d.connected && !d.caps.includes("pages"))
          .map((d) => (
            <p key={d.id} className="muted">
              <span className="mono">{d.id}</span> shows page 1 only. Update its
              firmware.
            </p>
          ))}
        <button
          type="button"
          onClick={() => {
            setError("");
            setPairing("start");
          }}
        >
          Pair a device
        </button>
        {error && (
          <p className="problems" role="alert">
            {error}
          </p>
        )}
      </section>
      {pairing && active && (
        <PairDialog
          resume={pairing === "resume"}
          onClose={() => setPairing(null)}
        />
      )}
    </div>
  );
}
