import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import Panel from '../components/Panel.jsx'
import Button from '../components/Button.jsx'
import DropZone from '../components/DropZone.jsx'
import { useBackLayer } from '../lib/backLayer.js'
import PathPickerModal from '../components/PathPickerModal.jsx'
import ActionMenu from '../components/ActionMenu.jsx'
import SearchField from '../components/SearchField.jsx'
import EmptyState from '../components/EmptyState.jsx'
import { ConfirmDialog, InputDialog } from '../components/Dialog.jsx'
import DownloadPayloadDialog from '../components/DownloadPayloadDialog.jsx'
import TransferConflictDialog from '../components/TransferConflictDialog.jsx'
import { UploadIcon, DeleteIcon, LaunchIcon, RefreshIcon, PowerIcon, KillIcon, RenameIcon, StarIcon, FolderIcon, ClockIcon } from '../components/icons.jsx'
import { formatSize as formatTransferSize, formatSpeed, formatEta } from '../components/TransferProgress.jsx'
import JobCard from '../components/JobCard.jsx'
import { api } from '../lib/api.js'
import { useToast } from '../lib/ToastContext.jsx'
import { isNewerVersion } from '../lib/versions.js'
import { payloadOrigin as sourceOf } from '../lib/payloadOrigin.js'
import { useI18n } from '../lib/i18n.jsx'
import { usePolling } from '../lib/usePolling.js'
import { formatFileSize } from '../lib/format.js'
import { isPS5 } from '../lib/effects.js'
import { isPayloadLikeProcessName } from '../lib/processes.js'

// Process names on the PS5 can come back truncated/without extension —
// match loosely both ways instead of requiring an exact filename match.
function SectionHeading({ label, count }) {
  return (
    <div className="mb-2 flex items-center gap-2 text-xs font-bold uppercase tracking-wide text-text-dim">
      {label}
      <span className="font-mono text-[11px] font-semibold opacity-70" dir="ltr">{count}</span>
      <span className="h-px flex-1 bg-panel-border" />
    </div>
  )
}

// A poll that finds the same processes keeps the old array, so the grid is not redrawn for nothing.
const sameProcesses = (a, b) => a.length === b.length && a.every((p, i) => p.pid === b[i].pid && p.name === b[i].name)

function uniquePayloadName(name, taken) {
  const dot = name.lastIndexOf('.')
  const base = dot > 0 ? name.slice(0, dot) : name
  const ext = dot > 0 ? name.slice(dot) : ''
  for (let n = 1; ; n++) {
    const candidate = `${base} (${n})${ext}`
    if (!taken.has(candidate)) return candidate
  }
}

function stem(name) {
  return name.replace(/\.(elf|bin|self)$/i, '').toLowerCase()
}

function findRunningPid(payloadName, processes) {
  const s = stem(payloadName)
  if (!s) return null
  // Exact match, or a truncated prefix from the capped comm field. Not a loose
  // substring, which would false-match on a short fragment.
  const hit = processes.find((p) => {
    const ps = stem(p.name)
    return ps.length >= 3 && (ps === s || s.startsWith(ps))
  })
  return hit ? hit.pid : null
}

// Above this many payloads a search box earns its place.
const SEARCH_FROM = 12

// One grid for launching and pinning; only what a tap does changes.
const GRID = isPS5() ? 'grid-cols-3' : 'grid-cols-1 sm:grid-cols-2 xl:grid-cols-3'

