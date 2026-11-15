import { useEffect, useRef, useState } from 'react'
import Panel from '../components/Panel.jsx'
import DropZone from '../components/DropZone.jsx'
import Button from '../components/Button.jsx'
import PathPickerModal from '../components/PathPickerModal.jsx'
import { ConfirmDialog } from '../components/Dialog.jsx'
import { CheckCircleIcon, ChevronRightIcon, ClockIcon, CloseIcon, DeleteIcon, FileIcon, FolderIcon, InstallIcon, RefreshIcon, UploadIcon, XCircleIcon } from '../components/icons.jsx'
import Pill from '../components/Pill.jsx'
import JobCard, { KIND_KEYS, KIND_TONES } from '../components/JobCard.jsx'
import { formatSize, formatSpeed, formatEta } from '../components/TransferProgress.jsx'
import { api } from '../lib/api.js'
import { waitForDirectUploadIdle, watchDirectUploadActive } from '../lib/pkgDirectUpload.js'
import { readPkgInfo } from '../lib/pkgInfo.js'
import { formatDate } from '../lib/format.js'
import { isPS5 } from '../lib/effects.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useHistoryState } from '../lib/useHistoryState.js'
import { useI18n } from '../lib/i18n.jsx'
import { localizeError } from '../lib/errors.js'

const ON_PS5 = isPS5()
const METHODS = [
  { id: 'device', labelKey: 'pkg.methodDeviceLabel', descKey: 'pkg.methodDeviceDesc', icon: <UploadIcon size={18} /> },
  { id: 'url', labelKey: 'pkg.methodUrlLabel', descKey: 'pkg.methodUrlDesc', icon: <InstallIcon size={18} /> },
  { id: 'console', labelKey: 'pkg.methodConsoleLabel', descKey: 'pkg.methodConsoleDesc', icon: <FolderIcon size={18} /> },
]
// In the PS5's own browser there is no PC file to send, so its storage comes first.
const METHOD_ORDER = ON_PS5 ? [METHODS[2], METHODS[1], METHODS[0]] : METHODS

// Queue-row phase -> label. "finishing" is the PS5 still reading the package after
// "queued", not a stall — see pkg_live_cache.hpp's done callback.
const PHASE_LABEL_KEYS = { uploading: 'pkg.stepUploading', validating: 'pkg.stepValidating', finishing: 'pkg.stepFinishing', waitingBase: 'pkg.stepWaitingBase' }

// Pause between queued installs, so the next one does not start while the console is still busy.
const SETTLE_MS = 5000
// An update or DLC waits for its base game to finish installing on the console; the console is asked this often.
const BASE_POLL_MS = 3000
const BASE_WAIT_MAX_MS = 90 * 60 * 1000
const BASE_SILENT_POLLS = 20 // about a minute without an answer from the installer
const HISTORY_KEY = 'pmgr-pkg-history'
const HISTORY_MAX = 8

function loadHistory() {
  try {
    const saved = JSON.parse(localStorage.getItem(HISTORY_KEY))
    return Array.isArray(saved) ? saved : []
  } catch {
    return []
  }
}

// Dotted versions compare part by part as numbers.
function versionAtLeast(have, want) {
  const a = have.split('.').map(Number)
  const b = want.split('.').map(Number)
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    const x = a[i] || 0
    const y = b[i] || 0
    if (x !== y) return x > y
  }
  return true
}

// What the console already holds, as a warning key for the review card (null when nothing is wrong).
function installedWarning(info, found) {
  if (info.category === 'base') return found.installed ? { key: 'pkg.warnAlreadyInstalled' } : null
  if (info.category !== 'update') return null
  if (!found.installed) return { key: 'pkg.warnBaseMissing' }
  if (found.version && info.version && versionAtLeast(found.version, info.version)) {
    return { key: 'pkg.warnNotNewer', version: found.version }
  }
  return null
}

const KIND_ORDER = { base: 0, update: 1, dlc: 2 }

// Groups the staged packages by game and puts each group in install order: base, updates (oldest first), then DLC.
function groupStage(items) {
  const groups = []
  for (const item of items) {
    const key = item.info?.titleId || item.id
    let group = groups.find((g) => g.key === key)
    if (!group) {
      group = { key, items: [] }
      groups.push(group)
    }
    group.items.push(item)
  }
  const rank = (item) => KIND_ORDER[item.info?.category] ?? 3
  for (const group of groups) {
    group.items.sort((a, b) => {
      if (rank(a) !== rank(b)) return rank(a) - rank(b)
      const va = a.info?.version
      const vb = b.info?.version
      if (!va || !vb || va === vb) return 0
      return versionAtLeast(va, vb) ? 1 : -1
    })
    const lead = group.items.find((it) => it.info?.titleName) || group.items[0]
    group.name = lead.info?.titleName || lead.name
    group.titleId = lead.info?.titleId || ''
    group.icons = [...group.items.map((it) => it.info?.icon).filter(Boolean), group.titleId && api.titles.iconUrl(group.titleId)].filter(Boolean)
  }
  return groups
}

