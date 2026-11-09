// Small shared building blocks for the Settings tabs — kept separate from Panel.jsx since these
// are Settings-specific groupings. None animate on their own; Settings plays one entrance per tab switch.
import { useToast } from '../lib/ToastContext.jsx'
import { copyText } from '../lib/clipboard.js'
import { ClipboardIcon } from './icons.jsx'
import { useI18n } from '../lib/i18n.jsx'

export function SectionHero({ icon, title, description, status }) {
  return (
    <div className="pmgr-surface mb-4 flex flex-col gap-3 rounded-2xl border border-panel-border bg-panel p-4 sm:flex-row sm:items-center sm:justify-between sm:p-5">
      <div className="flex items-center gap-3">
        <span className="grid h-10 w-10 shrink-0 place-items-center rounded-xl bg-accent/10 text-accent">{icon}</span>
        <div className="min-w-0">
          <h2 className="text-base font-bold text-text">{title}</h2>
          {description && <p className="mt-0.5 max-w-xl text-sm text-text-dim">{description}</p>}
        </div>
      </div>
      {status && <div className="shrink-0 sm:pl-3">{status}</div>}
    </div>
  )
}

// The header-to-body divider lives here, not on the first SettingsRow — a row's own border
// only separates it from the *next* row, which would vanish for a single-row section (e.g. Danger Zone).
export function SettingsSection({ title, description, children, className = '' }) {
  return (
    <div className={`pmgr-surface mb-4 rounded-2xl border border-panel-border bg-panel p-4 sm:p-5 ${className}`}>
      {title && (
        <div className="mb-3 border-b border-panel-border pb-3">
          <h3 className="text-sm font-bold text-text">{title}</h3>
          {description && <p className="mt-0.5 text-xs text-text-dim">{description}</p>}
        </div>
      )}
      {children}
    </div>
  )
}

// Label + short explanation on the left, a single control on the right — the shape every
// toggle/fan-mode setting shares. Only separates rows; the section header divider covers the first one.
export function SettingsRow({ title, description, control }) {
  return (
    <div className="flex flex-col gap-2 border-b border-panel-border py-3 last:border-b-0 last:pb-0 sm:flex-row sm:items-center sm:justify-between sm:gap-4">
      <div className="min-w-0">
        <div className="text-sm font-medium text-text">{title}</div>
        {description && <div className="mt-0.5 text-xs text-text-dim">{description}</div>}
      </div>
      <div className="shrink-0">{control}</div>
    </div>
  )
}

export function MetricTile({ label, value, sub, loading = false }) {
  return (
    <div className="rounded-xl border border-panel-border bg-bg/45 p-3">
      <div className="text-xs font-bold uppercase tracking-wide text-text-dim">{label}</div>
      {loading ? (
        <div className="pmgr-skeleton mt-1.5 h-4 w-3/4 rounded" />
      ) : (
        <div className="mt-1 truncate text-sm font-semibold text-text" title={typeof value === 'string' ? value : undefined}>
          {value ?? '—'}
        </div>
      )}
      {sub && !loading && <div className="mt-0.5 truncate text-xs text-text-dim">{sub}</div>}
    </div>
  )
}

// Small inline copy affordance for paths/IDs in "Technical details" areas.
export function CopyButton({ text }) {
  const { t } = useI18n()
  const showToast = useToast()
  if (!text) return null
  return (
    <button
      type="button"
      onClick={() => copyText(text).then(() => showToast(t('toast.copied'))).catch(() => showToast(t('toast.copyFailed'), 'err'))}
      title={t('common.copy')}
      className="pmgr-icon-btn shrink-0 rounded-md p-1 text-text-dim hover:bg-panel-border hover:text-text"
    >
      <ClipboardIcon size={13} />
    </button>
  )
}

// A single technical-details line: label, monospace value, copy button.
export function CopyRow({ label, value }) {
  return (
    <div className="flex items-center justify-between gap-3 border-b border-panel-border py-1.5 last:border-b-0">
      <span className="shrink-0 text-text-dim">{label}</span>
      <span className="flex min-w-0 items-center gap-1.5">
        <code className="truncate rounded bg-bg px-1.5 py-0.5 text-xs text-text" title={value}>{value ?? '—'}</code>
        <CopyButton text={value} />
      </span>
    </div>
  )
}
