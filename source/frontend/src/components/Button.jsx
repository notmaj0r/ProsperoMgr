// Shared action button — every view was hand-rolling its own tiny text-xs/px-2.5/py-1
// button className, which is why they all felt cramped. Bigger default touch target, optional icon.

const TONES = {
  primary: 'pmgr-btn-primary border border-accent/60 bg-accent text-white hover:bg-accent-hover',
  neutral: 'border border-panel-border bg-panel-2 text-text hover:border-accent/35 hover:bg-panel-border',
  danger: 'border border-err/25 bg-err/10 text-err hover:border-err/45 hover:bg-err/20',
  warn: 'border border-warn/60 bg-warn text-black shadow-[0_8px_22px_-12px_var(--color-warn)] hover:brightness-105',
  ok: 'border border-ok/25 bg-ok/10 text-ok hover:border-ok/45 hover:bg-ok/20',
  ghost: 'border border-transparent text-text-dim hover:border-panel-border hover:bg-panel-border hover:text-text',
}

const SIZES = {
  // Icon-only, for tight row actions (table rows, per-item toolbars) — 44px
  // touch target, matching Checkbox's.
  icon: 'h-11 w-11 p-2.5',
  sm: 'min-h-9 px-3 py-1.5 text-xs gap-1.5',
  md: 'min-h-10 px-3.5 py-2 text-sm gap-2',
}

export default function Button({
  icon,
  children,
  tone = 'neutral',
  size = 'md',
  sheen = false,
  className = '',
  title,
  'aria-label': ariaLabel,
  ...props
}) {
  return (
    <button
      type="button"
      title={title}
      aria-label={ariaLabel || (size === 'icon' ? title : undefined)}
      className={`pmgr-button inline-flex shrink-0 items-center justify-center rounded-lg font-semibold disabled:pointer-events-none disabled:opacity-40 ${
        size === 'icon' ? 'pmgr-icon-btn' : ''
      } ${sheen ? 'pmgr-btn-sheen' : ''} ${TONES[tone]} ${SIZES[size]} ${className}`}
      {...props}
    >
      {icon}
      {children}
    </button>
  )
}
