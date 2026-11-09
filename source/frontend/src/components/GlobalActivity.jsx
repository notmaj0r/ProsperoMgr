import { useEffect, useState } from 'react'
import { api } from '../lib/api.js'
import { formatSize, formatSpeed } from './TransferProgress.jsx'
import Checkbox from './Checkbox.jsx'
import { ChevronRightIcon, CloseIcon, LaunchIcon, PauseIcon } from './icons.jsx'
import { useI18n } from '../lib/i18n.jsx'
import { useToast } from '../lib/ToastContext.jsx'
import { POLL_SCALE } from '../lib/usePolling.js'
import { useTransfers } from '../lib/transfers.js'

function hasActiveTasks(tasks) {
  if (tasks?.pkg && ['downloading', 'installing'].includes(tasks.pkg.phase)) return true
  if (tasks?.payload && ['downloading', 'launching'].includes(tasks.payload.phase)) return true
  if (tasks?.save_job?.phase === 'running') return true
  return Boolean(tasks?.autoloader?.running || tasks?.files?.length)
}

// Every job type is listed, zip included. An unlisted type falls back to its own
// name rather than borrowing another verb and mislabelling the job.
const FILE_VERB_KEYS = {
  copy: 'activity.copying',
  move: 'activity.moving',
  zip: 'activity.zipping',
  unzip: 'activity.unzipping',
  delete: 'activity.deleting',
}

function activityLabel(type, task, t) {
  if (type === 'pkg') {
    const verb = t(task.phase === 'installing' ? 'activity.installingPackage' : 'activity.downloadingPackage')
    return task.title ? `${verb}: ${task.title}` : verb
  }
  if (type === 'payload') {
    return t(task.phase === 'launching' ? 'activity.installingPayload' : 'activity.downloadingPayload')
  }
  if (type === 'upload') return t('activity.uploading', { name: task.name })
  if (type === 'saveJob') {
    if (task.kind === 'restore') return t('activity.restoringSave', { name: task.label })
    if (task.kind === 'backup_all') return t('activity.backingUpSaves')
    return t('activity.importingSave', { name: task.label })
  }
  if (type === 'autoloader') {
    return task.remaining_seconds > 0
      ? t('activity.autoloaderStartsIn', { seconds: task.remaining_seconds })
      : t('activity.autoloading', { current: task.current || t('activity.payloadsFallback') })
  }
  if (task.ephemeral) return t('activity.preparingDownload')
  const verbKey = FILE_VERB_KEYS[task.type]
  return `${verbKey ? t(verbKey) : task.type} ${task.source.split('/').pop() || task.source}`
}

const ROW_BUTTON = 'pmgr-icon-btn grid h-7 w-7 shrink-0 place-items-center rounded-lg text-text-dim hover:bg-panel-border hover:text-text disabled:opacity-40'

function RowControls({ type, task, queuedPosition, onChanged, t }) {
  const showToast = useToast()
  const run = (request) => request().then(onChanged).catch((e) => showToast(e.message, 'err'))
  const isFile = type === 'file'
  const canCancel = isFile || (type === 'pkg' && task.phase === 'downloading') || (type === 'upload' && Boolean(task.onCancel))
  if (!canCancel) return null

  return (
    <div className="flex shrink-0 items-center gap-0.5">
      {isFile && task.state === 'queued' && (
        <>
          <button type="button" className={ROW_BUTTON} disabled={queuedPosition.first} aria-label={t('activity.moveUp')} title={t('activity.moveUp')}
            onClick={() => run(() => api.ftp.moveJob(task.id, 'up'))}>
            <span className="-rotate-90"><ChevronRightIcon size={14} /></span>
          </button>
          <button type="button" className={ROW_BUTTON} disabled={queuedPosition.last} aria-label={t('activity.moveDown')} title={t('activity.moveDown')}
            onClick={() => run(() => api.ftp.moveJob(task.id, 'down'))}>
            <span className="rotate-90"><ChevronRightIcon size={14} /></span>
          </button>
        </>
      )}
      <button type="button" className={`${ROW_BUTTON} hover:!text-err`} aria-label={t('activity.cancel')} title={t('activity.cancel')}
        onClick={() => (type === 'upload' ? task.onCancel() : run(() => (isFile ? api.ftp.cancelJob(task.id) : api.pkg.cancel())))}>
        <CloseIcon size={14} />
      </button>
    </div>
  )
}

