import { useEffect, useMemo, useRef, useState } from 'react'
import Panel from '../components/Panel.jsx'
import Pill from '../components/Pill.jsx'
import PlatformBadge from '../components/PlatformBadge.jsx'
import Button from '../components/Button.jsx'
import Toggle from '../components/Toggle.jsx'
import CollapsibleSection from '../components/CollapsibleSection.jsx'
import CheckOption from '../components/CheckOption.jsx'
import EmptyState from '../components/EmptyState.jsx'
import SegmentedControl from '../components/SegmentedControl.jsx'
import ActionMenu from '../components/ActionMenu.jsx'
import { SkeletonRows } from '../components/Skeleton.jsx'
import PathPickerModal from '../components/PathPickerModal.jsx'
import GdriveDestination from '../components/GdriveDestination.jsx'
import { ConfirmDialog } from '../components/Dialog.jsx'
import { DownloadIcon, UploadIcon, RefreshIcon, SaveIcon, DeleteIcon, ShieldIcon, FolderIcon } from '../components/icons.jsx'
import { api } from '../lib/api.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'
import { formatDate, formatFileSize } from '../lib/format.js'

function formatWhen(unixSeconds, locale) {
  return formatDate(unixSeconds * 1000, locale)
}

// The backend's own message is English, so prefer the translated string its code
// names and keep the raw text only for unknown codes.
const SAVE_ERROR_KEYS = {
  title_running: 'saves.errorTitleRunning',
  no_saves: 'saves.errorNoSaves',
  no_files: 'saves.errorNoFiles',
  missing_key: 'saves.errorMissingKey',
  unknown_user: 'saves.errorUnknownUser',
  unsafe_path: 'saves.errorUnsafePath',
  invalid_zip: 'saves.errorInvalidZip',
  snapshot_failed: 'saves.errorSnapshotFailed',
  unpack_failed: 'saves.errorUnpackFailed',
  replace_failed: 'saves.errorReplaceFailed',
  delete_failed: 'saves.errorDeleteFailed',
  gdrive_auth: 'saves.errorGdriveAuth',
  gdrive_not_linked: 'saves.errorGdriveNotLinked',
}

function saveErrorText(t, code, fallback) {
  return code && SAVE_ERROR_KEYS[code] ? t(SAVE_ERROR_KEYS[code]) : fallback
}

// Why one USB destination did not take a backup, in the user's language.
const DEST_RESULT_KEYS = {
  disconnected: 'saves.destDisconnected',
  read_only: 'saves.destReadOnly',
  busy: 'saves.destBusy',
  not_usb: 'saves.destNotUsb',
  unsafe: 'saves.destUnsafe',
  no_space: 'saves.resultNoSpace',
  write_failed: 'saves.resultWriteFailed',
  verify_failed: 'saves.resultVerifyFailed',
  no_saves: 'saves.errorNoSaves',
}

function destResultText(t, result) {
  return result.code && DEST_RESULT_KEYS[result.code] ? t(DEST_RESULT_KEYS[result.code]) : result.error
}

const DEST_STATE_KEYS = { ok: 'saves.destOk', ...DEST_RESULT_KEYS }

const DEFAULT_BACKUP_DIR = '/data/ProsperoMgr/save_backups'

const SAVE_NAMES_SHOWN = 3

// The console writes "(empty)" and "-" for slots with no name; they say nothing.
const cleanSaveText = (text) => (/^\(?empty\)?$|^-+$/i.test((text || '').trim()) ? '' : (text || '').trim())

function saveNames(files, gameName) {
  const names = []
  for (const f of files) {
    if (f.filename.startsWith('sdimg_sce_bu_')) continue
    const name = cleanSaveText(f.save_subtitle) || cleanSaveText(f.save_title)
    if (name && name !== gameName && !names.some((n) => n.toLowerCase() === name.toLowerCase())) names.push(name)
  }
  return names
}

// One quiet line with the names of the saves inside; the console fills in nothing for many games.
function SaveNames({ files, gameName, t }) {
  const names = saveNames(files, gameName)
  if (names.length === 0) return null
  const shown = names.slice(0, SAVE_NAMES_SHOWN)
  const rest = names.length - shown.length
  return (
    <div className="mt-0.5 truncate text-xs text-text-dim" title={names.join('\n')}>
      {shown.join(' · ')}
      {rest > 0 && ` · ${t('saves.moreSaves', { count: rest })}`}
    </div>
  )
}