// Cover art with fallbacks: the package's own icon first, then the installed game's.
function Cover({ sources, small = false, large = false }) {
  const [failed, setFailed] = useState(0)
  const src = sources[failed]
  return (
    <span className={`grid shrink-0 place-items-center overflow-hidden ${large ? 'rounded-xl' : 'rounded-lg'} bg-panel-2 text-text-dim ${small ? 'h-9 w-9' : large ? 'h-16 w-16' : 'h-12 w-12'}`}>
      {src ? <img src={src} alt="" className="h-full w-full object-cover" onError={() => setFailed((n) => n + 1)} /> : <FileIcon size={18} />}
    </span>
  )
}

function releaseIcons(items) {
  items.forEach((it) => { if (it.info?.icon?.startsWith('blob:')) URL.revokeObjectURL(it.info.icon) })
}

// Review step: what each package is and whether internal storage can hold them, before anything starts.
function StageCard({ groups, volumes, busy, onRemove, onClear, onStart, t, locale }) {
  const items = groups.flatMap((g) => g.items)
  const internal = volumes.find((v) => v.kind === 'internal') || volumes[0]
  const needed = items.reduce((sum, it) => sum + (it.size || 0), 0)
  const free = internal?.free ?? 0
  const short = internal ? needed - free : 0
  const usedPct = internal?.total > 0 ? ((internal.total - internal.free) / internal.total) * 100 : 0
  const neededPct = internal?.total > 0 ? Math.min(100 - usedPct, (needed / internal.total) * 100) : 0

  const multi = groups.some((g) => g.items.length > 1)

  return (
    <div className="mb-4 rounded-2xl border border-accent/25 bg-accent/5 p-4">
      <div className="mb-3 flex flex-wrap items-center justify-between gap-2">
        <div>
          <div className="text-base font-bold text-text">{t('pkg.reviewTitle')} · {items.length}</div>
          {multi && <p className="mt-0.5 text-xs text-text-dim">{t('pkg.orderHint')}</p>}
        </div>
        <Button size="sm" tone="ghost" onClick={onClear} disabled={busy}>{t('pkg.clearList')}</Button>
      </div>

      <div className="flex flex-col gap-3">
        {groups.map((group) => {
          const groupSize = group.items.reduce((sum, it) => sum + (it.size || 0), 0)
          // A "base game missing" warning does not apply when the base game is in this same list.
          const baseInList = group.items.some((it) => it.info?.category === 'base')
          return (
            <div key={group.key} className="pmgr-row-in rounded-xl border border-panel-border bg-panel p-3">
              <div className="flex items-center">
                <span className="mr-4"><Cover key={group.icons.join('|')} sources={group.icons} large /></span>
                <div className="min-w-0 flex-1">
                  <div className="line-clamp-2 text-base font-bold leading-tight text-text" dir="ltr">{group.name}</div>
                  <div className="mt-1.5 flex flex-wrap items-center text-xs text-text-dim">
                    {group.titleId && <span className="pmgr-mono mr-3 rounded-md bg-panel-2 px-2 py-0.5 font-semibold text-text" dir="ltr">{group.titleId}</span>}
                    <span className="mr-3">{t('pkg.packagesCount', { count: group.items.length })}</span>
                    {groupSize > 0 && <span className="pmgr-num">{formatSize(groupSize, locale)}</span>}
                  </div>
                </div>
              </div>
              <div className="mt-3 flex flex-col border-t border-panel-border pt-2">
                {group.items.map((item, position) => {
                  const info = item.info
                  return (
                    <div key={item.id} className="flex items-start py-1.5">
                      {group.items.length > 1 && (
                        <span className="mr-3 mt-0.5 grid h-5 w-5 shrink-0 place-items-center rounded-full bg-panel-2 text-[11px] font-bold text-text-dim">{position + 1}</span>
                      )}
                      <div className="min-w-0 flex-1">
                        <div className="flex flex-wrap items-center">
                          <span className="mr-3"><Pill tone={KIND_TONES[info?.category] || 'neutral'}>{info?.category ? t(KIND_KEYS[info.category]) : t('pkg.kindUnknown')}</Pill></span>
                          {!info && <span className="mr-3 text-xs text-text-dim">{t('pkg.reading')}</span>}
                          {info?.version && <span className="pmgr-mono mr-3 text-sm font-semibold text-text" dir="ltr">{t('pkg.versionLabel', { version: info.version })}</span>}
                          {item.size > 0 && <span className="pmgr-num text-sm text-text-dim">{formatSize(item.size, locale)}</span>}
                        </div>
                        <div className="pmgr-mono mt-1 truncate text-xs text-text-dim" dir="ltr" title={item.name}>{item.name}</div>
                        {item.warning && !(baseInList && item.warning.key === 'pkg.warnBaseMissing') && (
                          <div className="mt-1.5 rounded-lg bg-warn/10 px-2.5 py-1.5 text-xs font-semibold text-warn">{t(item.warning.key, { version: item.warning.version })}</div>
                        )}
                      </div>
                      <Button size="icon" tone="danger" title={t('pkg.remove')} onClick={() => onRemove(item.id)}>
                        <CloseIcon size={14} />
                      </Button>
                    </div>
                  )
                })}
              </div>
            </div>
          )
        })}
      </div>

      {internal && needed > 0 && (
        <div className="mt-3 rounded-xl border border-panel-border bg-bg/40 p-3">
          <div className="mb-2 flex flex-wrap items-center justify-between gap-2 text-xs">
            <span className="text-sm font-bold text-text">{t('pkg.spaceTitle')}</span>
            <span className="text-text-dim">{t('pkg.spaceFree', { size: formatSize(free, locale) })}</span>
          </div>
          <div className="flex h-2.5 overflow-hidden rounded-full bg-panel-border">
            <div className="h-full bg-text-dim/50" style={{ width: `${usedPct}%` }} />
            <div className={`h-full ${short > 0 ? 'bg-err' : 'bg-accent'}`} style={{ width: `${neededPct}%`, minWidth: 4 }} />
          </div>
          <div className="mt-2 flex flex-wrap items-center justify-between text-xs">
            <span className="font-semibold text-text">{t('pkg.spaceNeeded', { size: formatSize(needed, locale) })}</span>
            <span className={short > 0 ? 'font-semibold text-err' : 'text-text-dim'}>
              {short > 0 ? t('pkg.spaceShort', { size: formatSize(short, locale) }) : t('pkg.spaceAfter', { size: formatSize(free - needed, locale) })}
            </span>
          </div>
        </div>
      )}

      <div className="mt-3 flex justify-end">
        <Button tone={short > 0 ? 'warn' : 'primary'} sheen icon={<InstallIcon size={15} />} onClick={onStart} disabled={busy || items.length === 0}>
          {items.length > 1 ? t('pkg.installAll') : t('pkg.install')}
        </Button>
      </div>
    </div>
  )
}

