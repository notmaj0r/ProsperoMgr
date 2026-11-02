import { useEffect, useMemo, useRef, useState } from 'react'
import { ICONS, NAV } from './NavRail.jsx'
import { useDismiss } from '../lib/useDismiss.js'
import { useDialogAccessibility } from '../lib/useDialogAccessibility.js'
import { useI18n } from '../lib/i18n.jsx'

const DESCRIPTION_KEYS = {
  home: 'commandPalette.descHome',
  ftp: 'commandPalette.descFiles',
  payloads: 'commandPalette.descPayloads',
  repository: 'commandPalette.descRepository',
  pkg: 'commandPalette.descPkg',
  saves: 'commandPalette.descSaves',
  storage: 'commandPalette.descStorage',
  processes: 'commandPalette.descProcesses',
  avatar: 'commandPalette.descAvatar',
  settings: 'commandPalette.descSettings',
  logs: 'commandPalette.descLogs',
}

const SETTINGS_COMMANDS = [
  { id: 'settings-autoloader', target: 'settings', tab: 'autoloader', labelKey: 'settings.tabAutoloader', descKey: 'settings.autoloaderDesc' },
  { id: 'settings-appearance', target: 'settings', tab: 'appearance', labelKey: 'settings.tabAppearance', descKey: 'settings.appearanceDesc' },
  { id: 'settings-thermal', target: 'settings', tab: 'thermal', labelKey: 'settings.tabThermal', descKey: 'settings.thermalDesc' },
  { id: 'settings-experimental', target: 'settings', tab: 'experimental', labelKey: 'settings.tabExperimental', descKey: 'settings.experimentalDesc' },
  { id: 'settings-about', target: 'settings', tab: 'about', labelKey: 'settings.tabAbout', descKey: 'settings.aboutDesc' },
]

export default function CommandPalette({ onClose, onNavigate }) {
  const { t, locale } = useI18n()
  const [query, setQuery] = useState('')
  const [selected, setSelected] = useState(0)
  const inputRef = useRef(null)
  const { closing, close } = useDismiss(onClose)
  const dialogRef = useDialogAccessibility(close, inputRef)

  // Search matches the translated labels currently visible on screen, not
  // the raw id or an English-only string — so it works in every locale.
  const items = useMemo(
    () => [
      ...NAV.map((item) => ({ id: item.id, target: item.id, label: t(item.labelKey), description: t(DESCRIPTION_KEYS[item.id]) })),
      ...SETTINGS_COMMANDS.map((item) => ({ ...item, label: t(item.labelKey), description: t(item.descKey) })),
    ],
    [t],
  )

  const results = useMemo(() => {
    const q = query.trim().toLocaleLowerCase(locale)
    if (!q) return items
    return items.filter((item) => `${item.label} ${item.description}`.toLocaleLowerCase(locale).includes(q))
  }, [items, query, locale])

  useEffect(() => setSelected(0), [query])

  const choose = (item) => {
    onNavigate(item.target, item.tab)
    close()
  }

  return (
    <div className={`pmgr-modal-overlay items-start pt-[12vh] ${closing ? 'is-closing' : ''}`} onClick={close}>
      <div
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-label={t('header.command')}
        tabIndex={-1}
        className={`pmgr-command pmgr-modal-pop w-full max-w-xl overflow-hidden rounded-2xl border border-panel-border bg-panel shadow-2xl ${closing ? 'is-closing' : ''}`}
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex items-center gap-3 border-b border-panel-border px-4">
          <svg className="shrink-0 text-text-dim" width="19" height="19" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8"><circle cx="11" cy="11" r="7" /><path d="m16.5 16.5 4 4" strokeLinecap="round" /></svg>
          <input
            ref={inputRef}
            value={query}
            onChange={(e) => setQuery(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'ArrowDown') {
                e.preventDefault()
                setSelected((value) => Math.min(results.length - 1, value + 1))
              }
              if (e.key === 'ArrowUp') {
                e.preventDefault()
                setSelected((value) => Math.max(0, value - 1))
              }
              if (e.key === 'Enter' && results[selected]) choose(results[selected])
            }}
            placeholder={t('commandPalette.placeholder')}
            role="combobox"
            aria-expanded="true"
            aria-controls="pmgr-command-results"
            aria-activedescendant={results[selected] ? `pmgr-command-${results[selected].id}` : undefined}
            className="h-14 min-w-0 flex-1 bg-transparent text-base text-text outline-none placeholder:text-text-dim"
          />
          <kbd className="rounded-md border border-panel-border bg-bg px-1.5 py-1 text-xs font-semibold text-text-dim">ESC</kbd>
        </div>

        <div id="pmgr-command-results" role="listbox" className="max-h-[55vh] overflow-y-auto p-2">
          {results.map((item, index) => (
            <button
              key={item.id}
              id={`pmgr-command-${item.id}`}
              type="button"
              role="option"
              aria-selected={selected === index}
              onMouseEnter={() => setSelected(index)}
              onClick={() => choose(item)}
              className={`flex w-full items-center gap-3 rounded-xl px-3 py-3 text-left ${selected === index ? 'bg-accent/15' : 'hover:bg-panel-2'}`}
            >
              <span className={`grid h-10 w-10 shrink-0 place-items-center rounded-xl border ${selected === index ? 'border-accent/30 bg-accent text-white' : 'border-panel-border bg-panel-2 text-text-dim'}`}>
                {ICONS[item.target]}
              </span>
              <span className="min-w-0">
                <span className="block text-sm font-bold text-text">{item.label}</span>
                <span className="block truncate text-xs text-text-dim">{item.description}</span>
              </span>
              <span className="ml-auto text-text-dim">→</span>
            </button>
          ))}
          {results.length === 0 && (
            <div className="px-4 py-10 text-center text-sm text-text-dim">{t('commandPalette.noResults', { query })}</div>
          )}
        </div>

        <div className="flex items-center gap-4 border-t border-panel-border px-4 py-2 text-xs text-text-dim">
          <span>↑↓ {t('commandPalette.navigate')}</span><span>↵ {t('commandPalette.openItem')}</span><span className="ml-auto">{t('commandPalette.shortcutHint')}</span>
        </div>
      </div>
    </div>
  )
}