// A launcher card: initial, name, version, size and origin, with a Stop button while it runs. Memoized, since the 5s
// process poll below would otherwise re-render every tile.
const PayloadButton = memo(function PayloadButton({ p, index, pinMode, busy, runningPid, inAutoloader, onTap, onStop, onDelete, onToggleEnabled, onToggleAutoloader, onRename, t, locale }) {
  const launching = busy === p.name
  const external = p.external
  const disabled = !external && !p.enabled
  const pinning = pinMode && !external
  const name = p.name.replace(/\.(elf|bin|self)$/i, '')
  const pinned = pinning && p.favorite
  // The whole card launches the payload; its buttons and menu keep their own actions.
  const onCardClick = (event) => {
    if (launching || !event.currentTarget.contains(event.target) || event.target.closest('button, a')) return
    onTap(p, runningPid)
  }

  return (
    <div
      data-menu-host
      onClick={onCardClick}
      className={`pmgr-tile pmgr-tile-in group relative flex cursor-pointer flex-col rounded-2xl border bg-panel ${pinned ? 'border-warn/60 bg-warn/5' : 'border-panel-border'}`}
      style={{ '--ti': index }}
    >
      <div className="flex items-stretch">
        <button
          type="button"
          onClick={() => onTap(p, runningPid)}
          disabled={launching}
          aria-label={p.name}
          className="flex min-w-0 flex-1 items-center rounded-2xl p-4 pb-2 text-left"
        >
          <span
            aria-hidden="true"
            className={`mr-4 grid h-14 w-14 shrink-0 place-items-center rounded-xl bg-panel-2 text-2xl font-bold uppercase ${disabled ? 'text-text-dim' : 'text-accent'}`}
          >
            {name.charAt(0)}
          </span>
          <span className="min-w-0 flex-1">
            <span className={`line-clamp-2 block break-words text-lg font-bold leading-tight ${disabled ? 'text-text-dim' : 'text-text'}`} title={p.name}>{name}</span>
            <span className="mt-1 block truncate text-sm text-text-dim">
              {external ? <span dir="ltr">{`pid ${runningPid}`}</span> : (
                <>
                  {p.installed_version && <span className="pmgr-mono mr-3 text-text" dir="ltr">{p.installed_version}</span>}
                  <span className="pmgr-num">{formatFileSize(p.size_bytes, locale)}</span>
                </>
              )}
            </span>
            <span className="mt-0.5 flex h-5 min-w-0 items-center text-sm text-text-dim">
              <span className="truncate">{!external && t(`payloads.origin.${sourceOf(p)}`)}</span>
              {disabled && <span className="ml-2 shrink-0 rounded bg-panel-2 px-1.5 text-xs font-semibold">{t('payloads.disabledPill')}</span>}
            </span>
          </span>
          {pinning && <span className={`ml-2 shrink-0 ${p.favorite ? 'text-warn' : 'text-text-dim'}`}><StarIcon size={22} filled={p.favorite} /></span>}
          {launching && <span className="pmgr-live-dot ml-2 shrink-0" />}
        </button>
        {!pinMode && (inAutoloader || !external) && (
          <div className="flex shrink-0 items-start pl-1 pr-3 pt-3">
            {inAutoloader && <span className="mr-1 mt-2 text-text-dim" title={t('payloads.inAutoloader')}><ClockIcon size={15} /></span>}
            {!external && (
              <ActionMenu
                label={t('payloads.moreActionsFor', { name: p.name })}
                align="right"
                items={[
                  !p.bundled && { label: t('payloads.rename'), icon: <RenameIcon size={15} />, onClick: () => onRename(p.name) },
                  { label: t(inAutoloader ? 'payloads.removeFromAutoloader' : 'payloads.addToAutoloader'), icon: <ClockIcon size={15} />, onClick: () => onToggleAutoloader(p.name, inAutoloader) },
                  { label: t(p.enabled ? 'payloads.disable' : 'payloads.enable'), icon: <PowerIcon size={15} />, onClick: () => onToggleEnabled(p) },
                  '-',
                  !p.bundled && { label: t('payloads.delete'), icon: <DeleteIcon size={15} />, tone: 'danger', onClick: () => onDelete(p.name) },
                ]}
              />
            )}
          </div>
        )}
      </div>
      {!pinMode && (
        <div className="flex min-h-[2.75rem] items-center justify-end px-4 pb-3">
          {runningPid ? (
            <>
              <span className="mr-3 flex items-center text-sm font-semibold text-ok">
                <span className="mr-1.5 h-2 w-2 rounded-full bg-ok" />
                {t('payloads.running')}
              </span>
              <Button size="sm" tone="danger" icon={<KillIcon size={14} />} onClick={() => onStop(runningPid, p.name)}>
                {t('payloads.stop')}
              </Button>
            </>
          ) : (
            <button
              type="button"
              tabIndex={-1}
              aria-hidden="true"
              disabled={launching}
              onClick={() => onTap(p, runningPid)}
              className="grid h-9 w-9 place-items-center rounded-full text-text-dim transition-colors duration-150 group-hover:text-accent"
            >
              <svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor"><path d="M8 5v14l11-7Z" /></svg>
            </button>
          )}
        </div>
      )}
    </div>
  )
})

