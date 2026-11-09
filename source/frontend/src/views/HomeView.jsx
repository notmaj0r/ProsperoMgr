import { memo, useCallback, useEffect, useMemo, useState } from 'react'
import Pill from '../components/Pill.jsx'
import PlatformBadge from '../components/PlatformBadge.jsx'
import SkeletonGrid from '../components/Skeleton.jsx'
import Panel from '../components/Panel.jsx'
import SearchField from '../components/SearchField.jsx'
import SegmentedControl from '../components/SegmentedControl.jsx'
import EmptyState from '../components/EmptyState.jsx'
import Button from '../components/Button.jsx'
import { ConfirmDialog } from '../components/Dialog.jsx'
import { ICONS } from '../components/NavRail.jsx'
import QrCodeDialog from '../components/QrCodeDialog.jsx'
import { ChevronRightIcon, ClockIcon, CloseIcon, DeleteIcon, PowerIcon, QrIcon, RefreshIcon } from '../components/icons.jsx'
import { api } from '../lib/api.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'
import { setFilesPath } from '../lib/filesPath.js'
import { formatDate, formatFileSize, volumeLabel, volumeBarClass } from '../lib/format.js'
import { useDebouncedValue } from '../lib/useDebouncedValue.js'
import { usePolling } from '../lib/usePolling.js'
import { isPS5 } from '../lib/effects.js'

// Word order/punctuation around a name differs by language, so the caller
// picks a plain vs. a "WithName" key instead of concatenating "greeting, name".
function greetingKey(userName) {
  const h = new Date().getHours()
  const period = h < 5 ? 'night' : h < 12 ? 'morning' : h < 18 ? 'afternoon' : 'evening'
  return userName ? `home.greeting.${period}WithName` : `home.greeting.${period}`
}

const QUICK_ACTIONS = [
  { id: 'ftp', labelKey: 'home.quickBrowseFiles', descKey: 'home.quickBrowseFilesDesc' },
  { id: 'pkg', labelKey: 'home.quickInstallPkg', descKey: 'home.quickInstallPkgDesc' },
  { id: 'payloads', labelKey: 'home.quickRunPayload', descKey: 'home.quickRunPayloadDesc' },
  { id: 'autoloader', labelKey: 'home.autoloader', descKey: 'home.quickAutoloaderDesc', icon: <PowerIcon size={18} /> },
  { id: 'saves', labelKey: 'home.quickBackUpSaves', descKey: 'home.quickBackUpSavesDesc' },
]

// app.db stores play time in seconds. The cover chip uses the short form ("33 min", "5 h 20 min");
// the tooltip uses the long {key, count} form, which goes through the plural-aware strings.
function formatPlaytime(seconds) {
  if (!seconds) return null
  const minutes = Math.floor(seconds / 60)
  const h = Math.floor(minutes / 60)
  const long = h >= 1 ? { key: 'home.playedHours', count: h } : { key: 'home.playedMinutes', count: Math.max(1, minutes) }
  if (minutes < 1) return { ...long, short: { key: 'home.playtimeUnderMinute' } }
  if (h < 1) return { ...long, short: { key: 'home.playtimeMinutes', count: minutes } }
  if (minutes % 60 === 0) return { ...long, short: { key: 'home.playtimeHours', hours: h } }
  return { ...long, short: { key: 'home.playtimeHoursMinutes', hours: h, minutes: minutes % 60 } }
}

function formatLastPlayed(raw, locale) {
  if (!raw) return null
  const d = new Date(raw)
  if (isNaN(d.getTime())) return raw
  return formatDate(d, locale, { month: 'short', day: 'numeric', year: 'numeric' })
}

function formatUptime(seconds) {
  if (!Number.isFinite(seconds)) return '—'
  const days = Math.floor(seconds / 86400)
  const hours = Math.floor((seconds % 86400) / 3600)
  const minutes = Math.floor((seconds % 3600) / 60)
  if (days) return `${days}d ${hours}h`
  if (hours) return `${hours}h ${minutes}m`
  return `${minutes}m`
}

