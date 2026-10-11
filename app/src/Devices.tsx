import { listen } from "@tauri-apps/api/event";
import { useEffect, useState } from "react";
import {
  type DeviceInfo,
  forgetDevice,
  listDevices,
  pairingState,
} from "./api";
import PairDialog from "./PairDialog";

export default function Devices() {
  const [devices, setDevices] = useState<DeviceInfo[]>([]);
  // "start" opens the pairing port; "resume" shows one an earlier page load left open.
  const [pairing, setPairing] = useState<"start" | "resume" | null>(null);
  const [error, setError] = useState("");

  useEffect(() => {
    const refresh = () =>
      listDevices().then(setDevices, (e) => setError(String(e)));
    // Fetch once the listener is in place, so no change falls between the two.
    const unlisten = listen("devices-changed", refresh);
    unlisten.then(refresh);
    pairingState().then(
      (s) => {
        if (s.state === "waiting" || s.state === "confirm") {
          setPairing((p) => p ?? "resume");
        }
      },
      (e) => setError(String(e)),
    );
    return () => {
      unlisten.then((stop) => stop());
    };
  }, []);

  async function forget(device: string) {
    setError("");
    try {
      await forgetDevice(device);
    } catch (e) {
      setError(String(e));
    }
  }

  return (
    <section>
      <h2>Devices</h2>
      {devices.length === 0 ? (
        <p>No paired devices.</p>
      ) : (
        <ul className="devices">
          {devices.map((d) => (
            <li key={d.id}>
              <span>{d.id}</span>
              <span>{d.connected ? "connected" : "offline"}</span>
              <button type="button" onClick={() => forget(d.id)}>
                Forget
              </button>
            </li>
          ))}
        </ul>
      )}
      <button
        type="button"
        onClick={() => {
          setError("");
          setPairing("start");
        }}
      >
        Pair a device
      </button>
      {error && <p role="alert">{error}</p>}
      {pairing && (
        <PairDialog
          resume={pairing === "resume"}
          onClose={() => setPairing(null)}
        />
      )}
    </section>
  );
}
