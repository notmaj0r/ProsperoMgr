import Button from './Button.jsx'
import Pill from './Pill.jsx'
import { CheckCircleIcon, XCircleIcon } from './icons.jsx'

export const KIND_KEYS = { base: 'pkg.kindBase', update: 'pkg.kindUpdate', dlc: 'pkg.kindDlc' }
export const KIND_TONES = { update: 'info', dlc: 'warn' }

// The live card: which package, which step, how far along, and how fast. A real percentage only
// where bytes are counted; steps with nothing to measure (the PS5's own validation) sweep instead.
export default function JobCard({ steps, activeIndex, item, label, pct, sub, stats, indeterminate, onCancel, cancelLabel, t }) {
  const info = item?.info
  const kindKey = info?.category ? KIND_KEYS[info.category] : null
  return (
    <div className="pmgr-surface mb-4 overflow-hidden rounded-2xl border border-accent/30 bg-accent/5 p-4 sm:p-5">
      <div className="mb-4 flex items-start justify-between gap-3">
        <div className="min-w-0">
          <div className="flex flex-wrap items-center gap-2">
            {kindKey && <Pill tone={KIND_TONES[info.category] || 'neutral'}>{t(kindKey)}</Pill>}
            <span className="min-w-0 truncate text-base font-bold text-text" dir="ltr">{info?.titleName || item?.name || label}</span>
          </div>
          {(info?.contentId || info?.version) && (
            <div className="pmgr-mono mt-1 truncate text-xs text-text-dim" dir="ltr">
              {[info.contentId, info.version && `v${info.version}`].filter(Boolean).join(' · ')}
            </div>
          )}
        </div>
        {!indeterminate && <div className="pmgr-num shrink-0 text-3xl font-extrabold leading-none text-accent">{Math.round(pct)}%</div>}
      </div>

      <div className="mb-4 flex items-center">
        {steps.map((step, i) => (
          <div key={step} className={`flex items-center ${i < steps.length - 1 ? 'flex-1' : ''}`}>
            <div className="flex items-center gap-2">
              <span
                className={`grid h-7 w-7 shrink-0 place-items-center rounded-full text-xs font-bold ${
                  i < activeIndex ? 'bg-ok text-white' : i === activeIndex ? 'bg-accent text-white ring-4 ring-accent/20' : 'bg-panel-border text-text-dim'
                }`}
              >
                {i < activeIndex ? <CheckCircleIcon size={15} /> : i + 1}
              </span>
              <span className={`text-xs font-bold ${i === activeIndex ? 'text-text' : 'text-text-dim'}`}>{step}</span>
            </div>
            {i < steps.length - 1 && <span className={`mx-3 h-0.5 flex-1 rounded-full ${i < activeIndex ? 'bg-ok' : 'bg-panel-border'}`} />}
          </div>
        ))}
      </div>

      <div className="h-3 overflow-hidden rounded-full bg-panel-border">
        <div
          role="progressbar"
          aria-label={label}
          aria-valuemin={0}
          aria-valuemax={100}
          aria-valuenow={indeterminate ? undefined : Math.round(pct)}
          className={`h-full rounded-full bg-accent transition-[width] duration-200 ${indeterminate ? 'pmgr-indeterminate' : ''}`}
          style={indeterminate ? undefined : { width: `${pct}%` }}
        />
      </div>

      <div className="mt-3 flex flex-wrap items-center justify-between gap-3">
        <div className="flex flex-wrap gap-2 text-xs">
          {(stats || []).map((stat) => (
            <span key={stat.label} className="rounded-full border border-panel-border bg-bg/40 px-2.5 py-1 text-text-dim">
              {stat.label} <span className="pmgr-num font-semibold text-text">{stat.value}</span>
            </span>
          ))}
          {sub && <span className="px-1 py-1 text-text-dim">{sub}</span>}
        </div>
        {onCancel && (
          <Button tone="danger" size="sm" icon={<XCircleIcon size={14} />} onClick={onCancel}>{cancelLabel}</Button>
        )}
      </div>
    </div>
  )
}


