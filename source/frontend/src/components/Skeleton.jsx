// Shimmering placeholder grid, swapped in for tile grids' initial load instead of a bare
// "Loading…" line. Pass the same grid-cols classes the real grid uses so it lines up.
export default function SkeletonGrid({ count = 6, gridClassName = 'grid-cols-2 gap-3 sm:grid-cols-3 md:grid-cols-4 xl:grid-cols-6', gridStyle }) {
  return (
    <div className={`grid ${gridClassName}`} style={gridStyle}>
      {Array.from({ length: count }, (_, i) => (
        <div key={i} className="overflow-hidden rounded-lg border border-panel-border">
          <div className="pmgr-skeleton aspect-square w-full" />
          <div className="space-y-2 p-3">
            <div className="pmgr-skeleton h-3 w-3/4" />
            <div className="pmgr-skeleton h-3 w-1/2" />
          </div>
        </div>
      ))}
    </div>
  )
}

// Same idea for a table/list — Files, Processes. A handful of shimmering
// rows instead of a plain "Loading…" line, shaped like the real rows.
export function SkeletonRows({ count = 6 }) {
  return (
    <div className="space-y-1.5">
      {Array.from({ length: count }, (_, i) => (
        <div key={i} className="flex items-center gap-3 rounded-lg border border-panel-border bg-bg/30 px-3 py-2.5">
          <div className="pmgr-skeleton h-8 w-8 shrink-0 rounded-lg" />
          <div className="min-w-0 flex-1 space-y-1.5">
            <div className="pmgr-skeleton h-3 w-1/3" />
            <div className="pmgr-skeleton h-2.5 w-1/5" />
          </div>
          <div className="pmgr-skeleton h-7 w-16 shrink-0 rounded-md" />
        </div>
      ))}
    </div>
  )
}
