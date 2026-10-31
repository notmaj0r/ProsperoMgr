import { useCallback, useEffect, useState } from 'react'
import NavRail from './components/NavRail.jsx'
import Header from './components/Header.jsx'
import ErrorBoundary from './components/ErrorBoundary.jsx'
import SupportModal from './components/SupportModal.jsx'
import OnboardingTour from './components/OnboardingTour.jsx'
import GlobalActivity from './components/GlobalActivity.jsx'
import CommandPalette from './components/CommandPalette.jsx'
import HomeView from './views/HomeView.jsx'
import FtpView from './views/FtpView.jsx'
import PayloadsView from './views/PayloadsView.jsx'
import RepositoryView from './views/RepositoryView.jsx'
import ProcessesView from './views/ProcessesView.jsx'
import SettingsView from './views/SettingsView.jsx'
import LogsView from './views/LogsView.jsx'
import AvatarView from './views/AvatarView.jsx'
import SavesView from './views/SavesView.jsx'
import PkgView from './views/PkgView.jsx'
import StorageView from './views/StorageView.jsx'
import { api } from './lib/api.js'
import { useI18n } from './lib/i18n.jsx'

const VIEWS = {
  home: HomeView,
  ftp: FtpView,
  payloads: PayloadsView,
  repository: RepositoryView,
  pkg: PkgView,
  saves: SavesView,
  storage: StorageView,
  processes: ProcessesView,
  avatar: AvatarView,
  settings: SettingsView,
  logs: LogsView,
}

const TITLE_KEYS = {
  home: 'nav.home',
  ftp: 'nav.files',
  payloads: 'nav.payloads',
  repository: 'nav.payloadRepositories',
  pkg: 'nav.installPkg',
  saves: 'nav.saveBackups',
  storage: 'nav.storageAnalyzer',
  processes: 'nav.runningProcesses',
  avatar: 'nav.profile',
  settings: 'nav.settings',
  logs: 'nav.logs',
}

const TOUR_SEEN_KEY = 'pmgr_tour_seen'
const VALID_VIEWS = new Set(Object.keys(VIEWS))
const VALID_SETTINGS_TABS = new Set(['general', 'autoloader', 'appearance', 'thermal', 'experimental', 'about'])

function routeFromLocation() {
  const [view, detail] = window.location.hash.replace(/^#\/?/, '').split('/')
  if (!VALID_VIEWS.has(view)) return { view: 'home', settingsTab: null }
  const settingsTab = view === 'settings' && VALID_SETTINGS_TABS.has(detail) ? detail : null
  return { view, settingsTab }
}

export default function App() {
  const { t } = useI18n()
  const initialRoute = routeFromLocation()
  const [active, setActive] = useState(initialRoute.view)
  const [settingsTab, setSettingsTab] = useState(initialRoute.settingsTab)
  const [status, setStatus] = useState(null)
  const [connection, setConnection] = useState('connecting')
  const [supportOpen, setSupportOpen] = useState(false)
  const [tourOpen, setTourOpen] = useState(() => !window.localStorage.getItem(TOUR_SEEN_KEY))
  const [commandOpen, setCommandOpen] = useState(false)

  const refreshStatus = useCallback(() => {
    setConnection((current) => (current === 'offline' ? 'reconnecting' : current))
    return api
      .status()
      .then((s) => {
        setStatus(s)
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
    const id = setInterval(poll, 5000)
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

  const closeTour = () => {
    window.localStorage.setItem(TOUR_SEEN_KEY, '1')
    setTourOpen(false)
  }

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
        <main className="pmgr-main min-w-0 flex-1 overflow-y-auto p-4 pb-24 sm:p-5 sm:pb-24 md:p-6 md:pb-6">
          <div key={active} className="pmgr-view-enter mx-auto w-full max-w-[1800px]">
            <ErrorBoundary resetKey={active}>
              <ActiveView status={status} connection={connection} onNavigate={navigate} settingsTab={settingsTab} />
            </ErrorBoundary>
          </div>
        </main>
      </div>

      {supportOpen && <SupportModal onClose={() => setSupportOpen(false)} />}
      {tourOpen && <OnboardingTour onClose={closeTour} onNavigate={navigate} />}
      {commandOpen && <CommandPalette onClose={() => setCommandOpen(false)} onNavigate={navigate} />}
      <GlobalActivity connection={connection} onNavigate={navigate} />
    </div>
  )
}
