import { listen } from "@tauri-apps/api/event";
import { useCallback, useEffect, useRef, useState } from "react";
import {
  getConfig,
  importIcon,
  listDevices,
  listIcons,
  saveConfig,
} from "./api";
import ButtonForm from "./ButtonForm";
import {
  type Config,
  type ConfigStatus,
  type ConfigView,
  newButton,
} from "./config";
import { layoutChanged } from "./edit";
import {
  type ErrorMap,
  emptyErrors,
  hasErrors,
  mapErrors,
  numberErrors,
} from "./errors";
import GridEditor from "./GridEditor";
import { useIconUrls, useTileReadings } from "./hooks";
import SaveBar from "./SaveBar";
import {
  diskChanged as diskMoved,
  isDirty,
  ownStatus,
  pushOutcome,
  replaceNotice,
  shouldFollowDisk,
  testBlock,
} from "./saveflow";

/** A save reaches a connected device within this long, or the page says it didn't. */
const PUSH_WITHIN_MS = 2000;

type Saved = {
  t0: number;
  /** Connected devices that should get a new layout. */
  targets: string[];
  connected: number;
};

export default function ConfigPage({ secrets }: { secrets: string[] }) {
  const [view, setView] = useState<ConfigView | null>(null);
  const [draft, setDraft] = useState<Config | null>(null);
  const [selected, setSelected] = useState<number | null>(null);
  const [errors, setErrors] = useState<ErrorMap>(emptyErrors);
  const [loadError, setLoadError] = useState("");
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState<Saved | null>(null);
  const [pushes, setPushes] = useState<Record<string, number>>({});
  const [late, setLate] = useState(false);
  const [onDisk, setOnDisk] = useState<ConfigStatus | null>(null);
  const [iconFiles, setIconFiles] = useState<string[]>([]);
  const [iconError, setIconError] = useState("");
  // Messages of the last refused save, whole, so none can go unseen.
  const [refused, setRefused] = useState<string[]>([]);
  // Problems the forms see before any call, such as a header listed twice.
  const [local, setLocal] = useState<string[]>([]);
  const [confirming, setConfirming] = useState(false);
  // Bumped when the draft is replaced from outside, so the forms start over from it.
  const [formKey, setFormKey] = useState(0);
  const save$ = useRef<{ inFlight: boolean; revision: string | null }>({
    inFlight: false,
    revision: null,
  });
  const readings = useTileReadings();
  const iconUrl = useIconUrls();

  const refreshIcons = useCallback(async () => {
    try {
      setIconFiles(await listIcons());
    } catch (e) {
      setIconError(String(e));
    }
  }, []);

  const load = useCallback(async () => {
    void refreshIcons();
    try {
      const next = await getConfig();
      setView(next);
      setDraft(structuredClone(next.config));
      setSelected(null);
      setErrors(emptyErrors());
      setOnDisk(null);
      setSaved(null);
      setRefused([]);
      setLocal([]);
      setConfirming(false);
      setFormKey((k) => k + 1);
      setLoadError("");
    } catch (e) {
      setLoadError(String(e));
    }
  }, [refreshIcons]);
  // Listen first, then read, so no change falls between the two.
  useEffect(() => {
    const listeners = Promise.all([
      listen<ConfigStatus>("config-status", (e) => {
        if (!ownStatus(e.payload, save$.current)) {
          setOnDisk(e.payload);
        }
      }),
      listen<string>("layout-pushed", (e) =>
        setPushes((p) => ({ ...p, [e.payload]: Date.now() })),
      ),
    ]);
    listeners.then(
      () => load(),
      (e) => setLoadError(String(e)),
    );
    return () => {
      listeners.then((stops) => {
        for (const stop of stops) {
          stop();
        }
      });
    };
  }, [load]);

  const dirty = view !== null && draft !== null && isDirty(draft, view);
  const diskChanged = view !== null && diskMoved(view, onDisk);
  // With no edits to lose, follow the file.
  useEffect(() => {
    if (shouldFollowDisk(diskChanged, dirty, saving)) {
      void load();
    }
  }, [diskChanged, dirty, saving, load]);

  useEffect(() => {
    setLate(false);
    if (!saved || saved.targets.length === 0) {
      return;
    }
    const timer = setTimeout(() => setLate(true), PUSH_WITHIN_MS);
    return () => clearTimeout(timer);
  }, [saved]);

  if (!view || !draft) {
    return (
      <div className="page">
        {loadError ? (
          <p className="problems" role="alert">
            {loadError}
          </p>
        ) : (
          <p className="muted">Loading…</p>
        )}
      </div>
    );
  }

  const set = (next: Config) => setDraft(next);
  const repair = view.errors.length > 0;

  async function save(confirmed = false) {
    if (!draft || !view) {
      return;
    }
    if (local.length > 0) {
      setRefused(local);
      return;
    }
    const numbers = numberErrors(draft);
    if (hasErrors(numbers)) {
      setErrors(numbers);
      setRefused([
        ...numbers.grid,
        ...[...numbers.buttons].flatMap(([id, f]) =>
          Object.values(f)
            .flat()
            .map((m) => `${id}: ${m}`),
        ),
      ]);
      return;
    }
    if (repair && !confirmed) {
      setConfirming(true);
      return;
    }
    setConfirming(false);
    setSaving(true);
    save$.current = { inFlight: true, revision: null };
    setSaved(null);
    try {
      const connected = (await listDevices())
        .filter((d) => d.connected)
        .map((d) => d.id);
      // Before the call: the push can be announced before the call returns.
      const t0 = Date.now();
      const revision = await saveConfig(draft, view.revision);
      save$.current = { inFlight: false, revision };
      setView({ ...view, config: draft, revision, errors: [], loaded: true });
      setErrors(emptyErrors());
      setRefused([]);
      setOnDisk(null);
      setSaved({
        t0,
        connected: connected.length,
        targets: layoutChanged(view.config, draft) ? connected : [],
      });
    } catch (e) {
      const messages = Array.isArray(e) ? (e as string[]) : [String(e)];
      setErrors(
        mapErrors(
          messages,
          draft.button.map((b) => b.id),
        ),
      );
      setRefused(messages);
    } finally {
      save$.current.inFlight = false;
      setSaving(false);
    }
  }

  async function pickIcon() {
    setIconError("");
    try {
      const name = await importIcon();
      if (name !== null && selected !== null && draft) {
        await refreshIcons();
        set({
          ...draft,
          button: draft.button.map((b, i) =>
            i === selected ? { ...b, icon: name } : b,
          ),
        });
      }
    } catch (e) {
      setIconError(String(e));
    }
  }

  const button = selected === null ? undefined : draft.button[selected];
  // The picker offers the files in the icons directory, and the button's own icon even if its
  // file is gone, so the select doesn't show "(none)" for a button that names one.
  const icons =
    button?.icon && !iconFiles.includes(button.icon)
      ? [...iconFiles, button.icon].sort()
      : iconFiles;

  return (
    <div className="page layout">
      <SaveBar
        dirty={dirty}
        repair={repair}
        saving={saving}
        onSave={() => void save()}
        onRevert={() => {
          setDraft(structuredClone(view.config));
          setSelected(null);
          setErrors(emptyErrors());
          setRefused([]);
          setLocal([]);
          setConfirming(false);
          setFormKey((k) => k + 1);
        }}
        status={<SavedStatus saved={saved} pushes={pushes} late={late} />}
      />

      {repair && (
        <div className="banner bad" role="alert">
          {view.loaded ? (
            <span>
              <strong>config.toml is not live.</strong> The app is running the
              last good config, which is the grid below. Fix the file by hand
              and this page follows, or replace the file with what you see.
            </span>
          ) : (
            <span>
              <strong>config.toml could not be loaded</strong> since the app
              started, so the grid below is empty. It is not your file. Fix the
              file by hand and this page follows. If you replace it, the file is
              overwritten with this empty grid and its text is kept as a backup.
            </span>
          )}
          <ul>
            {view.errors.map((message) => (
              <li className="mono" key={message}>
                {message}
              </li>
            ))}
          </ul>
        </div>
      )}
      {confirming && (
        <div
          className="banner stale"
          role="alertdialog"
          aria-label="Confirm replacing config.toml"
        >
          <span>{replaceNotice(view, draft)}</span>
          <button
            type="button"
            className="danger"
            onClick={() => void save(true)}
          >
            Replace config.toml
          </button>
          <button type="button" onClick={() => setConfirming(false)}>
            Cancel
          </button>
        </div>
      )}
      {diskChanged && dirty && (
        <div className="banner stale" role="alert">
          <strong>config.toml changed on disk</strong> while you were editing. A
          save would be refused.
          <button type="button" onClick={() => void load()}>
            Reload and drop my edits
          </button>
        </div>
      )}
      {refused.length > 0 && (
        <div className="banner bad" role="alert">
          <strong>Save refused.</strong> Nothing was written.
          <ul>
            {refused.map((message) => (
              <li key={message}>{message}</li>
            ))}
          </ul>
          {errors.general.length > 0 && (
            <button type="button" onClick={() => void load()}>
              Reload from disk
            </button>
          )}
        </div>
      )}

      <GridEditor
        config={draft}
        selected={selected}
        errors={errors.buttons}
        gridProblems={errors.grid}
        readings={readings}
        iconUrl={iconUrl}
        onSelect={setSelected}
        onChange={set}
        onAdd={(pos) => {
          set({ ...draft, button: [...draft.button, newButton(draft, pos)] });
          setSelected(draft.button.length);
        }}
      />

      {button ? (
        <ButtonForm
          // A fresh form per button, and after the draft is replaced, so its rows start from it.
          key={`${formKey}:${selected}`}
          button={button}
          errors={errors.buttons.get(button.id) ?? {}}
          secrets={secrets}
          icons={icons}
          iconUrl={iconUrl}
          iconError={iconError}
          testBlock={testBlock(button, view.config, diskChanged)}
          onImportIcon={pickIcon}
          onLocalProblems={setLocal}
          onChange={(next) =>
            set({
              ...draft,
              button: draft.button.map((b, i) => (i === selected ? next : b)),
            })
          }
          onDelete={() => {
            set({
              ...draft,
              button: draft.button.filter((_, i) => i !== selected),
            });
            setSelected(null);
          }}
        />
      ) : (
        <p className="muted hint">Select a button to edit it.</p>
      )}
    </div>
  );
}

/** What the last save did, and whether the devices it should reach were sent the layout. */
function SavedStatus({
  saved,
  pushes,
  late,
}: {
  saved: Saved | null;
  pushes: Record<string, number>;
  late: boolean;
}) {
  if (!saved) {
    return <span className="chip offline">No changes</span>;
  }
  const lines = saved.targets.map((device) => {
    const outcome = pushOutcome(pushes[device], saved.t0, late);
    if (outcome.kind === "sent") {
      return (
        <span className="chip ok" key={device}>
          <span className="mono">{device}</span> sent the layout in {outcome.ms}{" "}
          ms
        </span>
      );
    }
    return outcome.kind === "late" ? (
      <span className="chip bad" key={device}>
        <span className="mono">{device}</span> was not sent a layout within 2 s
      </span>
    ) : (
      <span className="chip busy" key={device}>
        sending to <span className="mono">{device}</span>…
      </span>
    );
  });
  const note =
    saved.targets.length > 0
      ? null
      : saved.connected === 0
        ? "no device connected, so it gets the layout when it connects"
        : "the layout is unchanged, so devices keep it";
  return (
    <>
      <span className="chip ok">Saved</span>
      {lines}
      {note && <span className="muted">{note}</span>}
    </>
  );
}
