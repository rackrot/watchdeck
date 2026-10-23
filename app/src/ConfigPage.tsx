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
import {
  barChanged,
  buttonAt,
  layoutTargets,
  pageCount,
  pageIndexOf,
  pageName,
} from "./edit";
import {
  type ErrorMap,
  emptyErrors,
  hasErrors,
  mapErrors,
  numberErrors,
} from "./errors";
import GridEditor from "./GridEditor";
import { useIconUrls, useTileReadings } from "./hooks";
import PageTabs from "./PageTabs";
import SaveBar from "./SaveBar";
import StatusBarForm from "./StatusBarForm";
import {
  diskChanged as diskMoved,
  isDirty,
  type LayoutStamp,
  ownStatus,
  pushOutcome,
  replaceNotice,
  shouldFollowDisk,
  testBlock,
} from "./saveflow";

/** A save reaches a connected device within this long, or the page says it didn't. */
const PUSH_WITHIN_MS = 2000;

/** Payload of `layout-pushed` and `layout-applied`. */
type LayoutEvent = { device: string; rev: number };

type Saved = {
  t0: number;
  /** Connected devices that should get a new layout. */
  targets: string[];
  connected: number;
  /** The save changed `[status_bar]`, which connected devices pick up without a layout. */
  bar: boolean;
};

/** A `[status_bar]` edit reaches a connected device within this long: the config poll and the next tick. */
const BAR_WITHIN_S = 2;

