export default function Panel({ title, actions, children, className = '' }) {
  return (
    <div
      className={`pmgr-panel overflow-hidden rounded-lg border border-panel-border shadow-lg shadow-black/30 ${className}`}
    >
      {title && (
        <div className="flex flex-wrap items-center justify-between border-b border-panel-border px-4 py-3">
          <h3 className="text-sm font-semibold tracking-wide text-text">{title}</h3>
          {actions}
        </div>
      )}
      <div className="p-4">{children}</div>
    </div>
  )
}