function ActivityRow({ type, task, onNavigate, onChanged, queuedPosition, t, locale }) {
  const knownTotal = task.total > 0
  const loaded = type === 'file' ? task.completed
    : type === 'autoloader' || type === 'saveJob' ? task.done
    : task.loaded
  const pct = knownTotal ? Math.min(100, (loaded / task.total) * 100) : 0
  const label = activityLabel(type, task, t)

  return (
    <div className="pmgr-row-in rounded-xl p-3 transition-colors hover:bg-panel-2">
      <div className="mb-2 flex items-center justify-between gap-2">
        <button
          type="button"
          onClick={() => type === 'autoloader'
            ? onNavigate('autoloader')
            : onNavigate(type === 'pkg' ? 'pkg' : type === 'payload' ? 'payloads' : type === 'saveJob' ? 'saves' : 'ftp')}
          className="min-w-0 flex-1 rounded-md text-left"
        >
          <div className="truncate text-xs font-bold text-text">{label}</div>
          <div className="mt-0.5 text-xs text-text-dim">
            {type === 'upload'
              ? `${formatSpeed(task.speed, locale)} · ${formatSize(task.loaded, locale)}${knownTotal ? ` / ${formatSize(task.total, locale)}` : ''}`
              : type === 'autoloader'
              ? `${t('activity.autoloaderProgress', { done: task.done, total: task.total })}${task.failed ? t('activity.autoloaderFailed', { failed: task.failed }) : ''}`
              : type === 'file'
              ? task.state === 'queued' ? t('activity.queuedWaiting') : `${formatSize(loaded, locale)}${knownTotal ? ` / ${formatSize(task.total, locale)}` : ''}`
              : type === 'saveJob'
              ? (knownTotal ? t('activity.savesProgress', { done: task.done, total: task.total }) : t('activity.workingInBackground'))
              : type !== 'payload' && task.phase === 'downloading'
                ? `${formatSpeed(task.speed_bps, locale)} · ${formatSize(task.loaded, locale)}${knownTotal ? ` / ${formatSize(task.total, locale)}` : ''}`
                : t('activity.workingInBackground')}
          </div>
        </button>
        <RowControls type={type} task={task} queuedPosition={queuedPosition} onChanged={onChanged} t={t} />
        <span className="pmgr-live-dot shrink-0 text-info" />
      </div>
      <div className="h-1.5 overflow-hidden rounded-full bg-panel-border">
        <div
          role="progressbar"
          aria-label={label}
          aria-valuemin={0}
          aria-valuemax={100}
          aria-valuenow={knownTotal ? Math.round(pct) : undefined}
          className={`h-full rounded-full bg-info ${knownTotal && (type === 'file' || type === 'upload' || type === 'autoloader' || type === 'saveJob' || task.phase === 'downloading') ? 'transition-[width] duration-300' : 'pmgr-indeterminate'}`}
          style={knownTotal && (type === 'file' || type === 'upload' || type === 'autoloader' || type === 'saveJob' || task.phase === 'downloading') ? { width: `${pct}%` } : undefined}
        />
      </div>
    </div>
  )
}

