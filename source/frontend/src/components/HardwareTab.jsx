import { useState } from 'react'
import { api } from '../lib/api.js'
import { useI18n } from '../lib/i18n.jsx'
import { usePolling } from '../lib/usePolling.js'
import { formatFileSize } from '../lib/format.js'

const MB = 1024 * 1024
const HISTORY = 60

const formatClock = (mhz) => (mhz >= 1000 ? `${(mhz / 1000).toFixed(2)} GHz` : `${mhz} MHz`)
const toneOf = (percent) => (percent >= 93 ? 'text-err' : percent >= 80 ? 'text-warn' : 'text-accent')
const percentOf = (used, total) => Math.max(0, Math.min(100, Math.round((used / total) * 100)))

// Circular gauge for a 0-100 value.
function Ring({ percent }) {
  const r = 26
  const c = 2 * Math.PI * r
  return (
    <div className="relative h-16 w-16 shrink-0">
      <svg width="64" height="64" viewBox="0 0 64 64" className={toneOf(percent)} aria-hidden="true">
        <circle cx="32" cy="32" r={r} fill="none" stroke="currentColor" strokeOpacity="0.15" strokeWidth="6" />
        <circle
          cx="32" cy="32" r={r} fill="none" stroke="currentColor" strokeWidth="6" strokeLinecap="round"
          strokeDasharray={`${(c * percent) / 100} ${c}`} transform="rotate(-90 32 32)"
        />
      </svg>
      <div className="pmgr-num absolute left-0 top-0 grid h-16 w-16 place-items-center text-sm font-bold text-text">{percent}%</div>
    </div>
  )
}

// Recent readings as a filled line; `max` fixes the top of the scale.
function Sparkline({ values, max, className = 'text-accent' }) {
  if (values.length < 2) return <div className="h-10" />
  const step = 100 / (HISTORY - 1)
  const line = values.map((v, i) => `${(i * step).toFixed(2)},${(30 - (Math.min(v, max) / max) * 28).toFixed(2)}`).join(' ')
  return (
    <svg viewBox="0 0 100 30" preserveAspectRatio="none" className={`h-10 w-full ${className}`} aria-hidden="true">
      <polygon points={`0,30 ${line} ${((values.length - 1) * step).toFixed(2)},30`} fill="currentColor" fillOpacity="0.12" />
      <polyline points={line} fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinejoin="round" vectorEffect="non-scaling-stroke" />
    </svg>
  )
}

function StatCard({ label, value, sub, percent, children }) {
  return (
    <div className="pmgr-surface rounded-2xl border border-panel-border bg-panel p-4 sm:p-5">
      <div className="flex items-center justify-between">
        <div className="min-w-0">
          <div className="text-xs font-bold uppercase tracking-wide text-text-dim">{label}</div>
          <div className="pmgr-num mt-1 truncate text-3xl font-extrabold text-text" dir="ltr">{value}</div>
          {sub && <div className="mt-1 truncate text-sm text-text-dim">{sub}</div>}
        </div>
        {percent != null && <Ring percent={percent} />}
      </div>
      <div className="mt-3">{children}</div>
    </div>
  )
}

