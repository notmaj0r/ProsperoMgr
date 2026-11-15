const TONES = {
  neutral: 'bg-panel-border text-text-dim',
  accent: 'bg-accent/15 text-accent',
  // "ok" is reserved for real status (running, installed, signed-in), so green
  // doesn't leak onto labels that aren't a status.
  ok: 'bg-ok/15 text-ok',
  warn: 'bg-warn/15 text-warn',
  err: 'bg-err/15 text-err',
  info: 'bg-info/15 text-info',
}

// `live` adds a pulsing dot — used for "reachable"/"running" status pills.
export default function Pill({ tone = 'neutral', live = false, children }) {
  return (
    <span className={`pmgr-mono inline-flex items-center gap-1.5 rounded-full px-2.5 py-1 text-xs font-semibold ${TONES[tone]}`}>
      {live && <span className="pmgr-live-dot" />}
      {children}
    </span>
  )
}