export default function PayloadsView({ onNavigate }) {
  const { t, locale } = useI18n()
  const [payloads, setPayloads] = useState([])
  const [catalog, setCatalog] = useState([])
  const [updateEntry, setUpdateEntry] = useState(null) // catalog entry currently open in the download dialog
  const [processes, setProcesses] = useState([])
  const [loaded, setLoaded] = useState(false)
  const [error, setError] = useState('')
  const [busy, setBusy] = useState('')
  const [uploadJob, setUploadJob] = useState(null)
  const [autoloaderNames, setAutoloaderNames] = useState(() => new Set())
  const [uploadConflict, setUploadConflict] = useState(null)
  const [volumes, setVolumes] = useState([])
  const [consolePickerOpen, setConsolePickerOpen] = useState(false)
  const [pinMode, setPinMode] = useState(false) // taps pin favorites instead of launching
  const [showUpdates, setShowUpdates] = useState(false)
  const [search, setSearch] = useState('')
  const [dialog, setDialog] = useState(null) // { kind: 'delete'|'rename'|'kill', ... }
  const fileInputRef = useRef(null)
  const showToast = useToast()
  useBackLayer(pinMode, () => setPinMode(false))

  const loadAutoloader = useCallback(() => {
    api.autoloader.getConfig()
      .then((c) => setAutoloaderNames(new Set(c.sequence.filter((step) => step.type === 'load').map((step) => step.value))))
      .catch(() => {})
  }, [])

  const load = useCallback(() => {
    loadAutoloader()
    api.payloads
      .list()
      .then((r) => setPayloads(r.payloads))
      .catch((e) => setError(e.message))
      .finally(() => setLoaded(true))
  }, [loadAutoloader])

  useEffect(() => {
    load()
    api.ftp.storage().then((r) => setVolumes(r.volumes || [])).catch(() => {})
    // Cached only — never forces a refresh here, that's the Payload Repositories tab's
    // job (GitHub's unauthenticated rate limit is shared and easy to burn through).
    api.catalog.list(false).then((r) => setCatalog(r.entries || [])).catch(() => {})
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  // A payload whose recorded source repo has a cached "latest" version that doesn't
  // match what's installed. Empty until the user has checked at least once in Repositories.
  const updates = useMemo(() => {
    if (!catalog.length) return []
    const byUrl = new Map(catalog.map((entry) => [entry.url, entry]))
    const out = []
    for (const p of payloads) {
      if (!p.source_url) continue
      const entry = byUrl.get(p.source_url)
      if (!entry || entry.version_state !== 'ok' || !entry.version) continue
      if (p.installed_version && !isNewerVersion(entry.version, p.installed_version)) continue
      out.push({ payload: p, entry })
    }
    return out
  }, [payloads, catalog])
  // Cross-referenced against Processes so an already-running bundled payload can
  // be killed here rather than from the Processes tab.
  usePolling(() => api.processes.list().then((r) => setProcesses((prev) => (sameProcesses(prev, r.processes) ? prev : r.processes))).catch(() => {}), 5000)

  const killPayload = useCallback((pid, name) => {
    api.processes
      .kill(pid)
      .then(() => {
        showToast(t('payloads.stoppedToast', { name, pid }))
        api.processes.list().then((r) => setProcesses(r.processes)).catch(() => {})
      })
      .catch((e) => showToast(e.message, 'err'))
  }, [t, showToast])

  const launch = useCallback((name) => {
    setBusy(name)
    setError('')
    api.payloads
      .launch(name)
      .then(() => { showToast(t('payloads.launchedToast', { name })); load() })
      .catch((e) => {
        setError(e.message)
        showToast(t('payloads.launchFailedToast', { name }), 'err')
      })
      .finally(() => setBusy(''))
  }, [t, showToast, load])

  const remove = (name) =>
    api.payloads.delete(name).then(() => {
      load()
      showToast(t('payloads.deletedToast', { name }))
    })

  const toggleEnabled = useCallback((p) => {
    api.payloads
      .setEnabled(p.name, !p.enabled)
      .then(() => {
        load()
        showToast(t(p.enabled ? 'payloads.disabledToast' : 'payloads.enabledToast', { name: p.name }))
      })
      .catch((e) => showToast(e.message, 'err'))
  }, [t, showToast, load])

  const toggleAutoloader = useCallback((name, remove) => {
    api.autoloader.getConfig()
      .then((c) => {
        const sequence = remove
          ? c.sequence.filter((step) => !(step.type === 'load' && step.value === name))
          : [...c.sequence, { type: 'load', value: name }]
        return api.autoloader.setConfig({ ...c, sequence })
      })
      .then(() => {
        loadAutoloader()
        showToast(t(remove ? 'payloads.removedFromAutoloaderToast' : 'payloads.addedToAutoloaderToast', { name }))
      })
      .catch((e) => showToast(e.message, 'err'))
  }, [t, showToast, loadAutoloader])

  const toggleFavorite = useCallback((p) => {
    api.payloads.setFavorite(p.name, !p.favorite)
      .then(() => { load(); showToast(t(p.favorite ? 'payloads.unpinnedToast' : 'payloads.pinnedToast', { name: p.name })) })
      .catch((e) => showToast(e.message, 'err'))
  }, [t, showToast, load])

  // Stable references so PayloadButton's memo isn't broken by fresh
  // arrow functions on every render.
  const handleDelete = useCallback((name) => setDialog({ kind: 'delete', name }), [])
  const handleRename = useCallback((name) => setDialog({ kind: 'rename', name }), [])

  const rename = (name, next) =>
    api.payloads.rename(name, next).then((result) => {
      load()
      showToast(`${t('payloads.renamedToast', { name, next })}${result.warning ? ` · ${result.warning}` : ''}`)
    })

  const importFiles = (files) => {
    const existing = new Set(payloads.map((payload) => payload.name))
    const conflicts = files.filter((file) => existing.has(file.name))
    if (conflicts.length) {
      setUploadConflict({ files, conflicts: conflicts.map((file) => ({ name: file.name })) })
      return
    }
    runImport(files.map((file) => ({ file, name: file.name })))
  }

  const resolveImportConflict = (policy) => {
    const { files } = uploadConflict
    setUploadConflict(null)
    const taken = new Set(payloads.map((payload) => payload.name))
    const items = []
    for (const file of files) {
      if (!taken.has(file.name) || policy === 'replace') items.push({ file, name: file.name })
      else if (policy === 'keep_both') {
        const name = uniquePayloadName(file.name, taken)
        taken.add(name)
        items.push({ file, name })
      }
    }
    if (items.length) runImport(items)
  }

  const runImport = async (items) => {
    setError('')
    let completed = 0
    for (const { file, name } of items) {
      let startedAt = performance.now()
      setUploadJob({ name, loaded: 0, total: file.size, speed: 0, retrying: false })
      try {
        await api.payloads.upload(
          file,
          (loaded, total) => {
            const elapsed = (performance.now() - startedAt) / 1000
            setUploadJob({ name, loaded, total, speed: elapsed > 0.15 ? loaded / elapsed : 0, retrying: false })
          },
          () => {
            startedAt = performance.now()
            setUploadJob({ name, loaded: 0, total: file.size, speed: 0, retrying: true })
          },
          name,
        )
        completed++
      } catch (e) {
        setError(`${t('payloads.uploadFailedToast', { name })}: ${e.message}`)
        showToast(t('payloads.uploadFailedToast', { name }), 'err')
        break
      }
    }
    setUploadJob(null)
    if (completed > 0) {
      load()
      showToast(completed === 1 ? t('payloads.uploadedToast', { name: items[0].name }) : t('payloads.uploadedCountToast', { count: completed }))
    }
  }

  const doImport = (e) => {
    const files = Array.from(e.target.files || [])
    e.target.value = ''
    if (files.length) importFiles(files)
  }

  const importFromConsole = (selectedPath) => {
    return api.payloads.importPath(selectedPath).then((result) => {
      setConsolePickerOpen(false)
      load()
      showToast(t('payloads.importedToast', { name: result.name }))
    })
  }

  // Running processes that match no known payload — started outside Prospero or
  // since deleted. Rendered as regular tiles, not a separate list.
  const externalRunning = useMemo(() => {
    return processes
      .filter((proc) => {
        if (!isPayloadLikeProcessName(proc.name)) return false
        return !payloads.some((p) => findRunningPid(p.name, [proc]))
      })
      .map((proc) => ({
        name: proc.name,
        size_bytes: 0,
        bundled: false,
        enabled: true,
        external: true,
        pid: proc.pid,
      }))
  }, [processes, payloads])

  const q = search.trim().toLocaleLowerCase(locale)
  const items = useMemo(() => (
    [...payloads, ...externalRunning]
      .filter((p) => !q || p.name.toLocaleLowerCase(locale).includes(q))
      .sort((a, b) => a.name.localeCompare(b.name, locale))
  ), [payloads, externalRunning, q, locale])
  const favorites = items.filter((p) => p.favorite)
  const others = items.filter((p) => !p.favorite)

  // A tap pins in pin mode, otherwise it stops a running payload or launches it.
  const tap = useCallback((p, pid) => {
    if (pinMode) {
      if (!p.external) toggleFavorite(p)
    } else if (pid) {
      // The red Stop button is the way to stop it, so a stray tap does not.
    } else if (!p.external && !p.enabled) {
      showToast(t('payloads.disabledTapHint', { name: p.name }))
    } else {
      launch(p.name)
    }
  }, [pinMode, toggleFavorite, launch, showToast, t])

  const tiles = (list) => (
    <div className={`grid gap-4 ${GRID}`}>
      {list.map((p, i) => (
        <PayloadButton
          key={p.external ? `ext-${p.pid}` : p.name}
          p={p}
          index={i}
          pinMode={pinMode}
          busy={busy}
          runningPid={p.external ? p.pid : findRunningPid(p.name, processes)}
          inAutoloader={autoloaderNames.has(p.name)}
          onTap={tap}
          onStop={killPayload}
          onDelete={handleDelete}
          onToggleEnabled={toggleEnabled}
          onToggleAutoloader={toggleAutoloader}
          onRename={handleRename}
          t={t}
          locale={locale}
        />
      ))}
    </div>
  )

  const title = pinMode ? t('payloads.modeFavoritesTitle') : t('payloads.launchTitle')

  return (
    <div>
      <Panel
        title={title}
        actions={
          <div className="flex flex-wrap items-center justify-end gap-2">
            <Button size="sm" tone="neutral" icon={<UploadIcon size={14} />} disabled={Boolean(uploadJob)} onClick={() => fileInputRef.current?.click()}>
              {uploadJob ? t('payloads.uploading') : t('payloads.upload')}
            </Button>
            <Button size="sm" tone="neutral" icon={<FolderIcon size={14} />} disabled={Boolean(uploadJob)} onClick={() => setConsolePickerOpen(true)}>
              {t('payloads.fromPs5')}
            </Button>
            <Button size="sm" tone="neutral" icon={<ClockIcon size={14} />} onClick={() => onNavigate?.('autoloader')}>
              {t('payloads.autoloader')}
            </Button>
            <Button size="icon" tone="ghost" title={t('payloads.refresh')} onClick={load} icon={<RefreshIcon size={16} />} />
            <button
              type="button"
              onClick={() => setPinMode(!pinMode)}
              title={t('payloads.modeFavorites')}
              aria-label={t('payloads.modeFavorites')}
              aria-pressed={pinMode}
              className={`pmgr-icon-btn grid h-9 w-9 place-items-center rounded-full ${pinMode ? 'bg-warn/15 text-warn' : 'text-text-dim hover:text-text'}`}
            >
              <StarIcon size={18} filled={pinMode} />
            </button>
          </div>
        }
      >
        <DropZone global onFiles={importFiles} label={t('payloads.dropLabel')}>
          {pinMode && <p className="mb-4 text-xs text-text-dim">{t('payloads.modeFavoritesHint')}</p>}

          {updates.length > 0 && (
            <div className="mb-4 rounded-lg border border-accent/30 bg-accent/[0.06]">
              <div className="flex items-center justify-between gap-3 px-3 py-2">
                <span className="flex items-center gap-2 text-sm text-text"><RefreshIcon size={15} />{t('payloads.updatesAvailableTitle', { count: updates.length })}</span>
                <Button size="sm" tone="neutral" onClick={() => setShowUpdates(!showUpdates)} aria-expanded={showUpdates}>
                  {showUpdates ? t('common.close') : t('payloads.reviewUpdates')}
                </Button>
              </div>
              {showUpdates && (
                <div className="space-y-1.5 border-t border-accent/20 p-3">
                  {updates.map(({ payload, entry }) => (
                    <div key={payload.name} className="flex flex-wrap items-center justify-between gap-2 rounded-lg border border-panel-border bg-bg/55 px-3 py-2">
                      <div className="min-w-0">
                        <div className="truncate text-sm font-medium text-text">{entry.name}</div>
                        <div className="pmgr-mono text-xs text-text-dim" dir="ltr">
                          {payload.installed_version || t('payloads.updatesUnknownVersion')} → {entry.version}
                        </div>
                      </div>
                      <Button size="sm" tone="primary" onClick={() => setUpdateEntry(entry)}>{t('payloads.updateAction')}</Button>
                    </div>
                  ))}
                </div>
              )}
            </div>
          )}

          {uploadJob && (
            <JobCard
              t={t}
              steps={[t('pkg.stepUploading'), t('payloads.stepSaved')]}
              activeIndex={0}
              item={{ name: uploadJob.name }}
              label={uploadJob.name}
              pct={uploadJob.total > 0 ? Math.min(100, (uploadJob.loaded / uploadJob.total) * 100) : 0}
              sub={uploadJob.retrying ? t('files.retrying') : undefined}
              stats={[
                { label: t('pkg.statSpeed'), value: formatSpeed(uploadJob.speed, locale) },
                { label: t('pkg.statSent'), value: `${formatTransferSize(uploadJob.loaded, locale)} / ${formatTransferSize(uploadJob.total, locale)}` },
                { label: t('files.eta'), value: formatEta(uploadJob.speed > 0 ? (uploadJob.total - uploadJob.loaded) / uploadJob.speed : Infinity) },
              ]}
            />
          )}

          <input ref={fileInputRef} type="file" multiple hidden onChange={doImport} />

          {error && <div className="pmgr-notice-in mb-3 rounded-md bg-err/10 px-3 py-2 text-sm text-err">{error}</div>}

          {payloads.length >= SEARCH_FROM && (
            <SearchField value={search} onChange={setSearch} placeholder={t('payloads.searchPlaceholder')} className="mb-4 sm:max-w-64" />
          )}

          {!loaded ? (
            <div className={`grid gap-3 ${GRID}`}>
              {Array.from({ length: 6 }, (_, i) => <div key={i} className="pmgr-skeleton h-[9rem] rounded-2xl border border-panel-border bg-panel" />)}
            </div>
          ) : items.length === 0 ? (
            payloads.length === 0 ? (
              <div>
                <EmptyState icon={<LaunchIcon size={22} />} title={t('payloads.emptyTitle')} description={t('payloads.emptyDesc')} />
                <div className="mt-4 grid grid-cols-1 gap-3 sm:grid-cols-3">
                  {[
                    { icon: <UploadIcon size={18} />, label: t('payloads.upload'), desc: t('payloads.emptyUploadDesc'), onClick: () => fileInputRef.current?.click() },
                    { icon: <FolderIcon size={18} />, label: t('payloads.fromPs5'), desc: t('payloads.emptyPs5Desc'), onClick: () => setConsolePickerOpen(true) },
                    { icon: <StarIcon size={18} />, label: t('nav.payloadRepositories'), desc: t('payloads.emptyRepoDesc'), onClick: () => onNavigate?.('repository') },
                  ].map((cta) => (
                    <button key={cta.label} type="button" onClick={cta.onClick} className="pmgr-tile rounded-xl border border-panel-border bg-panel p-4 text-left">
                      <span className="mb-2 grid h-9 w-9 place-items-center rounded-lg bg-accent/10 text-accent">{cta.icon}</span>
                      <div className="text-sm font-bold text-text">{cta.label}</div>
                      <div className="mt-0.5 text-xs text-text-dim">{cta.desc}</div>
                    </button>
                  ))}
                </div>
              </div>
            ) : (
              <EmptyState size="sm" title={t('payloads.noMatchTitle')} description={t('payloads.noMatchDesc')} />
            )
          ) : (
            <>
              {favorites.length > 0 && (
                <div className="mb-6">
                  <SectionHeading label={t('payloads.favoritesHeading')} count={favorites.length} />
                  {tiles(favorites)}
                </div>
              )}
              {others.length > 0 && (
                <div>
                  {favorites.length > 0 && <SectionHeading label={t('payloads.othersHeading')} count={others.length} />}
                  {tiles(others)}
                </div>
              )}
            </>
          )}
        </DropZone>
      </Panel>
      {consolePickerOpen && (
        <PathPickerModal
          title={t('payloads.importTitle')}
          description={t('payloads.importDesc')}
          initialPath="/data"
          volumes={volumes}
          mode="file"
          extensions={['.elf', '.bin', '.self']}
          actionLabel={t('payloads.importAction')}
          onClose={() => setConsolePickerOpen(false)}
          onSelect={importFromConsole}
        />
      )}

      {dialog?.kind === 'delete' && (
        <ConfirmDialog
          title={t('payloads.deleteTitle', { name: dialog.name })}
          description={t('payloads.deleteDesc', { name: dialog.name })}
          tone="danger"
          confirmLabel={t('payloads.delete')}
          onClose={() => setDialog(null)}
          onConfirm={() => remove(dialog.name)}
        />
      )}
      {dialog?.kind === 'rename' && (
        <InputDialog
          title={t('payloads.renameTitle', { name: dialog.name })}
          label={t('payloads.renameLabel')}
          defaultValue={dialog.name}
          confirmLabel={t('payloads.rename')}
          onClose={() => setDialog(null)}
          onConfirm={(next) => (next === dialog.name ? Promise.resolve() : rename(dialog.name, next))}
        />
      )}
      {uploadConflict && (
        <TransferConflictDialog
          conflict={uploadConflict}
          onClose={() => setUploadConflict(null)}
          onResolve={resolveImportConflict}
          t={t}
        />
      )}

      {updateEntry && (
        <DownloadPayloadDialog
          entry={updateEntry}
          onClose={() => setUpdateEntry(null)}
          onDownloaded={() => {
            showToast(t('repositories.downloadStarted', { name: updateEntry.name }))
            load()
          }}
        />
      )}
    </div>
  )
}