export default function HardwareTab() {
  const { t, locale } = useI18n()
  const [hw, setHw] = useState(null)
  const [failed, setFailed] = useState(false)
  const [history, setHistory] = useState({ load: [], gpu: [], ram: [] })

  usePolling(() => {
    api.hardware().then((r) => {
      setHw(r)
      setFailed(false)
      const push = (list, v) => (v >= 0 ? [...list, v].slice(-HISTORY) : list)
      const ram = r.ram_total_mb > 0 ? percentOf(r.ram_used_mb, r.ram_total_mb) : -1
      setHistory((h) => ({ load: push(h.load, r.load_percent), gpu: push(h.gpu, r.gpu_mhz), ram: push(h.ram, ram) }))
    }).catch(() => setFailed(true))
  }, 3000)

  if (!hw) return failed ? <p className="text-sm text-text-dim">{t('hardware.unavailable')}</p> : <div className="pmgr-skeleton h-48 rounded-2xl" />

  const ramPercent = hw.ram_total_mb > 0 ? percentOf(hw.ram_used_mb, hw.ram_total_mb) : null
  const loads = (hw.cpu_load_percent || []).filter((load) => load >= 0)
  const clocks = [
    hw.memory_mhz > 0 && [t('hardware.memoryClock'), hw.memory_mhz],
    hw.fabric_mhz > 0 && [t('hardware.fabricClock'), hw.fabric_mhz],
  ].filter(Boolean)
  const showCpu = hw.cpu_mhz > 0
  const showGpu = hw.gpu_mhz > 0
  const showMemory = ramPercent !== null
  if (!showCpu && !showGpu && !showMemory && !loads.length && !clocks.length) {
    return <p className="text-sm text-text-dim">{t('hardware.unavailable')}</p>
  }

  const cards = [showCpu, showGpu, showMemory].filter(Boolean).length
  return (
    <div className="space-y-4">
      <div className={`grid grid-cols-1 gap-4 ${cards === 3 ? 'lg:grid-cols-3' : cards === 2 ? 'md:grid-cols-2' : ''}`}>
        {showCpu && (
          <StatCard
            label="CPU"
            value={formatClock(hw.cpu_mhz)}
            sub={hw.load_percent >= 0 ? `${t('hardware.load')} ${hw.load_percent}%` : undefined}
            percent={hw.load_percent >= 0 ? hw.load_percent : null}
          >
            <Sparkline values={history.load} max={100} className={toneOf(hw.load_percent)} />
          </StatCard>
        )}
        {showGpu && (
          <StatCard
            label="GPU"
            value={formatClock(hw.gpu_mhz)}
          >
            <Sparkline values={history.gpu} max={Math.max(hw.gpu_mhz, ...history.gpu, 1) * 1.1} />
          </StatCard>
        )}
        {showMemory && (
          <StatCard
            label={t('home.memory')}
            value={`${ramPercent}%`}
            sub={t('home.memoryOf', { used: formatFileSize(hw.ram_used_mb * MB, locale), total: formatFileSize(hw.ram_total_mb * MB, locale) })}
            percent={ramPercent}
          >
            <Sparkline values={history.ram} max={100} className={toneOf(ramPercent)} />
          </StatCard>
        )}
      </div>

      {loads.length > 0 && (
        <div className="pmgr-surface rounded-2xl border border-panel-border bg-panel p-4 sm:p-5">
          <div className="mb-3 text-xs font-bold uppercase tracking-wide text-text-dim">{t('hardware.cores')}</div>
          <div className="grid grid-cols-4 gap-3 sm:grid-cols-8">
            {loads.map((load, i) => (
              <div key={i}>
                <div className="flex h-24 flex-col justify-end overflow-hidden rounded-lg bg-panel-border">
                  <div className={`w-full rounded-lg transition-[height] duration-300 bg-current ${toneOf(load)}`} style={{ height: `${Math.max(load, 2)}%` }} />
                </div>
                <div className="pmgr-num mt-1 text-center text-xs font-semibold text-text">{load}%</div>
                <div className="text-center text-[11px] text-text-dim">{i}</div>
              </div>
            ))}
          </div>
        </div>
      )}

      {clocks.length > 0 && (
        <div className="grid grid-cols-2 gap-4">
          {clocks.map(([label, mhz]) => (
            <div key={label} className="pmgr-surface rounded-2xl border border-panel-border bg-panel p-4">
              <div className="text-xs font-bold uppercase tracking-wide text-text-dim">{label}</div>
              <div className="pmgr-num mt-1 text-xl font-bold text-text" dir="ltr">{formatClock(mhz)}</div>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
