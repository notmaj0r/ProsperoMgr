import { useEffect, useState } from 'react'
import { ChevronRightIcon } from './icons.jsx'
import { useDismiss } from '../lib/useDismiss.js'
import { useI18n } from '../lib/i18n.jsx'
import { useDialogAccessibility } from '../lib/useDialogAccessibility.js'

// Exported so OnboardingTour can reuse the same icon set per step.
export const ICONS = {
  home: (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7">
      <path d="M4 11.5 12 4l8 7.5" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M6 10v9a1 1 0 0 0 1 1h3v-5.5h4V20h3a1 1 0 0 0 1-1v-9" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  ),
  ftp: (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7">
      <path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7Z" />
    </svg>
  ),
  payloads: (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7">
      <path d="M13 2 4 14h6l-1 8 9-12h-6l1-8Z" strokeLinejoin="round" />
    </svg>
  ),
  repository: (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7">
      <path d="M12 3 4 7v10l8 4 8-4V7l-8-4Z" strokeLinejoin="round" />
      <path d="M4 7l8 4 8-4M12 11v10" strokeLinejoin="round" />
    </svg>
  ),
  processes: (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7">
      <path d="M4 6h16M4 12h10M4 18h13" strokeLinecap="round" />
    </svg>
  ),
  settings: (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="currentColor">
      <path d="M19.43 12.98c.04-.32.07-.64.07-.98 0-.34-.03-.66-.07-.98l2.11-1.65c.19-.15.24-.42.12-.64l-2-3.46c-.12-.22-.39-.3-.61-.22l-2.49 1c-.52-.4-1.08-.73-1.69-.98l-.38-2.65c-.03-.24-.24-.42-.49-.42h-4c-.25 0-.46.18-.49.42l-.38 2.65c-.61.25-1.17.59-1.69.98l-2.49-1c-.23-.09-.49 0-.61.22l-2 3.46c-.13.22-.07.49.12.64l2.11 1.65c-.04.32-.07.65-.07.98 0 .33.03.66.07.98l-2.11 1.65c-.19.15-.24.42-.12.64l2 3.46c.12.22.39.3.61.22l2.49-1c.52.4 1.08.73 1.69.98l.38 2.65c.03.24.24.42.49.42h4c.25 0 .46-.18.49-.42l.38-2.65c.61-.25 1.17-.59 1.69-.98l2.49 1c.23.09.49 0 .61-.22l2-3.46c.12-.22.07-.49-.12-.64l-2.11-1.65ZM12 15.5a3.5 3.5 0 1 1 0-7 3.5 3.5 0 0 1 0 7Z" />
    </svg>
  ),
  logs: (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7">
      <rect x="3" y="4" width="18" height="16" rx="2" />
      <path d="m7 10 3 2-3 2M13 14h4" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  ),
  avatar: (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7">
      <circle cx="12" cy="8" r="3.5" />
      <path d="M5 20c1.2-3.8 4-5.5 7-5.5s5.8 1.7 7 5.5" strokeLinecap="round" />
    </svg>
  ),
  saves: (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7">
      <path d="M5 4h11l3 3v13a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1Z" strokeLinejoin="round" />
      <path d="M8 4v5h7V4M8 14h8v6H8v-6Z" strokeLinejoin="round" />
    </svg>
  ),
  storage: (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7">
      <rect x="3" y="4" width="18" height="16" rx="2" />
      <path d="M11 4v16M11 13h10M16 13v7" strokeLinejoin="round" />
    </svg>
  ),
  pkg: (
    <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7">
      <path d="M12 3v11" strokeLinecap="round" />
      <path d="m7.5 9.5 4.5 4.5 4.5-4.5" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M4 16v3a1 1 0 0 0 1 1h14a1 1 0 0 0 1-1v-3" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  ),
}

// Grouped by how often you'd reach for it, not alphabetically — everyday tasks first, console
// internals last. `id` (routing/API) stays stable even when the label changes, e.g. "Processes" -> "Running Processes".
export const NAV = [
  { id: 'home', labelKey: 'nav.home', group: 'Main' },
  { id: 'ftp', labelKey: 'nav.files', group: 'Main' },
  { id: 'payloads', labelKey: 'nav.payloads', group: 'Main' },
  { id: 'pkg', labelKey: 'nav.installPkg', group: 'Main' },
  { id: 'saves', labelKey: 'nav.saveBackups', group: 'Main' },
  { id: 'repository', labelKey: 'nav.payloadRepositories', mobileLabelKey: 'nav.repositoriesShort', group: 'Tools' },
  { id: 'storage', labelKey: 'nav.storageAnalyzer', group: 'Tools' },
  { id: 'avatar', labelKey: 'nav.profile', group: 'Tools' },
  { id: 'processes', labelKey: 'nav.runningProcesses', group: 'Advanced' },
  { id: 'logs', labelKey: 'nav.logs', group: 'Advanced' },
  { id: 'settings', labelKey: 'nav.settings', group: 'System' },
]

const GROUP_LABEL_KEYS = {
  Main: 'nav.groupMain',
  Tools: 'nav.groupTools',
  Advanced: 'nav.groupAdvanced',
  System: 'nav.groupSystem',
}

const MOBILE_PRIMARY = new Set(['home', 'ftp', 'payloads', 'pkg'])
const COLLAPSE_KEY = 'pmgr_nav_collapsed'

function MobileMoreSheet({ active, closing, onClose, onOpen, t }) {
  const dialogRef = useDialogAccessibility(onClose)

  return (
    <div
      className={`fixed bottom-0 left-0 right-0 top-0 z-50 bg-black/45 pmgr-blur-sheet backdrop-blur-sm transition-opacity duration-150 md:hidden ${closing ? 'opacity-0' : 'opacity-100'}`}
      onClick={onClose}
    >
      <div
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-label={t('nav.more')}
        tabIndex={-1}
        className={`pmgr-sheet ${closing ? 'is-closing' : ''} absolute bottom-0 left-0 right-0 grid grid-cols-3 gap-2 rounded-t-2xl border-t border-panel-border bg-panel p-3 pb-[calc(0.75rem+env(safe-area-inset-bottom))] shadow-2xl`}
        onClick={(event) => event.stopPropagation()}
      >
        <div className="col-span-3 mx-auto mb-1 h-1 w-10 rounded-full bg-panel-border" />
        {NAV.filter((item) => !MOBILE_PRIMARY.has(item.id)).map((item) => (
          <button
            type="button"
            key={item.id}
            onClick={() => onOpen(item.id)}
            className={`flex min-h-20 flex-col items-center justify-center gap-2 rounded-xl text-xs font-semibold ${
              active === item.id ? 'bg-accent text-white' : 'bg-panel-2 text-text-dim'
            }`}
          >
            {ICONS[item.id]}
            {t(item.labelKey)}
          </button>
        ))}
      </div>
    </div>
  )
}

export default function NavRail({ active, onChange }) {
  const { t } = useI18n()
  const [moreOpen, setMoreOpen] = useState(false)
  const [collapsed, setCollapsed] = useState(() => window.localStorage.getItem(COLLAPSE_KEY) === '1')
  const { closing: moreClosing, close: closeMore } = useDismiss(() => setMoreOpen(false))

  useEffect(() => {
    window.localStorage.setItem(COLLAPSE_KEY, collapsed ? '1' : '0')
  }, [collapsed])

  const open = (id) => {
    setMoreOpen(false)
    onChange(id)
  }
  const moreActive = !MOBILE_PRIMARY.has(active)

  return (
    <>
      {/* Icon column stays at a fixed position the whole time — only the
          label strip next to it grows/shrinks. Animating justify-content
          instead (icon jumping from centered to left-aligned) is what made
          the old version feel like it snapped instead of sliding. */}
      <aside
        className={`pmgr-nav-shell hidden h-full shrink-0 flex-col border-r border-panel-border transition-[width] duration-300 ease-[cubic-bezier(0.4,0,0.2,1)] md:flex ${
          collapsed ? 'w-[72px]' : 'w-64'
        }`}
      >
        <div className="flex h-[92px] shrink-0 items-center gap-3 overflow-hidden border-b border-panel-border px-[15px]">
          <img
            src="/icon-64.png"
            alt="Prospero Manager"
            className="h-12 w-12 shrink-0 rounded-xl border border-accent/40 shadow-[0_0_28px_-4px_var(--color-accent)]"
          />
          <div
            className="min-w-0 flex-1 overflow-hidden whitespace-nowrap transition-[opacity] duration-200"
            style={{ opacity: collapsed ? 0 : 1, transitionDelay: collapsed ? '0ms' : '120ms' }}
          >
            <div className="text-[15px] font-extrabold tracking-tight text-text">Prospero Manager</div>
          </div>
        </div>

        <nav className="pmgr-nav-list flex flex-1 flex-col gap-1 overflow-y-auto overflow-x-hidden px-2 py-4">
          {NAV.map((n, i) => (
            <div key={n.id}>
              {n.group !== NAV[i - 1]?.group && (
                collapsed ? (
                  // Collapsed rail has no room for a label, so the group becomes a
                  // separator rather than an empty block holding its margins.
                  <div className={`mx-3 border-t border-panel-border/70 ${i === 0 ? '' : 'mt-3'} mb-2`} />
                ) : (
                  <div
                    className="mb-1 mt-3 overflow-hidden whitespace-nowrap px-3 text-[11px] font-bold uppercase tracking-[0.14em] text-text-dim/70 transition-[opacity] duration-200 first:mt-1"
                    style={{ transitionDelay: '120ms' }}
                  >
                    {t(GROUP_LABEL_KEYS[n.group])}
                  </div>
                )
              )}
              <button
                onClick={() => open(n.id)}
                title={t(n.labelKey)}
                aria-current={active === n.id ? 'page' : undefined}
                className={`pmgr-nav-item flex min-h-12 w-full items-center gap-3 overflow-hidden rounded-xl px-3 text-sm font-semibold transition-colors ${
                  active === n.id ? 'is-active' : 'text-text-dim hover:bg-panel-border/70 hover:text-text'
                }`}
              >
                <span className="grid h-6 w-6 shrink-0 place-items-center">{ICONS[n.id]}</span>
                <span
                  className="overflow-hidden whitespace-nowrap transition-[opacity] duration-200"
                  style={{ opacity: collapsed ? 0 : 1, transitionDelay: collapsed ? '0ms' : '120ms' }}
                >
                  {t(n.labelKey)}
                </span>
              </button>
            </div>
          ))}
        </nav>

        <button
          onClick={() => setCollapsed((v) => !v)}
          title={collapsed ? t('nav.expand') : t('nav.collapse')}
          className="mx-2 mb-2 flex h-9 items-center gap-3 overflow-hidden rounded-xl px-3 text-text-dim transition-colors hover:bg-panel-border/70 hover:text-text"
        >
          <span className="grid h-6 w-6 shrink-0 place-items-center">
            <ChevronRightIcon size={15} className={`shrink-0 transition-transform duration-300 ${collapsed ? '' : 'rotate-180'}`} />
          </span>
          <span
            className="overflow-hidden whitespace-nowrap text-sm font-semibold transition-[opacity] duration-200"
            style={{ opacity: collapsed ? 0 : 1, transitionDelay: collapsed ? '0ms' : '120ms' }}
          >
            {t('nav.collapse')}
          </span>
        </button>

        <div
          className="overflow-hidden whitespace-nowrap border-t border-panel-border px-5 py-4 transition-[opacity] duration-200"
          style={{ opacity: collapsed ? 0 : 1, transitionDelay: collapsed ? '0ms' : '120ms' }}
        >
          <div className="text-xs leading-relaxed text-text-dim">
            {t('nav.madeWith')}
          </div>
        </div>
      </aside>

      <nav className="pmgr-mobile-nav fixed bottom-0 left-0 right-0 z-40 grid grid-cols-5 border-t border-panel-border px-2 md:hidden">
        {NAV.filter((n) => MOBILE_PRIMARY.has(n.id)).map((n) => (
          <button
            key={n.id}
            onClick={() => open(n.id)}
            aria-current={active === n.id ? 'page' : undefined}
            className={`pmgr-mobile-nav-item flex min-h-16 flex-col items-center justify-center gap-1 text-xs font-semibold ${
              active === n.id ? 'text-accent' : 'text-text-dim'
            }`}
          >
            {ICONS[n.id]}
            {t(n.mobileLabelKey || n.labelKey)}
          </button>
        ))}
        <button
          type="button"
          onClick={() => setMoreOpen(true)}
          aria-haspopup="dialog"
          aria-expanded={moreOpen}
          className={`pmgr-mobile-nav-item flex min-h-16 flex-col items-center justify-center gap-1 text-xs font-semibold ${
            moreActive || moreOpen ? 'text-accent' : 'text-text-dim'
          }`}
        >
          <svg width="19" height="19" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
            <circle cx="5" cy="12" r="1.7" /><circle cx="12" cy="12" r="1.7" /><circle cx="19" cy="12" r="1.7" />
          </svg>
          {t('nav.more')}
        </button>
      </nav>

      {moreOpen && (
        <MobileMoreSheet active={active} closing={moreClosing} onClose={closeMore} onOpen={open} t={t} />
      )}
    </>
  )
}
