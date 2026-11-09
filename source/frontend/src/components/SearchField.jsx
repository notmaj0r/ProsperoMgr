import { CloseIcon } from './icons.jsx'
import { useI18n } from '../lib/i18n.jsx'

// One search input shape reused across Files/Payloads/Processes/Repositories
// instead of each screen hand-rolling its own icon+input+clear markup.
export default function SearchField({ value, onChange, placeholder, className = '', inputClassName = '' }) {
  const { t } = useI18n()
  const effectivePlaceholder = placeholder ?? `${t('common.search')}…`
  return (
    <div className={`relative min-w-0 ${className}`}>
      <svg className="pointer-events-none absolute left-3 top-1/2 -translate-y-1/2 text-text-dim" width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" aria-hidden="true">
        <circle cx="11" cy="11" r="7" /><path d="m16.5 16.5 4 4" strokeLinecap="round" />
      </svg>
      <input
        value={value}
        onChange={(e) => onChange(e.target.value)}
        placeholder={effectivePlaceholder}
        aria-label={effectivePlaceholder}
        className={`min-h-10 w-full rounded-lg border border-panel-border bg-bg py-2 pl-9 pr-8 text-sm text-text outline-none transition focus:border-accent focus:ring-2 focus:ring-accent/10 ${inputClassName}`}
      />
      {value && (
        <button
          type="button"
          onClick={() => onChange('')}
          aria-label={t('common.clearSearch')}
          className="pmgr-icon-btn !absolute right-2 top-1/2 grid h-6 w-6 -translate-y-1/2 place-items-center rounded-md text-text-dim hover:bg-panel-border hover:text-text"
        >
          <CloseIcon size={13} />
        </button>
      )}
    </div>
  )
}
