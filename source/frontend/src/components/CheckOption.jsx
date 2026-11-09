// A whole row you can click to turn an option on or off: box, label and a short hint.
export default function CheckOption({ checked, onChange, label, hint }) {
  return (
    <label
      className={`flex cursor-pointer items-start rounded-xl border px-3.5 py-3 transition-colors duration-150 ${
        checked ? 'border-accent/45 bg-accent/[0.07]' : 'border-panel-border bg-bg/30 hover:border-accent/30'
      }`}
    >
      <input type="checkbox" checked={checked} onChange={(e) => onChange(e.target.checked)} className="peer sr-only" />
      <span
        aria-hidden="true"
        className={`mt-0.5 grid h-[18px] w-[18px] shrink-0 place-items-center rounded-md border transition-colors duration-150 ${
          checked ? 'border-accent bg-accent text-white' : 'border-panel-border bg-bg text-transparent'
        }`}
      >
        <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="3" strokeLinecap="round" strokeLinejoin="round">
          <path d="M5 13l4 4L19 7" />
        </svg>
      </span>
      <span className="ml-3 min-w-0">
        <span className="block text-sm font-semibold leading-snug text-text">{label}</span>
        {hint && <span className="mt-0.5 block text-xs leading-relaxed text-text-dim">{hint}</span>}
      </span>
    </label>
  )
}