// Opened from the console itself, the address is useless on a phone.
// Same reason as the file list: render the library in pages on the console's slow browser.
const TILE_PAGE = isPS5() ? 24 : 60
// As many columns as fit, so the covers stay large and the rows stay full.
const GAMES_GRID = { gridTemplateColumns: `repeat(auto-fill, minmax(${isPS5() ? 250 : 220}px, 1fr))` }
const isLoopbackHost = ['localhost', '127.0.0.1', '[::1]'].includes(location.hostname)

function stemOf(name) {
  return name.replace(/\.(elf|bin|self)$/i, '').toLowerCase()
}

function PanelTile({ label, value, sub, bar, barClass, onClick, valueClass = 'text-text' }) {
  const body = (
    <>
      <div className="text-xs font-bold uppercase tracking-wide text-text-dim">{label}</div>
      <div className={`pmgr-num mt-1 truncate text-lg font-bold ${valueClass}`}>{value}</div>
      {bar != null && (
        <div className="mt-1.5 h-1.5 overflow-hidden rounded-full bg-panel-border">
          <div className={`h-full rounded-full transition-[width] duration-300 ${barClass}`} style={{ width: `${bar}%` }} />
        </div>
      )}
      {sub && <div className="mt-1 truncate text-xs text-text-dim">{sub}</div>}
    </>
  )
  const classes = 'pmgr-tile flex flex-col items-stretch justify-start rounded-xl border border-panel-border bg-panel p-3 text-left'
  return onClick ? <button type="button" onClick={onClick} className={classes}>{body}</button> : <div className={classes}>{body}</div>
}

function ConsolePanel({ overview, status, system, payloadsRunning, onNavigate, t, locale }) {
  const thermal = overview.thermal
  const ramUsed = system?.ram_total > 0 ? Math.round(((system.ram_total - system.ram_free) / system.ram_total) * 100) : null
  const volumes = (system?.storage || []).filter((v) => v.total > 0 && (v.kind === 'internal' || v.kind === 'ext_m2'))
  const autoloader = overview.autoloader
  // Even rows beat a ragged last one: pick 3 or 4 columns by how many tiles there are.
  const tileCount = 4 + (system ? (ramUsed !== null ? 1 : 0) + volumes.length : 3)
  const columns = tileCount <= 4 ? tileCount : tileCount % 3 === 0 ? 3 : 4

  return (
    <section aria-label={t('home.consoleTitle')} className="pmgr-console-grid" style={{ '--cols': columns }}>
      <PanelTile
        label={t('home.thermals')}
        value={thermal?.ok ? `${thermal.hottest_c}°C` : '—'}
        valueClass={thermal?.ok && thermal.hottest_c >= thermal.hot_c ? 'text-err' : thermal?.ok && thermal.hottest_c >= thermal.warm_c ? 'text-warn' : 'text-text'}
        bar={thermal?.ok ? Math.max(0, Math.min(100, ((thermal.hottest_c - thermal.min_c) / (thermal.max_c - thermal.min_c)) * 100)) : undefined}
        barClass={thermal?.ok && thermal.hottest_c >= thermal.hot_c ? 'bg-err' : thermal?.ok && thermal.hottest_c >= thermal.warm_c ? 'bg-warn' : 'bg-ok'}
        sub={thermal?.ok && thermal.fan_duty_percent >= 0 ? t('home.fanPercent', { percent: thermal.fan_duty_percent }) : undefined}
        onClick={() => onNavigate?.('settings', 'thermal')}
      />
      {!system && [0, 1, 2].map((i) => <div key={i} className="pmgr-skeleton min-h-[6.5rem] rounded-xl border border-panel-border" />)}
      {ramUsed !== null && (
        <PanelTile
          label={t('home.memory')}
          value={`${ramUsed}%`}
          bar={ramUsed}
          barClass={volumeBarClass(ramUsed)}
          sub={t('home.memoryOf', { used: formatFileSize(system.ram_total - system.ram_free, locale), total: formatFileSize(system.ram_total, locale) })}
        />
      )}
      {volumes.map((volume) => {
        const used = Math.max(0, Math.min(100, ((volume.total - volume.free) / volume.total) * 100))
        return (
          <PanelTile
            key={volume.path}
            label={volumeLabel(volume, t)}
            value={t('home.storageFree', { size: formatFileSize(volume.free, locale) })}
            bar={used}
            barClass={volumeBarClass(used)}
            sub={formatFileSize(volume.total, locale)}
            onClick={() => { setFilesPath(volume.path); onNavigate?.('ftp') }}
          />
        )
      })}
      <PanelTile label={t('home.uptime')} value={formatUptime(status ? (Date.now() - status.started_at_ms) / 1000 : undefined)} />
      <PanelTile
        label={t('home.payloadsRunning')}
        value={payloadsRunning === null ? '—' : payloadsRunning}
        onClick={() => onNavigate?.('payloads')}
      />
      <PanelTile
        label={t('home.autoloader')}
        value={autoloader === null ? '—' : autoloader.running ? t('home.autoloaderRunning') : autoloader.enabled ? t('home.autoloaderEnabled') : t('home.autoloaderOff')}
        valueClass={autoloader?.running ? 'text-ok' : 'text-text'}
        onClick={() => onNavigate?.('autoloader')}
      />
    </section>
  )
}