// A game's saves normally share one account, shown once with the user. Only a mix is worth flagging.
function SaveOwner({ files, t }) {
  const ids = [...new Set(files.map((f) => f.account_id).filter(Boolean))]
  if (ids.length < 2) return null
  return (
    <span className="ml-2 shrink-0 text-[11px] font-semibold text-warn" title={ids.join('\n')}>{t('saves.mixedAccounts', { count: ids.length })}</span>
  )
}

function SaveArtwork({ save }) {
  const [failed, setFailed] = useState(false)

  return (
    <div className="grid h-14 w-14 shrink-0 place-items-center overflow-hidden rounded-xl border border-panel-border bg-bg text-accent/55">
      {!failed ? (
        <img
          src={api.saves.iconUrl(save.user_id_hex, save.title_id, save.filename)}
          alt={`${save.title_name || save.title_id} icon`}
          onError={() => setFailed(true)}
          loading="lazy"
          decoding="async"
          className="h-full w-full object-cover"
        />
      ) : (
        <svg width="25" height="25" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5" aria-hidden="true">
          <rect x="3" y="5" width="18" height="14" rx="2" />
          <path d="M8 9v6M5 12h6M14 10h.01M17 13h.01" strokeLinecap="round" />
        </svg>
      )}
    </div>
  )
}

// A Google Drive backup can be restored or deleted from here, but not downloaded through the console.
function backupMenuItems(b, t, { onDownload, onDelete }) {
  const items = []
  if (!b.remote) items.push({ label: t('saves.download'), icon: <DownloadIcon size={15} />, onClick: onDownload }, '-')
  items.push({ label: t('saves.deleteBackupTitle2'), icon: <DeleteIcon size={15} />, tone: 'danger', onClick: onDelete })
  return items
}

function BackupRow({ b, busy, onDownload, onRestore, onDelete, t, locale }) {
  return (
    <div data-menu-host className="flex flex-wrap items-center gap-3 rounded-lg border border-panel-border px-3 py-2">
      <div className="min-w-0 flex-1">
        <div className="text-sm text-text">{formatWhen(b.created_unix, locale)}</div>
        <div className="truncate text-xs text-text-dim" dir="ltr">{formatFileSize(b.size_bytes, locale)} · {b.destination}</div>
      </div>
      <div className="flex shrink-0 items-center">
        <Button size="sm" tone="neutral" disabled={busy} onClick={onRestore}>{t('saves.restore')}</Button>
        <ActionMenu items={backupMenuItems(b, t, { onDownload, onDelete })} />
      </div>
    </div>
  )
}