export default function GlobalActivity({ connection, onNavigate }) {
  const { t, locale } = useI18n()
  const showToast = useToast()
  const { transfers } = useTransfers()
  const [tasks, setTasks] = useState(null)
  const refresh = () => api.tasks().then(setTasks).catch(() => {})

  useEffect(() => {
    if (connection === 'offline') return undefined
    let cancelled = false
    let timer = null
    const schedule = (delay) => {
      if (!cancelled) timer = window.setTimeout(poll, delay)
    }
    const poll = () => {
      if (document.visibilityState === 'hidden') {
        schedule(5000 * POLL_SCALE)
        return
      }
      api.tasks()
        .then((value) => {
          if (cancelled) return
          setTasks(value)
          schedule((hasActiveTasks(value) ? 1000 : 5000) * POLL_SCALE)
        })
        .catch(() => schedule(5000 * POLL_SCALE))
    }
    const onVisible = () => {
      if (document.visibilityState !== 'visible') return
      clearTimeout(timer)
      poll()
    }
    poll()
    document.addEventListener('visibilitychange', onVisible)
    return () => {
      cancelled = true
      clearTimeout(timer)
      document.removeEventListener('visibilitychange', onVisible)
    }
  }, [connection])

  const active = []
  if (tasks?.pkg && ['downloading', 'installing'].includes(tasks.pkg.phase)) {
    active.push(['pkg', tasks.pkg])
  }
  if (tasks?.payload && ['downloading', 'launching'].includes(tasks.payload.phase)) {
    active.push(['payload', tasks.payload])
  }
  if (tasks?.save_job?.phase === 'running') active.push(['saveJob', tasks.save_job])
  if (tasks?.autoloader?.running) active.push(['autoloader', tasks.autoloader])
  for (const job of tasks?.files || []) active.push(['file', job])
  for (const transfer of transfers) {
    if (transfer.direction === 'upload' && !transfer.done) active.push(['upload', transfer])
  }
  if (active.length === 0) return null

  const queuedIds = (tasks?.files || []).filter((job) => job.state === 'queued').map((job) => job.id)
  const hasQueuedFiles = queuedIds.length > 0
  const shutdownArmed = tasks?.power_schedule?.active && tasks.power_schedule.mode === 'idle' && tasks.power_schedule.action === 'shutdown'

  const toggleShutdown = (checked) => {
    const request = checked ? api.power.setSchedule({ action: 'shutdown', mode: 'idle' }) : api.power.cancelSchedule()
    request.then(() => {
      showToast(t(checked ? 'activity.shutdownArmed' : 'activity.shutdownDisarmed'))
      refresh()
    }).catch((e) => showToast(e.message, 'err'))
  }

  return (
    <aside className="pmgr-global-activity pmgr-modal-pop fixed z-30 w-[min(380px,calc(100vw-24px))] overflow-hidden rounded-2xl border border-info/25 bg-panel/95 p-1.5 shadow-2xl backdrop-blur-xl">
      <div className="px-3 pb-1 pt-2 text-xs font-extrabold uppercase tracking-[0.18em] text-info">
        {t('activity.title')}
      </div>
      {active.map(([type, task]) => (
        <ActivityRow
          key={type === 'file' || type === 'upload' ? `${type}-${task.id}` : type}
          type={type}
          task={task}
          onNavigate={onNavigate}
          onChanged={refresh}
          queuedPosition={{ first: queuedIds[0] === task.id, last: queuedIds[queuedIds.length - 1] === task.id }}
          t={t}
          locale={locale}
        />
      ))}
      <div className="mt-1 flex items-center justify-between gap-2 border-t border-panel-border px-1 pt-1">
        <div className="flex min-w-0 items-center text-xs font-semibold text-text-dim">
          <Checkbox checked={Boolean(shutdownArmed)} onChange={toggleShutdown} label={t('activity.shutdownWhenDone')} />
          <span className="truncate">{t('activity.shutdownWhenDone')}</span>
        </div>
        {hasQueuedFiles && (
          <button
            type="button"
            onClick={() => api.ftp.pauseJobs(!tasks?.files_paused).then(refresh).catch((e) => showToast(e.message, 'err'))}
            className="pmgr-icon-btn inline-flex shrink-0 items-center gap-1.5 rounded-lg px-2 py-1.5 text-xs font-semibold text-text-dim hover:bg-panel-border hover:text-text"
          >
            {tasks?.files_paused ? <LaunchIcon size={13} /> : <PauseIcon size={13} />}
            {t(tasks?.files_paused ? 'activity.resumeQueue' : 'activity.pauseQueue')}
          </button>
        )}
      </div>
    </aside>
  )
}
