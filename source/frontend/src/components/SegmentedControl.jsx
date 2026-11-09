// Shared tab/filter visual language — "All / Running / Favorites", "All / PS5 / PS4", Settings
// nav all use the same selected/hover treatment instead of each screen building its own row.
export default function SegmentedControl({ options, value, onChange, className = '', label }) {
  return (
    <div role="group" aria-label={label} className={`inline-flex flex-wrap items-center gap-1 rounded-xl border border-panel-border bg-bg/45 p-1 ${className}`}>
      {options.map((opt) => (
        <button
          key={opt.id}
          type="button"
          aria-pressed={value === opt.id}
          onClick={() => onChange(opt.id)}
          className={`pmgr-seg grow rounded-lg px-3 py-1.5 text-xs font-semibold ${
            value === opt.id ? 'bg-accent text-white' : 'text-text-dim hover:bg-panel-border hover:text-text'
          }`}
        >
          {opt.label}
          {typeof opt.count === 'number' && <span className="ml-1.5 opacity-70">{opt.count}</span>}
        </button>
      ))}
    </div>
  )
}