// Auto-backup: waits for a title to close and settle, then zips its saves to the chosen drives.
// Config and the resulting backups live together since one drives the other.
function AutoBackupPanel({ refreshKey, onJobStarted }) {
  const { t, locale } = useI18n()
  const [config, setConfig] = useState(null)
  const [backups, setBackups] = useState(null)
  const [driveBackups, setDriveBackups] = useState([])
  const [gdriveOn, setGdriveOn] = useState(false)
  const [gdriveOnly, setGdriveOnly] = useState(false)
  const [loadError, setLoadError] = useState('')
  const [busyPath, setBusyPath] = useState('')
  const [dialog, setDialog] = useState(null) // { kind: 'restore'|'delete', backup }
  const [destPickerOpen, setDestPickerOpen] = useState(false)
  const [copyPickerOpen, setCopyPickerOpen] = useState(false)
  const [volumes, setVolumes] = useState([])
  const [backupAll, setBackupAll] = useState(null) // { running, done, total, failed, current_title_id }
  const [skipSnapshot, setSkipSnapshot] = useState(false)
  const [settingsOpen, setSettingsOpen] = useState(null) // null follows whether a destination has a problem
  const backupAllPollRef = useRef(null)
  const showToast = useToast()

  const load = () => {
    setLoadError('')
    api.saves.getBackupConfig().then(setConfig).catch((error) => setLoadError(error.message))
    api.saves.listBackups().then((r) => setBackups(r.backups || [])).catch((error) => setLoadError(error.message))
    // Drive is asked separately and last: it waits on the network, and a slow or offline Google
    // must never hold up the backups that are already on the console or a USB drive.
    api.gdrive
      .status()
      .then((g) => {
        setGdriveOn(g.linked && g.enabled)
        setGdriveOnly(g.linked && g.enabled && g.drive_only)
        if (!(g.linked && g.enabled)) return setDriveBackups([])
        return api.gdrive
          .backups()
          .then((r) => setDriveBackups(r.backups || []))
          .catch((error) => showToast(t('saves.gdriveListError', { message: saveErrorText(t, error.error_code, error.message) }), 'err'))
      })
      .catch(() => {})
  }
  useEffect(load, [refreshKey])
  useEffect(() => {
    api.ftp.storage().then((r) => setVolumes(r.volumes || [])).catch(() => {})
    return () => clearInterval(backupAllPollRef.current)
  }, [])

  const destinationPaths = (c) => (c.destinations || []).map((d) => d.path)

  const toggle = (checked) => {
    const next = { ...config, enabled: checked }
    setConfig(next)
    api.saves
      .setBackupConfig(next.enabled, next.settle_seconds, destinationPaths(next))
      .then(() => showToast(t(next.enabled ? 'saves.autoBackupEnabledToast' : 'saves.autoBackupDisabledToast')))
      .catch((e) => showToast(e.message, 'err'))
  }

  const changeSettle = (seconds) => {
    const next = { ...config, settle_seconds: seconds }
    setConfig(next)
    api.saves.setBackupConfig(next.enabled, seconds, destinationPaths(next)).catch((e) => showToast(e.message, 'err'))
  }

  // The backend re-checks every path, so a rejection here is the final word and the list reloads.
  const saveDestinations = (paths) =>
    api.saves
      .setBackupConfig(config.enabled, config.settle_seconds, paths)
      .then(() => showToast(paths.length ? t('saves.backupDestChangedToast') : t('saves.backupDestResetToast')))
      .catch((e) => showToast(e.message, 'err'))
      .finally(load)

  const addDestination = (path) => {
    const paths = destinationPaths(config)
    if (!paths.includes(path)) saveDestinations([...paths, path])
  }

  const removeDestination = (path) => saveDestinations(destinationPaths(config).filter((p) => p !== path))

  const startBackupAll = () => {
    api.saves
      .backupAll()
      .then(() => {
        backupAllPollRef.current = setInterval(() => {
          api.saves.backupAllStatus().then((s) => {
            setBackupAll(s)
            if (!s.running) {
              clearInterval(backupAllPollRef.current)
              showToast(
                s.failed > 0
                  ? t('saves.backupAllPartialToast', { done: s.done - s.failed, total: s.total })
                  : t('saves.backupAllDoneToast', { total: s.total }),
                s.failed > 0 ? 'err' : undefined,
              )
              load()
            }
          }).catch(() => {})
        }, 500)
      })
      .catch((e) => showToast(e.message, 'err'))
  }

  // Only starts the job; the outcome arrives through the parent's watcher. A
  // rejection here is the request itself being refused, which the dialog shows.
  const restore = (b) => {
    setBusyPath(b.path)
    return api.saves
      .restoreBackup(b.path, !skipSnapshot)
      .then(() => onJobStarted())
      .catch((e) => { throw new Error(saveErrorText(t, e.code, e.message)) })
      .finally(() => setBusyPath(''))
  }

  const remove = (b) =>
    api.saves.deleteBackup(b.path).then(() => {
      showToast(t('saves.backupDeletedToast'))
      load()
    })

  // Grouped by title, newest backup leading. A flat list gets unreadable past a
  // couple of games, so older ones sit behind History.
  const groups = useMemo(() => {
    if (!backups) return null
    const byTitle = new Map()
    for (const b of [...backups, ...driveBackups]) {
      if (!byTitle.has(b.title_id)) byTitle.set(b.title_id, { title_id: b.title_id, title_name: b.title_name, platform: b.platform, items: [] })
      byTitle.get(b.title_id).items.push(b)
    }
    for (const g of byTitle.values()) g.items.sort((a, b) => b.created_unix - a.created_unix)
    return [...byTitle.values()].sort((a, b) => b.items[0].created_unix - a.items[0].created_unix)
  }, [backups, driveBackups])

  const destIssue = Boolean(config?.destinations?.some((d) => d.state !== 'ok'))
  const settingsShown = settingsOpen ?? destIssue
  const percentAll = backupAll?.total ? Math.round((backupAll.done / backupAll.total) * 100) : 0

  return (
    <Panel
      title={t('saves.autoBackupTitle')}
      className="mb-4"
      actions={config && <Pill tone={config.enabled ? 'ok' : 'neutral'}>{t(config.enabled ? 'saves.enabled' : 'saves.disabled')}</Pill>}
    >
      {loadError && (
        <div className="pmgr-notice-in mb-3 flex items-center justify-between rounded-xl border border-err/20 bg-err/10 px-3 py-2.5 text-sm text-err">
          <span className="mr-3">{loadError}</span>
          <Button size="sm" tone="danger" onClick={load}>{t('common.tryAgain')}</Button>
        </div>
      )}

      {!config ? (
        !loadError && <SkeletonRows count={2} />
      ) : (
        <div className="mb-5">
          <div className="flex flex-wrap items-center justify-between">
            <div className="mb-2 mr-4 flex min-w-0 flex-[1_1_16rem] items-center">
              <Toggle checked={config.enabled} onChange={toggle} label={t('saves.autoBackupTitle')} />
              <p className="ml-3 min-w-0 text-sm text-text-dim">{t('saves.autoBackupDesc', { path: DEFAULT_BACKUP_DIR })}</p>
            </div>
            <Button
              className="mb-2"
              tone="primary"
              icon={<SaveIcon size={15} />}
              title={t('saves.backupAllDesc')}
              disabled={backupAll?.running}
              onClick={startBackupAll}
            >
              {backupAll?.running ? t('saves.backupAllRunning') : t('saves.backupAllAction')}
            </Button>
          </div>

          {backupAll?.running && (
            <div className="mb-3" aria-live="polite">
              <div className="mb-1 text-xs text-text-dim">{t('saves.backupAllProgress', { done: backupAll.done, total: backupAll.total })}</div>
              <div className="h-1.5 overflow-hidden rounded-full bg-panel-border" role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={percentAll}>
                <div className="h-full rounded-full bg-info transition-[width] duration-300" style={{ width: `${percentAll}%` }} />
              </div>
            </div>
          )}

          <div className="flex items-center justify-between border-t border-panel-border pt-3">
            <div className={`mr-3 flex min-w-0 items-center text-xs ${destIssue ? 'text-err' : 'text-text-dim'}`}>
              <FolderIcon size={13} className="mr-1.5 shrink-0" />
              <span className="truncate pmgr-mono" dir="ltr">
                {gdriveOnly
                  ? t('saves.gdriveTitle')
                  : [
                      config.destinations.length ? config.destinations.map((d) => d.path).join(' · ') : t('saves.backupDestInternal'),
                      gdriveOn ? t('saves.gdriveTitle') : '',
                    ].filter(Boolean).join(' · ')}
              </span>
            </div>
            <button
              type="button"
              onClick={() => setSettingsOpen(!settingsShown)}
              aria-expanded={settingsShown}
              className="inline-flex min-h-9 shrink-0 items-center rounded-lg px-2 text-xs font-semibold text-text-dim hover:text-text"
            >
              {t('nav.settings')}
              <span className={`pmgr-chevron ml-1.5 ${settingsShown ? 'pmgr-chevron-open' : ''}`}>
                <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round"><path d="M6 9l6 6 6-6" /></svg>
              </span>
            </button>
          </div>

          <div className={`pmgr-collapse ${settingsShown ? 'is-open' : ''}`}>
            <div>
              <div className="pt-3">
                <div className="flex flex-wrap items-center justify-between">
                  <label className="mb-2 mr-3 flex items-center text-sm text-text">
                    <span className="mr-2">{t('saves.settleWindow')}</span>
                    <select
                      value={config.settle_seconds}
                      onChange={(e) => changeSettle(Number(e.target.value))}
                      className="min-h-9 rounded-md border border-panel-border bg-bg px-2 py-1.5 text-sm outline-none focus:border-accent"
                    >
                      {[5, 10, 15, 30, 60].map((s) => (
                        <option key={s} value={s}>{t('saves.settleSeconds', { seconds: s })}</option>
                      ))}
                    </select>
                  </label>
                  <div className="mb-2 flex shrink-0 items-center">
                    <Button className="mr-2" size="sm" tone="neutral" icon={<FolderIcon size={14} />} onClick={() => setDestPickerOpen(true)}>
                      {t('saves.backupDestAdd')}
                    </Button>
                    <Button size="sm" tone="neutral" icon={<SaveIcon size={14} />} onClick={() => setCopyPickerOpen(true)}>
                      {t('saves.copyBackupsAction')}
                    </Button>
                  </div>
                </div>
                {config.destinations.map((d) => (
                  <div key={d.path} className="mb-2 flex flex-wrap items-center rounded-lg border border-panel-border px-3 py-2">
                    <div className="mr-3 min-w-0 flex-1">
                      <div className="truncate pmgr-mono text-sm text-text" dir="ltr">{d.path}</div>
                      {d.state === 'ok' && (
                        <div className="text-xs text-text-dim">{t('saves.destFree', { free: formatFileSize(d.free_bytes, locale) })}</div>
                      )}
                    </div>
                    <Pill tone={d.state === 'ok' ? 'ok' : 'err'}>{t(DEST_STATE_KEYS[d.state] || 'saves.destUnsafe')}</Pill>
                    <Button className="ml-2" size="icon" tone="ghost" title={t('saves.backupDestRemove')} onClick={() => removeDestination(d.path)} icon={<DeleteIcon size={15} />} />
                  </div>
                ))}
                <GdriveDestination onChanged={load} />
              </div>
            </div>
          </div>
        </div>
      )}

      {copyPickerOpen && (
        <PathPickerModal
          title={t('saves.copyBackupsTitle')}
          description={t('saves.copyBackupsDesc')}
          initialPath="/mnt"
          volumes={volumes}
          mode="folder"
          actionLabel={t('saves.copyBackupsAction')}
          onClose={() => setCopyPickerOpen(false)}
          onSelect={(selected) => {
            setCopyPickerOpen(false)
            api.ftp.copy(DEFAULT_BACKUP_DIR, `${selected.replace(/\/$/, '')}/save_backups`, 'skip')
              .then(() => showToast(t('files.jobQueued')))
              .catch((e) => showToast(e.message, 'err'))
          }}
        />
      )}

      {destPickerOpen && (
        <PathPickerModal
          title={t('saves.backupDestPickerTitle')}
          description={t('saves.backupDestPickerDesc')}
          initialPath={config?.devices?.[0]?.path || '/mnt'}
          volumes={config?.devices || []}
          mode="folder"
          actionLabel={t('common.add')}
          onClose={() => setDestPickerOpen(false)}
          onSelect={(selected) => {
            setDestPickerOpen(false)
            addDestination(selected.replace(/\/$/, ''))
          }}
        />
      )}

      {groups === null ? (
        !loadError && <SkeletonRows count={3} />
      ) : groups.length === 0 ? (
        <EmptyState size="sm" icon={<ShieldIcon size={20} />} title={t('saves.noBackupsTitle')} description={t('saves.noBackupsDesc')} />
      ) : (
        <div className="space-y-2">
          {groups.map((g) => {
            const latest = g.items[0]
            const older = g.items.slice(1)
            return (
              <div key={g.title_id} className="rounded-xl border border-panel-border">
                <div data-menu-host className="flex flex-wrap items-center gap-3 px-3 py-2.5">
                  <div className="min-w-0 flex-1">
                    <div className="flex items-center gap-2">
                      <span className="truncate text-sm font-medium text-text">{g.title_name || g.title_id}</span>
                      {g.platform && <PlatformBadge platform={g.platform} />}
                      {older.length > 0 && <Pill tone="neutral">{t('saves.backupsCount', { count: g.items.length })}</Pill>}
                    </div>
                    <div className="text-xs text-text-dim">{t('saves.latestBackup', { date: formatWhen(latest.created_unix, locale), size: formatFileSize(latest.size_bytes, locale) })}</div>
                  </div>
                  <div className="flex shrink-0 items-center">
                    <Button size="sm" tone="neutral" disabled={busyPath === latest.path} onClick={() => setDialog({ kind: 'restore', backup: latest })}>{t('saves.restore')}</Button>
                    <ActionMenu
                      items={backupMenuItems(latest, t, {
                        onDownload: () => window.open(api.saves.backupDownloadUrl(latest.path), '_blank'),
                        onDelete: () => setDialog({ kind: 'delete', backup: latest }),
                      })}
                    />
                  </div>
                </div>
                {older.length > 0 && (
                  <CollapsibleSection title={t('saves.historyOlder', { count: older.length })} defaultOpen={false}>
                    <div className="space-y-2">
                      {older.map((b) => (
                        <BackupRow
                          key={b.path}
                          b={b}
                          busy={busyPath === b.path}
                          onDownload={() => window.open(api.saves.backupDownloadUrl(b.path), '_blank')}
                          onRestore={() => setDialog({ kind: 'restore', backup: b })}
                          onDelete={() => setDialog({ kind: 'delete', backup: b })}
                          t={t}
                          locale={locale}
                        />
                      ))}
                    </div>
                  </CollapsibleSection>
                )}
              </div>
            )
          })}
        </div>
      )}

      {dialog?.kind === 'restore' && (
        <ConfirmDialog
          title={t('saves.restoreTitle')}
          description={t('saves.restoreDesc', { name: dialog.backup.title_name || dialog.backup.title_id, date: formatWhen(dialog.backup.created_unix, locale) })}
          tone="warn"
          confirmLabel={t('saves.restore')}
          onClose={() => { setDialog(null); setSkipSnapshot(false) }}
          onConfirm={() => restore(dialog.backup)}
        >
          <CheckOption checked={skipSnapshot} onChange={setSkipSnapshot} label={t('saves.skipSnapshot')} hint={t('saves.skipSnapshotHint')} />
        </ConfirmDialog>
      )}
      {dialog?.kind === 'delete' && (
        <ConfirmDialog
          title={t('saves.deleteBackupTitle')}
          description={t('saves.deleteBackupDesc', { name: dialog.backup.title_name || dialog.backup.title_id, date: formatWhen(dialog.backup.created_unix, locale) })}
          tone="danger"
          confirmLabel={t('common.delete')}
          onClose={() => setDialog(null)}
          onConfirm={() => remove(dialog.backup)}
        />
      )}
    </Panel>
  )
}

