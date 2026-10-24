import React from "react";
import ReactDOM from "react-dom/client";
import App from "./App";
import "./fonts.css";

// The splash shows at least this long, so a fast config load doesn't flash it.
const SPLASH_MIN_MS = 300;
const splash = document.getElementById("splash");
const shownAt = performance.now();
let hidden = false;

/** Removes the splash once the first config load has answered. Later calls do nothing. */
function hideSplash(): void {
  if (hidden || !splash) {
    return;
  }
  hidden = true;
  const wait = SPLASH_MIN_MS - (performance.now() - shownAt);
  window.setTimeout(() => splash.remove(), Math.max(0, wait));
}

ReactDOM.createRoot(document.getElementById("root") as HTMLElement).render(
  <React.StrictMode>
    <App onConfigLoaded={hideSplash} />
  </React.StrictMode>,
);