// Stays mounted while another page is open, so only the visible page may take dropped files.
export default function PkgView({ visible = true }) {
  const { t, locale } = useI18n()
  const [method, setMethodStep] = useHistoryState('pkgMethod', ON_PS5 ? 'console' : 'device', '#/pkg')
  const setMethod = (next, replace = false) => { if (next !== method) setMethodStep(next, replace) }
  const [url, setUrl] = useState('')
  const [volumes, setVolumes] = useState([])
  const [pathPickerOpen, setPathPickerOpen] = useState(false)
  const [pickerDir, setPickerDir] = useState('') // the folder the picker opens in, the last one used
  const [busy, setBusy] = useState(false)
  const [uploadJob, setUploadJob] = useState(null) // browser -> backend leg, upload flow only
  const [job, setJob] = useState(null) // backend job status (download/install)
  const [result, setResult] = useState(null)
  const [confirmDeleteDownload, setConfirmDeleteDownload] = useState(false)
  const [directUploadActive, setDirectUploadActive] = useState(false)
  const [history, setHistory] = useState(loadHistory)
  const [stage, setStage] = useState([]) // packages picked but not confirmed yet
  const [queue, setQueue] = useState([]) // multi-file installs — [{id, name, kind, file|path, status, error}]
  const pollRef = useRef(null)
  const canceledRef = useRef(false)
  const fileInputRef = useRef(null)
  const uploadSampleRef = useRef(null)
  const showToast = useToast()

  useEffect(() => {
    api.ftp.storage().then((r) => setVolumes(r.volumes || [])).catch(() => {})
    return () => clearInterval(pollRef.current)
  }, [])

  // The PS5 keeps reading the package over this connection well past "queued" — stays
  // true until the backend itself closes it, regardless of what the job status shows.
  useEffect(() => watchDirectUploadActive(setDirectUploadActive), [])

  // Resolves once the backend job reaches a terminal state — the shared tail end of
  // every install, single or queued.
  const waitForJobTerminal = () =>
    new Promise((resolve) => {
      pollRef.current = setInterval(() => {
        api.pkg
          .status()
          .then((s) => {
            setJob(s)
            if (s.phase === 'queued' || s.phase === 'error') {
              clearInterval(pollRef.current)
              resolve(s)
            }
          })
          .catch(() => {})
      }, 500)
    })

  const cancelInstall = () => {
    canceledRef.current = true
    api.pkg.cancel().catch(() => {})
  }

  const reportJobResult = (s) => {
    if (s.phase === 'queued') {
      setResult({ ok: true, content_id: s.content_id, title: s.title, hasStagedFile: s.has_staged_file })
      showToast(t('pkg.installQueuedToast', { contentId: s.title || s.content_id || t('pkg.packageAcceptedByPs5') }))
    } else {
      const error = s.error === 'canceled' ? t('pkg.canceled') : localizeError(s.error)
      setResult({ ok: false, error })
      showToast(error, 'err')
    }
  }

  const deleteDownloadedFile = () =>
    api.pkg.deleteDownload().then(() => {
      setResult((r) => (r ? { ...r, hasStagedFile: false } : r))
      showToast(t('pkg.deleteDownloadDone'))
    })

  const install = (opts) => {
    canceledRef.current = false
    setBusy(true)
    setResult(null)
    setConfirmDeleteDownload(false)
    setJob(null)
    setQueue([])
    api.pkg
      .install(opts)
      .then(() => waitForJobTerminal())
      .then((s) => {
        setBusy(false)
        reportJobResult(s)
      })
      .catch((e) => {
        setBusy(false)
        setResult({ ok: false, error: e.message })
        showToast(e.message, 'err')
      })
  }

  const recordHistory = (item, ok, error) => {
    const entry = {
      id: `${Date.now()}-${item.id}`,
      title: item.info?.titleName || item.name,
      category: item.info?.category || '',
      size: item.size || 0,
      ok,
      error: ok ? '' : error,
      at: Date.now(),
    }
    setHistory((current) => {
      const next = [entry, ...current].slice(0, HISTORY_MAX)
      try { localStorage.setItem(HISTORY_KEY, JSON.stringify(next)) } catch { /* history just won't persist */ }
      return next
    })
  }

  const clearHistory = () => {
    setHistory([])
    try { localStorage.removeItem(HISTORY_KEY) } catch { /* nothing stored to clear */ }
  }

  const updateQueueItem = (id, patch) => setQueue((q) => q.map((it) => (it.id === id ? { ...it, ...patch } : it)))

  // Resolves 'done' once the console reports the base game installed, 'failed' if it errors or never answers.
  const waitForBaseInstalled = async (contentId, titleId) => {
    const started = Date.now()
    let silent = 0
    while (!canceledRef.current && Date.now() - started < BASE_WAIT_MAX_MS) {
      try {
        const s = await api.pkg.installStatus(contentId)
        if (s.failed) return 'failed'
        if (s.finished) return 'done'
        silent = s.known ? 0 : silent + 1
      } catch {
        silent += 1
      }
      if (silent >= BASE_SILENT_POLLS) {
        // The installer is not answering, so go by whether the game is on the console.
        const found = await api.pkg.installed(titleId).catch(() => null)
        return found?.installed ? 'done' : 'failed'
      }
      await new Promise((resolve) => window.setTimeout(resolve, BASE_POLL_MS))
    }
    return canceledRef.current ? 'canceled' : 'failed'
  }

  // Installs each queued item in order — the backend only ever runs one install job at
  // a time anyway, so this just waits for each to fully finish before starting the next.
  const runQueue = async (items) => {
    canceledRef.current = false
    const outcomes = new Map() // item id -> 'done' | 'error', so an update can tell if its base made it
    for (const [index, item] of items.entries()) {
      // The console is still finalizing the previous package right after it reports done.
      if (index > 0) await new Promise((resolve) => window.setTimeout(resolve, SETTLE_MS))
      if (canceledRef.current) {
        updateQueueItem(item.id, { status: 'error', error: t('pkg.canceled') })
        continue
      }
      // Updates and DLC go in after their base game has really finished installing, and are skipped if it did not.
      const category = item.info?.category
      const titleId = item.info?.titleId
      if ((category === 'update' || category === 'dlc') && titleId) {
        const base = items.slice(0, index).find((it) => it.info?.titleId === titleId && it.info?.category === 'base')
        let skipped = ''
        if (base) {
          if (outcomes.get(base.id) !== 'done') {
            skipped = t('pkg.skippedBaseFailed')
          } else {
            updateQueueItem(item.id, { status: 'active', phase: 'waitingBase' })
            const waited = await waitForBaseInstalled(base.info.contentId, titleId)
            if (waited === 'canceled') skipped = t('pkg.canceled')
            else if (waited !== 'done') skipped = t('pkg.skippedBaseFailed')
          }
        } else if (category === 'update') {
          const found = await api.pkg.installed(titleId).catch(() => null)
          if (found && !found.installed) skipped = t('pkg.skippedBaseMissing')
        }
        if (skipped) {
          outcomes.set(item.id, 'error')
          updateQueueItem(item.id, { status: 'error', error: skipped })
          continue
        }
      }
      updateQueueItem(item.id, { status: 'active', phase: item.kind === 'device' ? 'uploading' : 'validating' })
      setResult(null)
      setJob(null)
      try {
        if (item.kind === 'device') {
          const now = performance.now()
          uploadSampleRef.current = { time: now, loaded: 0, speed: 0 }
          setUploadJob({ name: item.name, loaded: 0, total: item.file.size, speed: 0 })
          await api.pkg.cover(item.info?.icon, item.info?.debug)
          await api.pkg.upload(item.file, (loaded, total) => {
            const sample = uploadSampleRef.current
            const sampleNow = performance.now()
            const elapsed = (sampleNow - sample.time) / 1000
            let speed = sample.speed
            if (elapsed >= 0.15 && loaded >= sample.loaded) {
              const instant = (loaded - sample.loaded) / elapsed
              speed = sample.speed > 0 ? sample.speed * 0.72 + instant * 0.28 : instant
              uploadSampleRef.current = { time: sampleNow, loaded, speed }
            }
            setUploadJob({ name: item.name, loaded, total: total || item.file.size, speed })
          }, item.info?.titleName)
          setUploadJob(null)
          updateQueueItem(item.id, { phase: 'validating' })
        } else {
          await api.pkg.install({ path: item.path })
        }
        const status = await waitForJobTerminal()
        // Wait for the socket to close (success or error) before starting the next
        // item — a live session still winding down would reject a new one as busy.
        if (item.kind === 'device') {
          updateQueueItem(item.id, { phase: 'finishing' })
          await waitForDirectUploadIdle()
        }
        if (status.phase === 'error') throw new Error(localizeError(status.error))
        outcomes.set(item.id, 'done')
        updateQueueItem(item.id, { status: 'done' })
        recordHistory(item, true)
        reportJobResult(status)
      } catch (e) {
        // A console that rejects the package closes the upload socket; its own message says why.
        const failed = e.message === 'canceled' ? null : await api.pkg.status().catch(() => null)
        const error = e.message === 'canceled' ? t('pkg.canceled') : failed?.phase === 'error' && failed.error ? localizeError(failed.error) : e.message
        outcomes.set(item.id, 'error')
        updateQueueItem(item.id, { status: 'error', error })
        recordHistory(item, false, error)
        showToast(error, 'err')
      }
    }
    setBusy(false)
    setUploadJob(null)
    setJob(null)
  }

  const updateStageItem = (id, patch) => setStage((items) => items.map((it) => (it.id === id ? { ...it, ...patch } : it)))

  // Asks the console whether the game is already there, so a refusal is explained before the upload.
  const checkInstalled = (id, info) => {
    if (!info?.titleId || info.category === 'dlc') return
    api.pkg.installed(info.titleId)
      .then((r) => updateStageItem(id, { warning: installedWarning(info, r) }))
      .catch(() => {})
  }

  const stageFiles = (files) => {
    if (busy) {
      showToast(t('pkg.waitForCurrentJob'), 'err')
      return
    }
    const pkgFiles = files.filter((f) => f.name.toLowerCase().endsWith('.pkg'))
    if (pkgFiles.length === 0) {
      showToast(t('pkg.onlyPkgFiles'), 'err')
      return
    }
    if (pkgFiles.length < files.length) showToast(t('pkg.someFilesSkipped', { count: files.length - pkgFiles.length }), 'err')
    setMethod('device', true)
    setResult(null)
    releaseIcons(queue)
    setQueue([])
    const items = pkgFiles.map((file, i) => ({ id: `d${Date.now()}-${i}`, name: file.name, kind: 'device', file, size: file.size, info: null }))
    setStage((current) => [...current, ...items])
    items.forEach((item) => readPkgInfo(item.file).then((info) => {
      updateStageItem(item.id, { info })
      checkInstalled(item.id, info)
    }))
  }

  const stageConsolePaths = (paths) => {
    setPathPickerOpen(false)
    if (busy) {
      showToast(t('pkg.waitForCurrentJob'), 'err')
      return
    }
    setResult(null)
    releaseIcons(queue)
    setQueue([])
    const items = paths.map((p, i) => ({ id: `c${Date.now()}-${i}`, name: p.split('/').pop(), kind: 'console', path: p, size: 0, info: null }))
    setStage((current) => [...current, ...items])
    items.forEach((item) => api.pkg.info(item.path)
      .then((r) => {
        const info = { contentId: r.content_id, titleId: r.title_id, titleName: r.title_name, version: r.version, category: r.category, icon: api.pkg.iconUrl(item.path) }
        updateStageItem(item.id, { size: r.size, info })
        checkInstalled(item.id, info)
      })
      .catch(() => updateStageItem(item.id, { info: {} })))
  }

  const startStaged = () => {
    const items = groupStage(stage).flatMap((g) => g.items).map((it) => ({ ...it, status: 'pending' }))
    setStage([])
    setBusy(true)
    setResult(null)
    setConfirmDeleteDownload(false)
    setJob(null)
    setQueue(items)
    runQueue(items)
  }

  const retryItem = (item) => {
    if (busy) {
      showToast(t('pkg.waitForCurrentJob'), 'err')
      return
    }
    setBusy(true)
    setResult(null)
    updateQueueItem(item.id, { status: 'pending', error: '' })
    runQueue([{ ...item, status: 'pending' }])
  }

  const onChoosePkg = (event) => {
    const files = Array.from(event.target.files || [])
    event.target.value = ''
    if (files.length) stageFiles(files)
  }

  const jobPct = job && job.total > 0 ? Math.min(100, (job.loaded / job.total) * 100) : 0
  const jobEta = job && job.speed_bps > 0 ? (job.total - job.loaded) / job.speed_bps : Infinity

  // The device method has its own upload progress; URL and console share the
  // backend's job status once queued.
  const steps = { uploading: t('pkg.stepUploading'), downloading: t('pkg.stepDownloading'), validating: t('pkg.stepValidating'), queued: t('pkg.stepQueued') }

  const finishedCount = queue.filter((it) => it.status === 'done' || it.status === 'error').length
  const activeItem = queue.find((it) => it.status === 'active')
  const stepList = [method === 'url' ? steps.downloading : steps.uploading, steps.validating, steps.queued]
  const heroBase = { item: activeItem, t, onCancel: cancelInstall, cancelLabel: t('pkg.cancel') }

  let hero = null
  if (uploadJob) {
    const pct = uploadJob.total > 0 ? Math.min(100, (uploadJob.loaded / uploadJob.total) * 100) : 0
    const eta = uploadJob.speed > 0 ? (uploadJob.total - uploadJob.loaded) / uploadJob.speed : Infinity
    hero = (
      <JobCard
        {...heroBase}
        steps={[steps.uploading, steps.validating, steps.queued]}
        activeIndex={0}
        label={t('pkg.uploadingName', { name: uploadJob.name })}
        pct={pct}
        stats={[
          { label: t('pkg.statSpeed'), value: formatSpeed(uploadJob.speed, locale) },
          { label: t('pkg.statSent'), value: `${formatSize(uploadJob.loaded, locale)} / ${formatSize(uploadJob.total, locale)}` },
          { label: t('files.eta'), value: formatEta(eta) },
        ]}
      />
    )
  } else if (job?.phase === 'downloading') {
    hero = (
      <JobCard
        {...heroBase}
        steps={[steps.downloading, steps.validating, steps.queued]}
        activeIndex={0}
        label={t('pkg.downloadingPkg')}
        pct={jobPct}
        stats={[
          { label: t('pkg.statSpeed'), value: formatSpeed(job.speed_bps, locale) },
          { label: t('pkg.statSent'), value: `${formatSize(job.loaded, locale)} / ${job.total > 0 ? formatSize(job.total, locale) : '?'}` },
          { label: t('files.eta'), value: formatEta(jobEta) },
        ]}
      />
    )
  } else if (job?.phase === 'installing') {
    hero = (
      <JobCard {...heroBase} steps={stepList} activeIndex={1} label={t('pkg.validatingAndQueuing')} indeterminate sub={t('pkg.validatingSub')} />
    )
  } else if (activeItem?.phase === 'waitingBase') {
    hero = (
      <JobCard {...heroBase} steps={stepList} activeIndex={0} label={t('pkg.stepWaitingBase')} indeterminate sub={t('pkg.waitingBaseSub')} />
    )
  } else if (activeItem?.phase === 'finishing') {
    hero = (
      <JobCard {...heroBase} steps={stepList} activeIndex={2} label={t('pkg.stepFinishing')} indeterminate sub={t('pkg.finishingSub')} onCancel={undefined} />
    )
  }

  return (
    <Panel title={t('pkg.title')}>
      {directUploadActive && (
        <div className="pmgr-notice-in mb-4 rounded-2xl border border-warn/30 bg-warn/10 p-3 text-sm text-warn">
          {t('pkg.deviceStayConnected')}
        </div>
      )}

      {hero}

      {stage.length > 0 && !busy && (
        <StageCard
          groups={groupStage(stage)}
          volumes={volumes}
          busy={busy}
          onRemove={(id) => {
            releaseIcons(stage.filter((it) => it.id === id))
            setStage((items) => items.filter((it) => it.id !== id))
          }}
          onClear={() => {
            releaseIcons(stage)
            setStage([])
          }}
          onStart={startStaged}
          t={t}
          locale={locale}
        />
      )}

      {queue.length > 0 && (
        <div className="mb-4">
          <div className="mb-2 flex items-center justify-between gap-3 px-1 text-xs font-bold uppercase tracking-wide text-text-dim">
            <span>{t('pkg.queueProgress', { done: finishedCount, total: queue.length })}</span>
            <span className="h-1.5 w-28 overflow-hidden rounded-full bg-panel-border">
              <span className="block h-full rounded-full bg-ok transition-[width] duration-300" style={{ width: `${(finishedCount / queue.length) * 100}%` }} />
            </span>
          </div>
          <div className="flex flex-col gap-2">
            {queue.map((item) => {
              const info = item.info
              const tone = { done: 'border-ok/30', error: 'border-err/30', active: 'border-accent/40' }[item.status] || 'border-panel-border'
              return (
                <div key={item.id} className={`pmgr-row-in flex items-center gap-3 rounded-xl border bg-panel p-3 ${tone}`}>
                  <span className={`grid h-9 w-9 shrink-0 place-items-center rounded-lg ${
                    item.status === 'done' ? 'bg-ok/10 text-ok' : item.status === 'error' ? 'bg-err/10 text-err' : item.status === 'active' ? 'bg-accent/10 text-accent' : 'bg-panel-2 text-text-dim'
                  }`}>
                    {item.status === 'done' && <CheckCircleIcon size={18} />}
                    {item.status === 'error' && <XCircleIcon size={18} />}
                    {item.status === 'active' && <span className="h-4 w-4 animate-spin rounded-full border-2 border-accent/30 border-t-accent" />}
                    {item.status === 'pending' && <ClockIcon size={18} />}
                  </span>
                  <Cover small sources={[info?.icon, info?.titleId && api.titles.iconUrl(info.titleId)].filter(Boolean)} />
                  <div className="min-w-0 flex-1">
                    <div className="flex flex-wrap items-center gap-2">
                      {info?.category && <Pill tone={KIND_TONES[info.category] || 'neutral'}>{t(KIND_KEYS[info.category])}</Pill>}
                      <span className="min-w-0 truncate text-sm font-bold text-text" dir="ltr">{info?.titleName || item.name}</span>
                    </div>
                    <div className="mt-0.5 flex flex-wrap gap-x-3 text-xs text-text-dim">
                      {info?.contentId && <span className="pmgr-mono" dir="ltr">{info.contentId}</span>}
                      {item.size > 0 && <span className="pmgr-num">{formatSize(item.size, locale)}</span>}
                    </div>
                    {item.status === 'error' && <div className="mt-1 text-xs text-err">{item.error}</div>}
                  </div>
                  <div className="flex shrink-0 items-center gap-2">
                    {item.status === 'pending' && <span className="text-xs text-text-dim">{t('pkg.statusWaiting')}</span>}
                    {item.status === 'active' && item.phase && <span className="text-xs font-semibold text-accent">{t(PHASE_LABEL_KEYS[item.phase])}</span>}
                    {item.status === 'done' && <span className="text-xs font-semibold text-ok">{t('pkg.itemDone')}</span>}
                    {item.status === 'error' && (
                      <Button size="sm" tone="neutral" icon={<RefreshIcon size={13} />} onClick={() => retryItem(item)} disabled={busy}>{t('pkg.retry')}</Button>
                    )}
                  </div>
                </div>
              )
            })}
          </div>
        </div>
      )}

      {result && !hero && queue.length === 0 && (
        result.ok ? (
          <div className="mb-4 flex items-start gap-3 rounded-2xl border border-ok/30 bg-ok/10 p-4">
            <span className="mt-0.5 grid h-9 w-9 shrink-0 place-items-center rounded-xl bg-ok/15 text-ok"><CheckCircleIcon size={18} /></span>
            <div className="min-w-0">
              <div className="font-bold text-text">{t('pkg.acceptedTitle')}</div>
              <div className="mt-0.5 text-sm text-text-dim">{t('pkg.acceptedDesc')}</div>
              {result.title && <div className="mt-1 text-sm text-text">{result.title}</div>}
              {result.content_id && <div className="mt-1 pmgr-mono text-xs text-text-dim" dir="ltr">{t('pkg.contentId', { id: result.content_id })}</div>}
              {result.hasStagedFile && (
                <Button size="sm" tone="ghost" className="mt-2" icon={<DeleteIcon size={13} />} onClick={() => setConfirmDeleteDownload(true)}>
                  {t('pkg.deleteDownloadAction')}
                </Button>
              )}
            </div>
          </div>
        ) : (
          <div className="pmgr-notice-in mb-4 rounded-2xl border border-err/30 bg-err/10 p-4 text-sm text-err">{result.error}</div>
        )
      )}

      {confirmDeleteDownload && (
        <ConfirmDialog
          title={t('pkg.deleteDownloadTitle')}
          description={t('pkg.deleteDownloadDesc')}
          detail={t('pkg.deleteDownloadWarning')}
          confirmLabel={t('common.delete')}
          tone="warn"
          onConfirm={deleteDownloadedFile}
          onClose={() => setConfirmDeleteDownload(false)}
        />
      )}

      <DropZone global enabled={visible} onFiles={stageFiles} label={t('pkg.dropZoneLabel')} className="block">
        <div role="tablist" className="mb-3 inline-flex max-w-full flex-wrap rounded-xl border border-panel-border bg-bg/45 p-1">
          {METHOD_ORDER.map((m) => (
            <button
              key={m.id}
              type="button"
              role="tab"
              aria-selected={method === m.id}
              onClick={() => setMethod(m.id)}
              className={`mr-1 inline-flex min-h-10 items-center rounded-lg px-4 text-sm font-semibold transition-colors last:mr-0 ${
                method === m.id ? 'bg-accent text-white' : 'text-text-dim hover:text-text'
              }`}
            >
              <span className="mr-2 shrink-0">{m.icon}</span>
              {t(m.labelKey)}
            </button>
          ))}
        </div>
        <p className="mb-4 text-sm text-text-dim">{t(METHODS.find((m) => m.id === method).descKey)}</p>

        {method === 'device' && ON_PS5 && (
          <div className="pmgr-notice-in mb-3 rounded-xl border border-warn/40 bg-warn/10 p-3 text-sm text-text">
            <div>{t('pkg.deviceOnPs5')}</div>
            <Button size="sm" tone="primary" className="mt-2" icon={<FolderIcon size={14} />} onClick={() => setMethod('console')}>
              {t('pkg.deviceOnPs5Action')}
            </Button>
          </div>
        )}

        {method === 'device' && (
          <div className="flex flex-col items-stretch rounded-xl border border-dashed border-panel-border bg-bg/40 px-5 py-5 sm:flex-row sm:items-center sm:justify-between">
            <div className="mb-3 min-w-0 flex-1 text-sm font-semibold text-text sm:mb-0 sm:mr-4">{t('pkg.dropHint')}</div>
            <Button tone="primary" icon={<UploadIcon size={15} />} onClick={() => fileInputRef.current?.click()} disabled={busy}>
              {uploadJob ? t('pkg.uploading') : t('pkg.chooseFile')}
            </Button>
            <input ref={fileInputRef} type="file" accept=".pkg" multiple hidden onChange={onChoosePkg} />
          </div>
        )}

        {method === 'url' && (
          <div className="rounded-xl border border-panel-border bg-bg/40 p-4">
            <label className="mb-1.5 block text-xs font-bold uppercase tracking-wide text-text-dim">{t('pkg.urlLabel')}</label>
            <div className="flex flex-col gap-2 sm:flex-row">
              <input
                value={url}
                onChange={(e) => setUrl(e.target.value)}
                placeholder={t('pkg.urlPlaceholder')}
                dir="ltr"
                className="min-h-10 min-w-0 flex-1 rounded-md border border-panel-border bg-bg px-3 py-2 text-sm text-text outline-none focus:border-accent"
              />
              <Button tone="primary" sheen icon={<InstallIcon size={15} />} onClick={() => install({ url: url.trim() })} disabled={busy || !url.trim()}>
                {busy ? t('pkg.installing') : t('pkg.install')}
              </Button>
            </div>
            <p className="mt-1.5 text-xs text-text-dim">{t('pkg.urlHint')}</p>
          </div>
        )}

        {method === 'console' && (
          <div className="rounded-xl border border-panel-border bg-bg/40 p-4">
            <label className="mb-2 block text-xs font-bold uppercase tracking-wide text-text-dim">{t('pkg.consoleLabel')}</label>
            <button
              type="button"
              onClick={() => setPathPickerOpen(true)}
              disabled={busy}
              className="flex w-full items-center rounded-xl border border-dashed border-panel-border bg-bg px-4 py-5 text-left transition-colors hover:border-accent/50 hover:bg-accent/5 disabled:opacity-50"
            >
              <span className="mr-4 grid h-12 w-12 shrink-0 place-items-center rounded-xl bg-accent/10 text-accent"><FolderIcon size={22} /></span>
              <span className="min-w-0 flex-1">
                <span className="block font-bold text-text">{t('pkg.chooseFiles')}</span>
                <span className="mt-0.5 block text-xs text-text-dim">{t('pkg.consoleHint')}</span>
              </span>
              <span className="ml-3 shrink-0 text-accent"><ChevronRightIcon size={18} /></span>
            </button>
          </div>
        )}
      </DropZone>

      {history.length > 0 && (
        <div className="mt-5">
          <div className="mb-2 flex items-center justify-between px-1">
            <span className="text-xs font-bold uppercase tracking-wide text-text-dim">{t('pkg.recentTitle')}</span>
            <Button size="sm" tone="ghost" onClick={clearHistory}>{t('pkg.clearHistory')}</Button>
          </div>
          <div className="flex flex-col gap-1.5">
            {history.map((entry) => (
              <div key={entry.id} className="flex items-center gap-3 rounded-xl border border-panel-border bg-bg/40 px-3 py-2">
                <span className={entry.ok ? 'text-ok' : 'text-err'}>{entry.ok ? <CheckCircleIcon size={16} /> : <XCircleIcon size={16} />}</span>
                <span className="min-w-0 flex-1 truncate text-sm text-text" dir="ltr">{entry.title}</span>
                {entry.category && <Pill tone={KIND_TONES[entry.category] || 'neutral'}>{t(KIND_KEYS[entry.category])}</Pill>}
                {entry.size > 0 && <span className="pmgr-num hidden text-xs text-text-dim sm:inline">{formatSize(entry.size, locale)}</span>}
                <span className="shrink-0 text-xs text-text-dim">{formatDate(new Date(entry.at), locale, { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' })}</span>
              </div>
            ))}
          </div>
        </div>
      )}

      {pathPickerOpen && (
        <PathPickerModal
          title={t('pkg.pathPickerMultiTitle')}
          description={t('pkg.pathPickerMultiDesc')}
          initialPath={pickerDir || '/data'}
          volumes={volumes}
          mode="file"
          multiple
          extensions={['.pkg']}
          actionLabel={t('pkg.pathPickerMultiAction')}
          onClose={() => setPathPickerOpen(false)}
          onSelect={(selected) => {
            setPickerDir(selected[0].replace(/\/[^/]+$/, '') || '/')
            stageConsolePaths(selected)
          }}
        />
      )}
    </Panel>
  )
}