const SORTS = [
  { id: 'name-asc', labelKey: 'home.sortNameAsc' },
  { id: 'name-desc', labelKey: 'home.sortNameDesc' },
  { id: 'id-asc', labelKey: 'home.sortIdAsc' },
  { id: 'recent', labelKey: 'home.sortRecent' },
]

const PLATFORM_FILTERS = [
  { id: 'all', labelKey: 'home.allGames' },
  { id: 'ps5', labelKey: 'home.ps5Games' },
  { id: 'ps4', labelKey: 'home.ps4Games' },
]

// Memoized so the 5s/10s background polls below don't re-render every tile.
// Sizes are measured one at a time so a big library does not walk every folder at once.
const sizeCache = new Map()
let sizeQueue = Promise.resolve()
function loadTitleSize(titleId) {
  if (!sizeCache.has(titleId)) {
    sizeCache.set(titleId, sizeQueue.then(() => api.titles.size(titleId).then((r) => r.size, () => null)))
    sizeQueue = sizeCache.get(titleId)
  }
  return sizeCache.get(titleId)
}

const TitleTile = memo(function TitleTile({ title, index, launching, running, onPlay, onDelete, t, locale }) {
  const [iconFailed, setIconFailed] = useState(false)
  const showIcon = title.has_icon && !iconFailed
  const lastPlayed = formatLastPlayed(title.last_played, locale)
  const played = formatPlaytime(title.played_seconds)
  const [size, setSize] = useState(null)

  useEffect(() => {
    let alive = true
    loadTitleSize(title.title_id).then((bytes) => alive && setSize(bytes))
    return () => { alive = false }
  }, [title.title_id])

  return (
    <div
      className={`pmgr-game-card pmgr-tile pmgr-tile-in group relative flex flex-col overflow-hidden rounded-2xl border bg-panel ${
        running ? 'border-ok' : 'border-panel-border'
      }`}
      style={{ '--ti': index }}
      aria-label={`${title.name}, ${title.platform || 'PlayStation'}, ${title.title_id}`}
    >
      <div className="pmgr-game-art relative w-full shrink-0 overflow-hidden bg-panel-2" style={{ paddingBottom: '100%' }}>
        {showIcon ? (
          <img
            src={api.titles.iconUrl(title.title_id)}
            alt={`${title.name} cover`}
            onError={() => setIconFailed(true)}
            loading="lazy"
            decoding="async"
            className="absolute top-0 right-0 bottom-0 left-0 h-full w-full object-contain"
          />
        ) : (
          <div className="absolute top-0 right-0 bottom-0 left-0 grid place-items-center text-accent/40">
            <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.4">
              <rect x="3" y="5" width="18" height="14" rx="2" />
              <path d="M8 9v6M5 12h6M14 10h.01M17 13h.01" strokeLinecap="round" />
            </svg>
          </div>
        )}
        {played && (
          <span
            className="absolute bottom-2 left-2 inline-flex items-center rounded-lg bg-black/70 px-2.5 py-1.5 text-xs font-bold text-white shadow-sm"
            title={t(played.key, { count: played.count })}
          >
            <ClockIcon size={13} />
            <span className="pmgr-num ml-1.5">{t(played.short.key, played.short)}</span>
          </span>
        )}
        {running && (
          <span className="absolute left-2 top-2">
            <Pill tone="ok" live>
              {t('common.running')}
            </Pill>
          </span>
        )}
      </div>

      <div className="flex min-w-0 flex-1 flex-col p-4">
        <div className="line-clamp-2 min-h-[2.5em] text-base font-bold leading-tight text-text" title={title.name}>
          {title.name}
        </div>
        <div className="mt-2 flex min-w-0 items-center text-xs text-text-dim">
          <PlatformBadge platform={title.platform} />
          <span className="pmgr-mono ml-1.5 min-w-0 truncate rounded bg-bg/70 px-1.5 py-0.5 text-xs font-semibold tracking-wide" dir="ltr">{title.title_id}</span>
        </div>
        {(title.version || size > 0) && (
          <div className="mb-3 mt-3 flex flex-wrap items-center text-sm text-text">
            {title.version && (
              <span className="mr-4 inline-flex min-w-0 items-center" title={t('home.version')}>
                <svg className="mr-1.5 shrink-0 text-text-dim" width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><path d="M20.6 13.4 13.4 20.6a2 2 0 0 1-2.8 0L3 13V3h10l7.6 7.6a2 2 0 0 1 0 2.8Z" /><path d="M7.5 7.5h.01" /></svg>
                <span className="pmgr-mono font-semibold" dir="ltr">{title.version}</span>
              </span>
            )}
            {size > 0 && (
              <span className="inline-flex items-center" title={t('common.size')}>
                <svg className="mr-1.5 shrink-0 text-text-dim" width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true"><ellipse cx="12" cy="5" rx="8" ry="3" /><path d="M4 5v14c0 1.7 3.6 3 8 3s8-1.3 8-3V5" /><path d="M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3" /></svg>
                <span className="pmgr-num font-semibold" dir="ltr">{formatFileSize(size, locale)}</span>
              </span>
            )}
          </div>
        )}
        {lastPlayed && <div className="mt-2 truncate text-xs text-text-dim" title={lastPlayed}>{t('home.lastOpened')} <span className="text-text">{lastPlayed}</span></div>}
        <div className="mt-auto flex items-center justify-between border-t border-panel-border pt-3">
          <button
            type="button"
            onClick={() => onDelete(title)}
            disabled={running}
            aria-label={t('home.uninstallGame')}
            title={running ? t('home.uninstallRunning') : t('home.uninstallGame')}
            className="pmgr-icon-btn grid h-9 w-9 shrink-0 place-items-center rounded-full text-text-dim hover:text-err disabled:opacity-40"
          >
            <DeleteIcon size={16} />
          </button>
          <button
            onClick={() => onPlay(title.title_id)}
            disabled={launching === title.title_id}
            className="pmgr-game-play pmgr-btn-sheen ml-2 inline-flex min-h-10 shrink-0 items-center justify-center rounded-full bg-accent px-5 py-2 text-sm font-bold text-white hover:bg-accent-hover disabled:opacity-50"
          >
            {launching === title.title_id ? (
              t('home.launching')
            ) : (
              <>
                <svg className="mr-1.5" width="12" height="12" viewBox="0 0 24 24" fill="currentColor">
                  <path d="M8 5v14l11-7Z" />
                </svg>
                {t('home.play')}
              </>
            )}
          </button>
        </div>
      </div>
    </div>
  )
})

