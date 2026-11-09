import { Suspense, useCallback, useEffect, useRef, useState } from 'react'
import NavRail from './components/NavRail.jsx'
import Header from './components/Header.jsx'
import ErrorBoundary from './components/ErrorBoundary.jsx'
import SupportModal from './components/SupportModal.jsx'
import OnboardingTour from './components/OnboardingTour.jsx'
import GlobalActivity from './components/GlobalActivity.jsx'
import CommandPalette from './components/CommandPalette.jsx'
import { api } from './lib/api.js'
import { useI18n } from './lib/i18n.jsx'
import { POLL_SCALE } from './lib/usePolling.js'
import { isPS5 } from './lib/effects.js'

// Each page is its own chunk, so the console's browser (no JIT) only parses the page in use.
// A page that already finished loading renders synchronously, with no placeholder flash.
function lazyView(load) {
  let Loaded = null
  let failure = null
  let pending = null
  const start = () => pending || (pending = load().then((m) => { Loaded = m.default }, (e) => { failure = e }))
  function View(props) {
    if (failure) throw failure
    if (!Loaded) throw start()
    return <Loaded {...props} />
  }
  View.preload = start
  return View
}

const HomeView = lazyView(() => import('./views/HomeView.jsx'))
const FtpView = lazyView(() => import('./views/FtpView.jsx'))
const PayloadsView = lazyView(() => import('./views/PayloadsView.jsx'))
const AutoloaderView = lazyView(() => import('./views/AutoloaderView.jsx'))
const RepositoryView = lazyView(() => import('./views/RepositoryView.jsx'))
const ProcessesView = lazyView(() => import('./views/ProcessesView.jsx'))
const SettingsView = lazyView(() => import('./views/SettingsView.jsx'))
const LogsView = lazyView(() => import('./views/LogsView.jsx'))
const AvatarView = lazyView(() => import('./views/AvatarView.jsx'))
const SavesView = lazyView(() => import('./views/SavesView.jsx'))
const PkgView = lazyView(() => import('./views/PkgView.jsx'))
const MediaView = lazyView(() => import('./views/MediaView.jsx'))

const VIEWS = {
  home: HomeView,
  ftp: FtpView,
  payloads: PayloadsView,
  autoloader: AutoloaderView,
  repository: RepositoryView,
  pkg: PkgView,
  saves: SavesView,
  media: MediaView,
  processes: ProcessesView,
  avatar: AvatarView,
  settings: SettingsView,
  logs: LogsView,
}

const TITLE_KEYS = {
  home: 'nav.home',
  ftp: 'nav.files',
  payloads: 'nav.payloads',
  autoloader: 'nav.autoloader',
  repository: 'nav.payloadRepositories',
  pkg: 'nav.installPkg',
  saves: 'nav.saveBackups',
  media: 'nav.media',
  processes: 'nav.runningProcesses',
  avatar: 'nav.profile',
  settings: 'nav.settings',
  logs: 'nav.logs',
}

const TOUR_SEEN_KEY = 'pmgr_tour_seen'
const VALID_VIEWS = new Set(Object.keys(VIEWS))
const VALID_SETTINGS_TABS = new Set(['general', 'appearance', 'thermal', 'hardware', 'experimental', 'about'])