export default function ConfigPage({ secrets }: { secrets: string[] }) {
  const [view, setView] = useState<ConfigView | null>(null);
  const [draft, setDraft] = useState<Config | null>(null);
  const [selected, setSelected] = useState<number | null>(null);
  // The status bar form takes the button form's place, so a button and the form are never both open.
  const [statusBarOpen, setStatusBarOpen] = useState(false);
  // The page the grid shows, from 0. Clamped below, since the draft can lose pages.
  const [pageIdx, setPageIdx] = useState(0);
  const [errors, setErrors] = useState<ErrorMap>(emptyErrors);
  const [loadError, setLoadError] = useState("");
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState<Saved | null>(null);
  const [pushes, setPushes] = useState<Record<string, LayoutStamp>>({});
  const [applied, setApplied] = useState<Record<string, LayoutStamp>>({});
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
      setPageIdx(0);
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
      listen<LayoutEvent>("layout-pushed", (e) =>
        setPushes((p) => ({
          ...p,
          [e.payload.device]: { at: Date.now(), rev: e.payload.rev },
        })),
      ),
      listen<LayoutEvent>("layout-applied", (e) =>
        setApplied((p) => ({
          ...p,
          [e.payload.device]: { at: Date.now(), rev: e.payload.rev },
        })),
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
  const pages = pageCount(draft);
  const page = Math.min(pageIdx, pages - 1);

  /** Applies a change to the pages, and shows page `to`. A selection that isn't on it is dropped. */
  function changePages(next: Config, to: number) {
    const keep =
      selected !== null &&
      next.button.length === draft?.button.length &&
      pageIndexOf(next, next.button[selected]) === to;
    set(next);
    setPageIdx(to);
    if (!keep) {
      setSelected(null);
    }
  }

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
        ...Object.entries(numbers.statusBar).flatMap(([field, list]) =>
          list.map((m) => `status_bar.${field}: ${m}`),
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
      const devices = await listDevices();
      const connected = devices.filter((d) => d.connected);
      const targets = layoutTargets(view.config, draft, devices);
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
        targets,
        bar: barChanged(view.config, draft),
      });
    } catch (e) {
      const messages = Array.isArray(e) ? (e as string[]) : [String(e)];
      setErrors(
        mapErrors(
          messages,
          draft.button.map((b) => b.id),
          (draft.page ?? []).map((p) => p.id),
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
          setPageIdx(0);
          setErrors(emptyErrors());
          setRefused([]);
          setLocal([]);
          setConfirming(false);
          setFormKey((k) => k + 1);
        }}
        status={
          <SavedStatus
            saved={saved}
            pushes={pushes}
            applied={applied}
            late={late}
          />
        }
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

      <PageTabs
        config={draft}
        page={page}
        errors={errors.pages}
        onChange={changePages}
        onSelect={(to) => changePages(draft, to)}
      />

      <GridEditor
        config={draft}
        page={page}
        selected={selected}
        errors={errors.buttons}
        gridProblems={errors.grid}
        readings={readings}
        iconUrl={iconUrl}
        onSelect={(index) => {
          setStatusBarOpen(false);
          setSelected(index);
        }}
        onChange={set}
        onAdd={(pos) => {
          const home = draft.page?.[page]?.id;
          set({
            ...draft,
            button: [...draft.button, newButton(draft, pos, home)],
          });
          setStatusBarOpen(false);
          setSelected(draft.button.length);
        }}
        statusBarOpen={statusBarOpen}
        statusBarProblems={Object.keys(errors.statusBar).length > 0}
        onStatusBar={() => {
          setStatusBarOpen(!statusBarOpen);
          setSelected(null);
        }}
      />

      {statusBarOpen ? (
        <StatusBarForm
          key={formKey}
          bar={draft.status_bar}
          defaults={view.status_bar_defaults}
          errors={errors.statusBar}
          onChange={(status_bar) => {
            const { status_bar: _, ...rest } = draft;
            set(status_bar === undefined ? rest : { ...rest, status_bar });
          }}
        />
      ) : button ? (
        <ButtonForm
          // A fresh form per button, and after the draft is replaced, so its rows start from it.
          key={`${formKey}:${selected}`}
          button={button}
          gridFont={draft.layout.font}
          pages={(draft.page ?? []).map((p) => ({
            id: p.id,
            name: pageName(p),
          }))}
          pageId={draft.page?.[pageIndexOf(draft, button)]?.id ?? ""}
          occupant={(id) =>
            buttonAt(
              draft,
              draft.page?.findIndex((p) => p.id === id) ?? 0,
              button.pos,
              selected ?? undefined,
            )?.id
          }
          errors={errors.buttons.get(button.id) ?? {}}
          secrets={secrets}
          icons={icons}
          iconUrl={iconUrl}
          iconError={iconError}
          testBlock={testBlock(button, view.config, diskChanged)}
          onImportIcon={pickIcon}
          onLocalProblems={setLocal}
          onChange={(next) => {
            set({
              ...draft,
              button: draft.button.map((b, i) => (i === selected ? next : b)),
            });
            // A button moved to another page takes the grid with it.
            if (next.page !== button.page) {
              const to = draft.page?.findIndex((p) => p.id === next.page) ?? -1;
              if (to >= 0) {
                setPageIdx(to);
              }
            }
          }}
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

/** What the last save did, and whether the devices it should reach were sent the layout and applied it. */
function SavedStatus({
  saved,
  pushes,
  applied,
  late,
}: {
  saved: Saved | null;
  pushes: Record<string, LayoutStamp>;
  applied: Record<string, LayoutStamp>;
  late: boolean;
}) {
  if (!saved) {
    return <span className="chip offline">No changes</span>;
  }
  const lines = saved.targets.map((device) => {
    const outcome = pushOutcome(
      pushes[device],
      applied[device],
      saved.t0,
      late,
    );
    if (outcome.kind === "applied") {
      return (
        <span className="chip ok" key={device}>
          layout applied on <span className="mono">{device}</span>
        </span>
      );
    }
    if (outcome.kind === "sent") {
      return (
        <span className="chip ok" key={device}>
          layout sent to <span className="mono">{device}</span> in {outcome.ms}{" "}
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
  // A bar edit pushes no layout, so "the layout is unchanged" would only confuse beside its line.
  const note =
    saved.targets.length > 0 || (saved.bar && saved.connected > 0)
      ? null
      : saved.connected === 0
        ? "no device connected, so it gets the layout when it connects"
        : "the layout is unchanged, so devices keep it";
  return (
    <>
      <span className="chip ok">Saved</span>
      {saved.bar && saved.connected > 0 && (
        <span className="chip ok">
          status bar settings apply within {BAR_WITHIN_S} s
        </span>
      )}
      {lines}
      {note && <span className="muted">{note}</span>}
    </>
  );
}