export default function HomeView({ status, connection, onNavigate }) {
  const { t, locale } = useI18n()
  const [titles, setTitles] = useState(null)
  const [titleError, setTitleError] = useState('')
  const [runningTitleId, setRunningTitleId] = useState('')
  const [overview, setOverview] = useState({ thermal: null, autoloader: null })
  const [system, setSystem] = useState(null)
  const [payloadsRunning, setPayloadsRunning] = useState(null)
  const [userName, setUserName] = useState('')
  const [search, setSearch] = useState('')
  const [sort, setSort] = useState('name-asc')
  const [platformFilter, setPlatformFilter] = useState('all')
  const [libraryKind, setLibraryKind] = useState('game') // games and apps/homebrew are listed apart
  const [launching, setLaunching] = useState('')
  const [qrOpen, setQrOpen] = useState(false)
  const [closeGameOpen, setCloseGameOpen] = useState(false)
  const [uninstallTarget, setUninstallTarget] = useState(null)
  const [tileLimit, setTileLimit] = useState(TILE_PAGE)
  const showToast = useToast()
  // Opened on the console itself the page is on localhost, so share its LAN address instead.
  const shareUrl = !isLoopbackHost ? location.origin : system?.ip_address ? `${location.protocol}//${system.ip_address}${location.port ? `:${location.port}` : ''}` : ''

  const loadTitles = () => {
    setTitleError('')
    api.titles.list().then((r) => setTitles(r.titles)).catch((error) => setTitleError(error.message))
  }

  usePolling(() => api.titles.running().then((r) => setRunningTitleId(r.title_id)).catch(() => {}), 5000)

  useEffect(loadTitles, [])

  // Just the foreground user's name for the greeting. A one-time load on mount is
  // enough — it doesn't change minute to minute.
  useEffect(() => {
    api.system.info().then((info) => setUserName(info.user_name || '')).catch(() => {})
  }, [])

  // Poll only while the tab is visible; system info and payload counts change slowly.
  usePolling(() => {
    Promise.allSettled([api.thermal.status(), api.autoloader.status(), api.system.info(), api.payloads.list(), api.processes.list()]).then(
      ([thermal, autoloader, info, payloads, processes]) => {
        setOverview({
          thermal: thermal.status === 'fulfilled' ? thermal.value : null,
          autoloader: autoloader.status === 'fulfilled' ? autoloader.value : null,
        })
        if (info.status === 'fulfilled') setSystem(info.value)
        if (payloads.status === 'fulfilled' && processes.status === 'fulfilled') {
          const running = new Set((processes.value.processes || []).map((p) => stemOf(p.name || '')))
          setPayloadsRunning(payloads.value.payloads.filter((p) => running.has(stemOf(p.name))).length)
        }
      },
    )
  }, 10000)

  const play = useCallback((titleId) => {
    setLaunching(titleId)
    api.titles
      .launch(titleId)
      .then((r) => showToast(t(r.via_notification ? 'home.launchNotificationToast' : 'home.launchedToast')))
      .catch((e) => showToast(e.message, 'err'))
      .finally(() => setLaunching(''))
  }, [showToast, t])

  // Keeps typing from re-filtering and re-reconciling the whole grid per key.
  const debouncedSearch = useDebouncedValue(search, 150)

  const view = useMemo(() => {
    if (!titles) return []
    let items = titles.filter((title) => (title.kind === 'app' ? 'app' : 'game') === libraryKind)
    if (libraryKind === 'game' && platformFilter !== 'all') {
      items = items.filter((title) => String(title.platform || '').toLowerCase().includes(platformFilter))
    }
    if (debouncedSearch.trim()) {
      const q = debouncedSearch.trim().toLocaleLowerCase(locale)
      items = items.filter((title) => title.name.toLocaleLowerCase(locale).includes(q) || title.title_id.toLowerCase().includes(q))
    }
    items = [...items]
    if (sort === 'name-asc') items.sort((a, b) => a.name.localeCompare(b.name, locale))
    if (sort === 'name-desc') items.sort((a, b) => b.name.localeCompare(a.name, locale))
    if (sort === 'id-asc') items.sort((a, b) => a.title_id.localeCompare(b.title_id))
    if (sort === 'recent') items.sort((a, b) => (b.last_played || '').localeCompare(a.last_played || ''))
    if (runningTitleId) items.sort((a, b) => Number(b.title_id === runningTitleId) - Number(a.title_id === runningTitleId))
    return items
  }, [titles, debouncedSearch, sort, platformFilter, libraryKind, runningTitleId, locale])

  const platformCounts = useMemo(() => {
    const counts = { all: 0, ps5: 0, ps4: 0 }
    for (const title of titles || []) {
      if (title.kind === 'app') continue
      counts.all++
      const platform = String(title.platform || '').toLowerCase()
      if (platform.includes('ps5')) counts.ps5++
      if (platform.includes('ps4')) counts.ps4++
    }
    return counts
  }, [titles])

  const appCount = useMemo(() => (titles || []).filter((title) => title.kind === 'app').length, [titles])
  const gameCount = (titles?.length || 0) - appCount
  const runningTitle = titles?.find((title) => title.title_id === runningTitleId)
  const titleTiles = useMemo(
    () => view.slice(0, tileLimit).map((title, i) => (
      <TitleTile
        key={title.title_id}
        title={title}
        index={i}
        launching={launching}
        running={title.title_id === runningTitleId}
        onPlay={play}
        onDelete={setUninstallTarget}
        t={t}
        locale={locale}
      />
    )),
    [view, tileLimit, launching, runningTitleId, play, t, locale],
  )

  return (
    <div className="pmgr-home space-y-5">
      <section className="pmgr-hero pmgr-surface flex flex-wrap items-center justify-between gap-x-6 gap-y-4 rounded-2xl border border-panel-border bg-panel p-4 sm:p-5">
        <div>
          <div className="flex items-center gap-2 text-xs font-bold uppercase tracking-[0.14em] text-text-dim">
            <span className={`h-1.5 w-1.5 rounded-full ${connection === 'online' ? 'bg-ok' : 'bg-warn'}`} />
            {connection === 'online' ? t('home.connected') : t('home.waiting')}
          </div>
          <h2 className="mt-1 text-2xl font-bold tracking-tight text-text">{t(greetingKey(userName), { name: userName })}</h2>
          {shareUrl && (
            <button
              type="button"
              onClick={() => setQrOpen(true)}
              className="pmgr-icon-btn mt-2 inline-flex items-center gap-1.5 rounded-full border border-panel-border px-2.5 py-1 text-xs font-semibold text-text-dim hover:border-accent/50 hover:text-accent"
            >
              <QrIcon size={14} />
              {t('home.qrOpen')}
            </button>
          )}
        </div>
        {system && (
          <div className="flex items-center">
            {system.firmware && (
              <div className="py-1 pr-5 sm:pr-6">
                <div className="text-[11px] font-bold uppercase tracking-[0.14em] text-text-dim">{t('settings.metricFirmware')}</div>
                <div className="pmgr-mono mt-1 text-xl font-bold leading-none text-text sm:text-2xl" dir="ltr">{system.firmware}</div>
              </div>
            )}
            {system.ip_address && (
              <div className={`min-w-0 py-1 ${system.firmware ? 'border-l border-panel-border pl-5 sm:pl-6' : ''}`}>
                <div className="text-[11px] font-bold uppercase tracking-[0.14em] text-text-dim">IP</div>
                <div className="pmgr-mono mt-1 text-xl font-bold leading-none text-text sm:text-2xl" dir="ltr">{system.ip_address}</div>
              </div>
            )}
          </div>
        )}
      </section>

      <ConsolePanel overview={overview} status={status} system={system} payloadsRunning={payloadsRunning} onNavigate={onNavigate} t={t} locale={locale} />

      {closeGameOpen && runningTitle && (
        <ConfirmDialog
          title={t('home.closeGameTitle', { name: runningTitle.name })}
          description={t('home.closeGameDesc')}
          confirmLabel={t('home.closeGame')}
          cancelLabel={t('common.cancel')}
          tone="danger"
          onClose={() => setCloseGameOpen(false)}
          onConfirm={() => api.titles.close().then(() => {
            showToast(t('home.closedToast'))
            setRunningTitleId('')
          })}
        />
      )}

      {uninstallTarget && (
        <ConfirmDialog
          title={t('home.uninstallTitle', { name: uninstallTarget.name })}
          description={t('home.uninstallDesc')}
          confirmLabel={t('home.uninstallGame')}
          cancelLabel={t('common.cancel')}
          tone="danger"
          onClose={() => setUninstallTarget(null)}
          onConfirm={() => api.titles.uninstall(uninstallTarget.title_id).then(() => {
            showToast(t('home.uninstallStartedToast', { name: uninstallTarget.name }))
            // The console removes the files in the background, so the list is refreshed a couple of times.
            window.setTimeout(loadTitles, 3000)
            window.setTimeout(loadTitles, 10000)
          })}
        />
      )}

      {qrOpen && shareUrl && <QrCodeDialog url={shareUrl} onClose={() => setQrOpen(false)} />}

      {runningTitle && (
        <section className="relative isolate flex items-center gap-4 overflow-hidden rounded-2xl border border-ok/25 bg-panel p-3.5 sm:p-4">
          {runningTitle.has_icon && (
            <img
              src={api.titles.iconUrl(runningTitle.title_id)}
              alt=""
              aria-hidden="true"
              className="pmgr-cover-bg pointer-events-none absolute -left-4 top-1/2 -z-10 h-48 w-48 -translate-y-1/2 scale-150 object-cover opacity-25 blur-2xl"
            />
          )}
          <div className="h-16 w-16 shrink-0 overflow-hidden rounded-xl bg-panel-2 sm:h-20 sm:w-20">
            {runningTitle.has_icon ? (
              <img src={api.titles.iconUrl(runningTitle.title_id)} alt="" className="h-full w-full object-cover" />
            ) : (
              <div className="grid h-full w-full place-items-center text-text-dim">
                <svg width="26" height="26" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.4"><rect x="3" y="5" width="18" height="14" rx="2" /></svg>
              </div>
            )}
          </div>
          <div className="min-w-0 flex-1">
            <Pill tone="ok" live>{t('home.currentlyPlaying')}</Pill>
            <div className="mt-1.5 truncate text-lg font-bold text-text">{runningTitle.name}</div>
            <div className="mt-0.5 flex min-w-0 items-center gap-1.5 text-xs text-text-dim">
              <PlatformBadge platform={runningTitle.platform} />
              <span className="pmgr-mono truncate" dir="ltr">{runningTitle.title_id}</span>
            </div>
          </div>
          <Button tone="danger" className="shrink-0" icon={<CloseIcon size={16} />} onClick={() => setCloseGameOpen(true)}>
            {t('home.closeGame')}
          </Button>
        </section>
      )}

      <section className="grid grid-cols-2 gap-3 sm:grid-cols-3 xl:grid-cols-5">
        {QUICK_ACTIONS.map((a) => (
          <button
            key={a.id}
            onClick={() => onNavigate?.(a.target || a.id, a.tab)}
            className="pmgr-tile group flex items-center gap-3 rounded-xl border border-panel-border bg-panel p-3.5 text-left"
          >
            <span className="pmgr-home-action-icon grid h-10 w-10 shrink-0 place-items-center rounded-lg bg-accent/10 text-accent transition-transform duration-200 group-hover:scale-105 group-focus-visible:scale-105">{a.icon || ICONS[a.id]}</span>
            <span className="min-w-0 flex-1">
              <span className="block text-sm font-bold text-text">{t(a.labelKey)}</span>
              <span className="line-clamp-2 block text-xs text-text-dim">{t(a.descKey)}</span>
            </span>
            <ChevronRightIcon size={15} className="pmgr-home-action-chevron hidden shrink-0 text-text-dim/60 2xl:block transition-transform duration-200 group-hover:translate-x-0.5 group-hover:text-accent" />
          </button>
        ))}
      </section>

      <Panel
        title={t('home.library')}
        className="pmgr-home-library"
        actions={<span className="rounded-full bg-panel-border px-2.5 py-1 text-xs font-semibold text-text-dim">{titles === null ? t(titleError ? 'common.error' : 'home.loadingLibrary') : t('home.installedCount', { count: libraryKind === 'app' ? appCount : gameCount })}</span>}
      >
      <div className="pmgr-library-toolbar mb-4 rounded-xl border border-panel-border bg-bg/45 p-3">
        <div className="flex flex-wrap items-center gap-2">
        <SearchField value={search} onChange={setSearch} placeholder={t(libraryKind === 'app' ? 'home.searchApps' : 'home.searchGames')} className="min-w-48 flex-1" />
        <select
          value={sort}
          onChange={(e) => setSort(e.target.value)}
          className="rounded-lg border border-panel-border bg-bg px-2.5 py-2 text-xs text-text outline-none focus:border-accent"
        >
          {SORTS.map((s) => (
            <option key={s.id} value={s.id}>
              {t('home.sortPrefix', { label: t(s.labelKey) })}
            </option>
          ))}
        </select>
        <button
          onClick={loadTitles}
          className="pmgr-icon-btn inline-flex items-center gap-1.5 rounded-lg border border-panel-border px-3 py-2 text-xs font-semibold text-text-dim transition hover:border-accent/40 hover:text-text"
        >
          <RefreshIcon size={13} /> {t('home.refresh')}
        </button>
        </div>
        <div className="mt-3 flex flex-wrap items-center gap-2">
          <SegmentedControl
            className="mr-2"
            options={[
              { id: 'game', label: t('home.kindGames'), count: gameCount },
              { id: 'app', label: t('home.kindApps'), count: appCount },
            ]}
            value={libraryKind}
            onChange={setLibraryKind}
          />
          {libraryKind === 'game' && (
            <SegmentedControl
              options={PLATFORM_FILTERS.map((f) => ({ ...f, label: t(f.labelKey), count: platformCounts[f.id] }))}
              value={platformFilter}
              onChange={setPlatformFilter}
            />
          )}
          {runningTitleId && <span className="ml-auto inline-flex items-center gap-1.5 text-xs font-bold uppercase tracking-wide text-ok"><span className="pmgr-live-dot" /> {t('home.runningFirst')}</span>}
        </div>
      </div>

      <div className="mb-3 text-xs font-medium uppercase tracking-wide text-text-dim">
        {titles === null ? t('home.loadingEllipsis') : t('home.titlesInView', { count: view.length })}
      </div>

      {titleError && titles !== null && (
        <div className="pmgr-notice-in mb-3 flex items-center justify-between gap-3 rounded-xl border border-err/20 bg-err/10 px-3 py-2.5 text-sm text-err">
          <span>{titleError}</span>
          <Button size="sm" tone="danger" onClick={loadTitles}>{t('common.tryAgain')}</Button>
        </div>
      )}

      {titleError && titles === null ? (
        <EmptyState
          title={t('common.error')}
          description={titleError}
          action={<Button tone="neutral" onClick={loadTitles}>{t('common.tryAgain')}</Button>}
        />
      ) : titles === null ? (
        <SkeletonGrid count={8} gridClassName="gap-4" gridStyle={GAMES_GRID} />
      ) : (
        <div className="grid gap-4" style={GAMES_GRID}>
          {titleTiles}
        </div>
      )}

      {titles !== null && view.length > tileLimit && (
        <div className="flex justify-center">
          <Button tone="neutral" onClick={() => setTileLimit((limit) => limit + TILE_PAGE)}>
            {t('common.showMore', { count: Math.min(TILE_PAGE, view.length - tileLimit) })}
          </Button>
        </div>
      )}

      {titles !== null && view.length === 0 && (
        <EmptyState
          icon={
            <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.5"><rect x="3" y="5" width="18" height="14" rx="3" /><path d="M8 10v4M6 12h4M15 11h.01M18 13h.01" strokeLinecap="round" /></svg>
          }
          title={libraryKind === 'app' && appCount === 0 ? t('home.noAppsTitle') : titles.length === 0 ? t('home.noTitlesTitle') : t('home.noMatchTitle')}
          description={libraryKind === 'app' && appCount === 0 ? t('home.noAppsDesc') : titles.length === 0 ? t('home.noTitlesDesc') : t('home.noMatchDesc')}
        />
      )}
      </Panel>
    </div>
  )
}