function routeFromLocation() {
  const [view, detail] = window.location.hash.replace(/^#\/?/, '').split('?')[0].split('/')
  // Keeps bookmarks to the old Settings tab working.
  if (view === 'settings' && detail === 'autoloader') return { view: 'autoloader', settingsTab: null }
  if (!VALID_VIEWS.has(view)) return { view: 'home', settingsTab: null }
  const settingsTab = view === 'settings' && VALID_SETTINGS_TABS.has(detail) ? detail : null
  return { view, settingsTab }
}

// On a computer every page is fetched quietly after the first paint, so switching pages is instant.
// The console skips this and loads a page only when it is opened.
function prefetchViews() {
  if (isPS5()) return
  Object.values(VIEWS).forEach((View, i) => window.setTimeout(View.preload, 600 + i * 150))
}

export default function App() {
  const { t } = useI18n()
  const initialRoute = routeFromLocation()
  const [active, setActive] = useState(initialRoute.view)
  const [settingsTab, setSettingsTab] = useState(initialRoute.settingsTab)
  const [pkgMounted, setPkgMounted] = useState(initialRoute.view === 'pkg')
  const [status, setStatus] = useState(null)
  const [connection, setConnection] = useState('connecting')

  useEffect(prefetchViews, [])
  const mainRef = useRef(null)
  const [supportOpen, setSupportOpen] = useState(false)
  const [tourOpen, setTourOpen] = useState(() => !window.localStorage.getItem(TOUR_SEEN_KEY))
  const [commandOpen, setCommandOpen] = useState(false)

  const refreshStatus = useCallback(() => {
    setConnection((current) => (current === 'offline' ? 'reconnecting' : current))
    return api
      .status()
      .then(({ uptime_seconds: uptime, ...rest }) => {
        // The uptime ticks on every poll and would redraw the whole shell, so only a start time is kept.
        const next = { ...rest, started_at_ms: Date.now() - uptime * 1000 }
        setStatus((prev) => (prev && JSON.stringify({ ...prev, started_at_ms: 0 }) === JSON.stringify({ ...next, started_at_ms: 0 }) ? prev : next))
        setConnection('online')
      })
      .catch(() => setConnection('offline'))
  }, [])

  useEffect(() => {
    let cancelled = false
    const poll = () => {
      if (cancelled || document.visibilityState === 'hidden') return
      refreshStatus()
    }
    poll()
    const id = setInterval(poll, 5000 * POLL_SCALE)
    const onVisible = () => document.visibilityState === 'visible' && poll()
    document.addEventListener('visibilitychange', onVisible)
    return () => {
      cancelled = true
      clearInterval(id)
      document.removeEventListener('visibilitychange', onVisible)
    }
  }, [refreshStatus])

  useEffect(() => {
    const onLocation = () => {
      const route = routeFromLocation()
      setActive(route.view)
      setSettingsTab(route.settingsTab)
    }
    window.addEventListener('hashchange', onLocation)
    return () => window.removeEventListener('hashchange', onLocation)
  }, [])

  // Ctrl+K/Cmd+K opens the command palette from anywhere — the standard
  // shortcut, so it doesn't need to be discovered to be used.
  useEffect(() => {
    const onKeyDown = (e) => {
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'k') {
        e.preventDefault()
        setCommandOpen((open) => !open)
      }
    }
    window.addEventListener('keydown', onKeyDown)
    return () => window.removeEventListener('keydown', onKeyDown)
  }, [])

  const navigate = useCallback((view, tab) => {
    if (!VALID_VIEWS.has(view)) return
    const nextTab = view === 'settings' && VALID_SETTINGS_TABS.has(tab) ? tab : null
    setActive(view)
    setSettingsTab(nextTab)
    const nextHash = `#/${view}${nextTab ? `/${nextTab}` : ''}`
    if (window.location.hash !== nextHash) window.location.hash = nextHash.slice(1)
  }, [])

  // When the Autoloader starts (at boot, with this page already open), show it. Only while it is active,
  // once per run, and only for the first minutes after connecting.
  const autoloaderShown = useRef(false)
  useEffect(() => {
    if (connection !== 'online') return undefined
    let alive = true
    const check = () => {
      if (document.visibilityState === 'hidden') return
      api.autoloader.status().then((s) => {
        if (!alive) return
        if (!s.running) autoloaderShown.current = false
        else if (!autoloaderShown.current) {
          autoloaderShown.current = true
          navigate('autoloader')
        }
      }).catch(() => {})
    }
    check()
    const poll = window.setInterval(check, 1500 * POLL_SCALE)
    const stop = window.setTimeout(() => window.clearInterval(poll), 120000)
    return () => {
      alive = false
      window.clearInterval(poll)
      window.clearTimeout(stop)
    }
  }, [connection, navigate])

  const closeTour = () => {
    window.localStorage.setItem(TOUR_SEEN_KEY, '1')
    setTourOpen(false)
  }

  // Name the tab after the page and start each page at the top.
  useEffect(() => {
    document.title = `${t(TITLE_KEYS[active])} · Prospero Manager`
    if (mainRef.current) mainRef.current.scrollTop = 0
  }, [active, t])

  // Keep the PKG page mounted so a running install keeps its progress.
  useEffect(() => {
    if (active === 'pkg') setPkgMounted(true)
  }, [active])

  const ActiveView = VIEWS[active]

  return (
    <div className="pmgr-app flex h-screen overflow-hidden bg-bg">
      <div className="pmgr-bg" />
      <div className="pmgr-aurora-blob" />
      <div className="pmgr-aurora-blob pmgr-aurora-blob-2" />
      <NavRail active={active} onChange={navigate} />

      <div className="flex min-w-0 flex-1 flex-col">
        <Header
          title={t(TITLE_KEYS[active])}
          status={status}
          connection={connection}
          onRetry={refreshStatus}
          onOpenHelp={() => setTourOpen(true)}
          onOpenSupport={() => setSupportOpen(true)}
          onOpenCommand={() => setCommandOpen(true)}
        />
        {connection === 'offline' && (
          <button
            type="button"
            onClick={refreshStatus}
            className="pmgr-offline-banner flex items-center justify-center gap-2 px-4 py-2 text-sm font-medium"
          >
            <span className="h-2 w-2 rounded-full bg-err" />
            {t('header.offlineBanner')}
          </button>
        )}
        <main ref={mainRef} className="pmgr-main min-w-0 flex-1 overflow-y-auto p-4 pb-24 sm:p-5 sm:pb-24 md:p-6 md:pb-6">
          {active !== 'pkg' && (
            <div key={active} className="pmgr-view-enter mx-auto w-full max-w-[1800px]">
              <ErrorBoundary resetKey={active}>
                <Suspense fallback={<div className="pmgr-skeleton h-40 rounded-2xl" />}>
                  <ActiveView status={status} connection={connection} onNavigate={navigate} settingsTab={settingsTab} />
                </Suspense>
              </ErrorBoundary>
            </div>
          )}
          {pkgMounted && (
            <div hidden={active !== 'pkg'} className="pmgr-view-enter mx-auto w-full max-w-[1800px]">
              <ErrorBoundary resetKey="pkg">
                <Suspense fallback={<div className="pmgr-skeleton h-40 rounded-2xl" />}>
                  <PkgView status={status} connection={connection} onNavigate={navigate} visible={active === 'pkg'} />
                </Suspense>
              </ErrorBoundary>
            </div>
          )}
        </main>
      </div>

      {supportOpen && <SupportModal onClose={() => setSupportOpen(false)} />}
      {tourOpen && <OnboardingTour onClose={closeTour} onNavigate={navigate} />}
      {commandOpen && <CommandPalette onClose={() => setCommandOpen(false)} onNavigate={navigate} />}
      <GlobalActivity connection={connection} onNavigate={navigate} />
    </div>
  )
}