export default function SavesView() {
  const { t, locale } = useI18n()
  const [saves, setSaves] = useState(null)
  const [error, setError] = useState('')
  const [busy, setBusy] = useState('')
  const [user, setUser] = useState('all')
  const [backupsRefresh, setBackupsRefresh] = useState(0)
  const [importFile, setImportFile] = useState(null)
  const [watchingJob, setWatchingJob] = useState(false)
  const [deleteTarget, setDeleteTarget] = useState(null)
  const [backupBeforeDelete, setBackupBeforeDelete] = useState(false)
  const importInputRef = useRef(null)
  const showToast = useToast()

  const load = () => {
    setError('')
    api.saves.list().then((r) => setSaves(r.saves)).catch((requestError) => setError(requestError.message))
  }
  useEffect(load, [])

  const groups = useMemo(() => {
    if (!saves) return []
    const byKey = new Map()
    for (const s of saves) {
      const key = `${s.user_id_hex}:${s.title_id}`
      if (!byKey.has(key)) byKey.set(key, { ...s, files: [] })
      byKey.get(key).files.push(s)
    }
    return [...byKey.values()]
  }, [saves])

  // One tab per user, so it is clear whose save is whose.
  const users = useMemo(() => {
    const byHex = new Map()
    for (const g of groups) {
      if (!byHex.has(g.user_id_hex)) byHex.set(g.user_id_hex, { hex: g.user_id_hex, name: g.user_name || g.user_id_hex, count: 0, accounts: new Set() })
      const entry = byHex.get(g.user_id_hex)
      entry.count++
      for (const f of g.files) if (f.account_id) entry.accounts.add(f.account_id)
    }
    return [...byHex.values()].map((u) => ({ ...u, account: u.accounts.size === 1 ? [...u.accounts][0] : '' }))
  }, [groups])

  // One person's list shows who it belongs to once, at the top, instead of on every row.
  const shownUser = users.length === 1 ? users[0] : users.find((u) => u.hex === user)
  const visibleGroups = user === 'all' ? groups : groups.filter((g) => g.user_id_hex === user)
  const userOptions = [{ id: 'all', label: t('saves.allUsers') }, ...users.map((u) => ({ id: u.hex, label: `${u.name} (${u.count})` }))]

  const backup = (group) => {
    const key = `${group.user_id_hex}:${group.title_id}`
    setBusy(key)
    api.saves
      .downloadZip(group.user_id_hex, group.title_id)
      .then(async (res) => {
        if (!res.ok) throw new Error((await res.json().catch(() => ({}))).error || 'backup failed')
        const blob = await res.blob()
        const url = URL.createObjectURL(blob)
        const a = document.createElement('a')
        a.href = url
        a.download = `${group.title_id}-save.zip`
        a.click()
        URL.revokeObjectURL(url)
        showToast(t('saves.downloadedBackupToast', { name: group.title_name || group.title_id }))
      })
      .catch((e) => showToast(e.message, 'err'))
      .finally(() => setBusy(''))
  }

  const backupNow = (group) => {
    const key = `now:${group.title_id}`
    setBusy(key)
    api.saves
      .backupNow(group.title_id)
      .then((res) => {
        const failed = (res.results || []).filter((r) => !r.ok)
        const name = group.title_name || group.title_id
        if (failed.length === 0) {
          showToast(t('saves.backedUpToast', { name }))
        } else {
          // Never report a copy that missed a drive as a plain success.
          const detail = failed.map((r) => `${r.destination || ''} ${destResultText(t, r)}`.trim()).join(' · ')
          showToast(`${t('saves.backupPartialToast', { name, ok: res.results.length - failed.length, total: res.results.length })} ${detail}`, 'err')
        }
        setBackupsRefresh((n) => n + 1)
      })
      .catch((e) => showToast(e.message, 'err'))
      .finally(() => setBusy(''))
  }

  // Only starts the job — the outcome arrives through the poll below. A rejection
  // here is the upload itself failing, which the dialog shows inline.
  const runImport = () => api.saves.importZip(importFile, importFile.name).then(() => setWatchingJob(true))

  // Import, restore and back-up-all all run in the background, so a refusal
  // ("close the game first") comes back here rather than on the request.
  useEffect(() => {
    if (!watchingJob) return undefined
    let cancelled = false
    const timer = window.setInterval(() => {
      api.tasks().then((tasks) => {
        if (cancelled) return
        const job = tasks?.save_job
        if (!job || job.phase === 'running') return
        setWatchingJob(false)
        if (job.phase === 'error') {
          showToast(saveErrorText(t, job.error_code, job.error || t('saves.importFailed')), 'err')
          return
        }
        showToast(t(job.kind === 'restore' ? 'saves.restoredDoneToast' : 'saves.importedToast'))
        load()
        setBackupsRefresh((n) => n + 1)
      }).catch(() => {})
    }, 1000)
    return () => {
      cancelled = true
      window.clearInterval(timer)
    }
  }, [watchingJob])

  const removeSaves = (g) => {
    setBusy(`del:${g.title_id}`)
    return api.saves
      .deleteSaves(g.user_id_hex, g.title_id, backupBeforeDelete)
      .then(() => {
        showToast(t('saves.savesDeletedToast', { name: g.title_name || g.title_id }))
        load()
        setBackupsRefresh((n) => n + 1)
      })
      .catch((e) => { throw new Error(saveErrorText(t, e.code, e.message)) })
      .finally(() => setBusy(''))
  }

  return (
    <div>
      <AutoBackupPanel refreshKey={backupsRefresh} onJobStarted={() => setWatchingJob(true)} />

      <Panel
        title={t('saves.saveDataTitle')}
        actions={
          <div className="flex items-center gap-2">
            <Button
              tone="neutral"
              icon={<UploadIcon size={15} />}
              disabled={watchingJob}
              onClick={() => importInputRef.current?.click()}
            >
              {watchingJob ? t('saves.importing') : t('saves.import')}
            </Button>
            <Button tone="neutral" icon={<RefreshIcon size={15} />} onClick={load}>{t('saves.refresh')}</Button>
          </div>
        }
      >
        <p className="mb-3 text-sm text-text-dim">{t('saves.saveDataDesc')}</p>

        {users.length > 1 && <SegmentedControl className="mb-3" options={userOptions} value={user} onChange={setUser} />}

        {shownUser && saves !== null && (
          <div className="mb-3 flex items-center rounded-xl border border-panel-border bg-bg/30 px-3 py-2.5">
            <img
              src={api.avatar.userIconUrl(shownUser.hex)}
              alt=""
              onError={(e) => { e.target.style.display = 'none' }}
              className="mr-3 h-9 w-9 shrink-0 rounded-full border border-panel-border object-cover"
            />
            <div className="min-w-0">
              <div className="truncate text-sm font-semibold text-text">{shownUser.name}</div>
              {shownUser.account && (
                <div className="pmgr-mono truncate text-xs text-text-dim" dir="ltr" title={t('saves.saveAccountId')}>{shownUser.account}</div>
              )}
            </div>
          </div>
        )}

        {error && saves !== null && (
          <div className="pmgr-notice-in mb-3 flex items-center justify-between gap-3 rounded-xl border border-err/20 bg-err/10 px-3 py-2.5 text-sm text-err">
            <span>{error}</span>
            <Button size="sm" tone="danger" onClick={load}>{t('common.tryAgain')}</Button>
          </div>
        )}

        {error && saves === null ? (
          <EmptyState
            size="sm"
            title={t('common.error')}
            description={error}
            action={<Button tone="neutral" onClick={load}>{t('common.tryAgain')}</Button>}
          />
        ) : saves === null ? (
          <SkeletonRows count={4} />
        ) : visibleGroups.length === 0 ? (
          <EmptyState size="sm" title={t('saves.noSavesTitle')} description={t('saves.noSavesDesc')} />
        ) : (
          <div className="space-y-2">
            {visibleGroups.map((g) => {
              const key = `${g.user_id_hex}:${g.title_id}`
              const totalBytes = g.files.reduce((sum, f) => sum + f.size_bytes, 0)
              return (
                <div key={key} data-menu-host className="flex flex-wrap items-center rounded-xl border border-panel-border bg-bg/30 p-3 transition-colors duration-150 hover:bg-panel-2">
                  <SaveArtwork save={g} />
                  <div className="ml-3 mr-3 min-w-0 flex-1">
                    <div className="flex items-center">
                      <span className="truncate text-base font-semibold text-text">{g.title_name || g.title_id}</span>
                      {g.platform && <span className="ml-2 shrink-0"><PlatformBadge platform={g.platform} /></span>}
                      <SaveOwner files={g.files} t={t} />
                    </div>
                    <div className="mt-0.5 flex flex-wrap items-center text-xs text-text-dim">
                      <span className="pmgr-mono mr-3" dir="ltr">{g.title_id}</span>
                      <span className="mr-3">{t('saves.filesCount', { count: g.files.length })}</span>
                      <span className="mr-3 font-semibold text-text">{formatFileSize(totalBytes, locale)}</span>
                      {!shownUser && <span className="mr-3 truncate">{g.user_name || g.user_id_hex}</span>}
                    </div>
                    <SaveNames files={g.files} gameName={g.title_name} t={t} />
                  </div>
                  <Button
                    size="sm"
                    tone="primary"
                    title={t('saves.backUpNowTitle')}
                    disabled={busy === `now:${g.title_id}`}
                    onClick={() => backupNow(g)}
                    icon={<SaveIcon size={15} />}
                  >
                    <span className="hidden sm:inline">{t('saves.backUpNow')}</span>
                  </Button>
                  <ActionMenu
                    items={[
                      { label: t('saves.downloadZipTitle'), icon: <DownloadIcon size={15} />, disabled: busy === key, onClick: () => backup(g) },
                      '-',
                      { label: t('saves.deleteSaveAction'), icon: <DeleteIcon size={15} />, tone: 'danger', disabled: busy === `del:${g.title_id}`, onClick: () => setDeleteTarget(g) },
                    ]}
                  />
                </div>
              )
            })}
          </div>
        )}
      </Panel>

      <input
        ref={importInputRef}
        type="file"
        accept=".zip,application/zip"
        className="hidden"
        onChange={(e) => {
          const file = e.target.files?.[0]
          // Clear it so picking the same file twice in a row still fires onChange.
          e.target.value = ''
          if (file) setImportFile(file)
        }}
      />

      {importFile && (
        <ConfirmDialog
          title={t('saves.importTitle')}
          description={t('saves.importDesc', { name: importFile.name })}
          tone="warn"
          confirmLabel={t('saves.import')}
          onClose={() => setImportFile(null)}
          onConfirm={runImport}
        />
      )}

      {deleteTarget && (
        <ConfirmDialog
          title={t('saves.deleteSaveTitle')}
          description={t('saves.deleteSaveDesc', { name: deleteTarget.title_name || deleteTarget.title_id })}
          tone="danger"
          confirmLabel={t('common.delete')}
          onClose={() => { setDeleteTarget(null); setBackupBeforeDelete(false) }}
          onConfirm={() => removeSaves(deleteTarget)}
        >
          <CheckOption checked={backupBeforeDelete} onChange={setBackupBeforeDelete} label={t('saves.deleteBackupFirst')} hint={t('saves.deleteBackupHint')} />
        </ConfirmDialog>
      )}
    </div>
  )
}
