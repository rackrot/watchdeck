import { type KeyboardEvent, useState } from "react";
import logo from "./assets/logo.svg";
import ConfigPage from "./ConfigPage";
import Devices from "./Devices";
import { useDevices, useSecrets } from "./hooks";
import Secrets from "./Secrets";
import "./theme.css";
import "./App.css";

const TABS = [
  ["layout", "Layout"],
  ["secrets", "Secrets"],
  ["devices", "Devices"],
] as const;

type Tab = (typeof TABS)[number][0];

function App({ onConfigLoaded }: { onConfigLoaded: () => void }) {
  const [tab, setTab] = useState<Tab>("layout");
  const [devices] = useDevices();
  const [secrets, refreshSecrets] = useSecrets();
  const connected = devices.filter((d) => d.connected).length;

  // Arrow keys move between tabs, as in the tab pattern; only the shown tab is a tab stop.
  function arrows(e: KeyboardEvent) {
    const step = e.key === "ArrowRight" ? 1 : e.key === "ArrowLeft" ? -1 : 0;
    if (step === 0) {
      return;
    }
    e.preventDefault();
    const next =
      TABS[
        (TABS.findIndex(([id]) => id === tab) + step + TABS.length) %
          TABS.length
      ][0];
    setTab(next);
    document.getElementById(`tab-${next}`)?.focus();
  }

  // Every page stays mounted, so an edit in progress survives a look at another tab.
  return (
    <div className="app">
      <header className="topbar">
        <h1 className="wordmark">
          <img src={logo} alt="" className="brand-mark" />
          watchdeck
        </h1>
        <div
          className="tabs"
          role="tablist"
          aria-label="Pages"
          onKeyDown={arrows}
        >
          {TABS.map(([id, label]) => (
            <button
              type="button"
              role="tab"
              key={id}
              id={`tab-${id}`}
              aria-controls={`panel-${id}`}
              aria-selected={tab === id}
              tabIndex={tab === id ? 0 : -1}
              onClick={() => setTab(id)}
            >
              {label}
            </button>
          ))}
        </div>
        <span className="device-count">
          <span
            className={connected > 0 ? "dot ok" : "dot offline"}
            aria-hidden="true"
          />
          {connected > 0
            ? `${connected} device${connected === 1 ? "" : "s"} connected`
            : "no device connected"}
        </span>
      </header>
      <main>
        <div
          role="tabpanel"
          id="panel-layout"
          aria-labelledby="tab-layout"
          hidden={tab !== "layout"}
        >
          <ConfigPage
            secrets={secrets}
            onSecretsChanged={refreshSecrets}
            onConfigLoaded={onConfigLoaded}
          />
        </div>
        <div
          role="tabpanel"
          id="panel-secrets"
          aria-labelledby="tab-secrets"
          hidden={tab !== "secrets"}
        >
          <Secrets names={secrets} onChanged={refreshSecrets} />
        </div>
        <div
          role="tabpanel"
          id="panel-devices"
          aria-labelledby="tab-devices"
          hidden={tab !== "devices"}
        >
          <Devices
            active={tab === "devices"}
            onResume={() => setTab("devices")}
          />
        </div>
      </main>
    </div>
  );
}

export default App;
