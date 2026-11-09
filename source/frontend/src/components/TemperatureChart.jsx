import { useEffect, useRef, useState } from 'react'
import { useI18n } from '../lib/i18n.jsx'
import { formatDate } from '../lib/format.js'

const HEIGHT = 220
const PAD = { left: 42, right: 14, top: 16, bottom: 26 }
const RANGES = [5, 15, 60]
const SERIES = [
  { key: 'cpu', label: 'CPU', color: 'var(--color-info)' },
  { key: 'soc', label: 'SoC', color: 'var(--color-warn)' },
]

// Width of the element in pixels, so the SVG text keeps its real size on any screen.
function useWidth(ref, ready) {
  const [width, setWidth] = useState(0)
  useEffect(() => {
    const node = ref.current
    if (!node) return undefined
    const update = () => setWidth(node.clientWidth)
    update()
    if (typeof ResizeObserver === 'undefined') {
      window.addEventListener('resize', update)
      return () => window.removeEventListener('resize', update)
    }
    const observer = new ResizeObserver(update)
    observer.observe(node)
    return () => observer.disconnect()
  }, [ref, ready])
  return width
}

function niceStep(span) {
  return [2, 5, 10, 20, 25].find((step) => span / step <= 5) || 50
}

function summarize(samples, key) {
  let peak = samples[0]
  let low = samples[0]
  let sum = 0
  for (const sample of samples) {
    if (sample[key] >= peak[key]) peak = sample
    if (sample[key] < low[key]) low = sample
    sum += sample[key]
  }
  return { now: samples[samples.length - 1][key], peak: peak[key], peakAt: peak.t, low: low[key], avg: Math.round(sum / samples.length) }
}

function levelColor(c, warm, hot) {
  if (hot && c >= hot) return 'var(--color-err)'
  if (warm && c >= warm) return 'var(--color-warn)'
  return 'var(--color-text)'
}

// CPU and SoC history with a summary, a time range, labelled limits and a hover readout.
export default function TemperatureChart({ samples, warm, hot, toDisplay, unit }) {
  const { t, locale } = useI18n()
  const [range, setRange] = useState(60)
  const [hover, setHover] = useState(null)
  const frameRef = useRef(null)
  const width = useWidth(frameRef, samples.length >= 2)

  if (samples.length < 2) {
    return <div className="py-6 text-center text-xs text-text-dim">{t('settings.chartCollecting')}</div>
  }

  const end = samples[samples.length - 1].t
  const view = samples.filter((s) => s.t >= end - range * 60)
  const shown = view.length >= 2 ? view : samples
  const clock = (seconds) => formatDate(new Date(seconds * 1000), locale, { hour: '2-digit', minute: '2-digit' })
  const stats = Object.fromEntries(SERIES.map((series) => [series.key, summarize(shown, series.key)]))

  const values = shown.flatMap((s) => [s.cpu, s.soc])
  const top = Math.max(...values)
  let low = Math.floor((Math.min(...values) - 4) / 5) * 5
  let high = Math.ceil((top + 4) / 5) * 5
  // The limits only join the scale once the temperature gets close to them.
  for (const limit of [warm, hot]) {
    if (limit && top >= limit - 8) high = Math.max(high, Math.ceil((limit + 3) / 5) * 5)
  }
  if (high - low < 20) high = low + 20
  const step = niceStep(high - low)
  const ticks = []
  for (let tick = Math.ceil(low / step) * step; tick <= high; tick += step) ticks.push(tick)

  const plotW = Math.max(60, width - PAD.left - PAD.right)
  const plotH = HEIGHT - PAD.top - PAD.bottom
  const start = shown[0].t
  const span = Math.max(1, shown[shown.length - 1].t - start)
  const x = (time) => PAD.left + ((time - start) / span) * plotW
  const y = (temp) => PAD.top + (1 - (temp - low) / (high - low)) * plotH
  const line = (key) => shown.map((s) => `${x(s.t).toFixed(1)},${y(s[key]).toFixed(1)}`).join(' ')
  const area = (key) => `${PAD.left},${PAD.top + plotH} ${line(key)} ${x(shown[shown.length - 1].t).toFixed(1)},${PAD.top + plotH}`
  const limits = [[warm, 'var(--color-warn)', 'settings.chartWarmLimit'], [hot, 'var(--color-err)', 'settings.chartHotLimit']]
    .filter(([limit]) => limit && limit > low && limit < high)
  const timeTicks = (plotW < 340 ? [0, 0.5, 1] : [0, 1 / 3, 2 / 3, 1]).map((fraction) => start + span * fraction)

  const moveTo = (event) => {
    const box = frameRef.current.getBoundingClientRect()
    const time = start + ((event.clientX - box.left - PAD.left) / plotW) * span
    let nearest = 0
    shown.forEach((s, i) => { if (Math.abs(s.t - time) < Math.abs(shown[nearest].t - time)) nearest = i })
    setHover(nearest)
  }
  const hovered = hover !== null && hover < shown.length ? shown[hover] : null
  const summary = SERIES.map((series) => `${series.label} ${toDisplay(stats[series.key].peak)}°`).join(', ')

  return (
    <div>
      <div className="mb-4 grid grid-cols-1 gap-3 sm:grid-cols-2">
        {SERIES.map((series) => {
          const stat = stats[series.key]
          return (
            <div key={series.key} className="rounded-xl border border-panel-border bg-bg/45 p-3">
              <div className="flex items-center justify-between gap-3">
                <span className="inline-flex items-center gap-2 text-xs font-bold tracking-wide text-text-dim">
                  <span className="h-2.5 w-2.5 rounded-full" style={{ background: series.color }} />
                  {series.label}
                </span>
                <span className="pmgr-num text-2xl font-extrabold text-text">{toDisplay(stat.now)}°<span className="ml-1 text-xs font-semibold text-text-dim">{t('settings.chartNow')}</span></span>
              </div>
              <div className="mt-2 grid grid-cols-3 gap-2 text-xs">
                <div>
                  <div className="text-text-dim">{t('settings.chartPeak')}</div>
                  <div className="pmgr-num text-sm font-bold" style={{ color: levelColor(stat.peak, warm, hot) }}>{toDisplay(stat.peak)}°</div>
                  <div className="text-text-dim">{t('settings.chartAt', { time: clock(stat.peakAt) })}</div>
                </div>
                <div>
                  <div className="text-text-dim">{t('settings.chartAverage')}</div>
                  <div className="pmgr-num text-sm font-bold text-text">{toDisplay(stat.avg)}°</div>
                </div>
                <div>
                  <div className="text-text-dim">{t('settings.chartLowest')}</div>
                  <div className="pmgr-num text-sm font-bold text-text">{toDisplay(stat.low)}°</div>
                </div>
              </div>
            </div>
          )
        })}
      </div>

      <div className="mb-2 flex items-center justify-between gap-3">
        <span className="text-xs text-text-dim">°{unit}</span>
        <div role="group" aria-label={t('settings.chartRangeLabel')} className="inline-flex gap-1 rounded-lg border border-panel-border bg-bg/45 p-1">
          {RANGES.map((minutes) => (
            <button
              key={minutes}
              type="button"
              onClick={() => { setRange(minutes); setHover(null) }}
              aria-pressed={range === minutes}
              className={`rounded-md px-2.5 py-1 text-xs font-semibold transition-colors ${range === minutes ? 'bg-accent text-white' : 'text-text-dim hover:text-text'}`}
            >
              {minutes === 60 ? t('settings.chartRangeHour') : t('settings.chartRangeMinutes', { minutes })}
            </button>
          ))}
        </div>
      </div>

      <div ref={frameRef} className="relative w-full" style={{ height: HEIGHT }}>
        {width > 0 && (
          <svg
            width={width}
            height={HEIGHT}
            role="img"
            aria-label={`${t('settings.chartTitle')}: ${summary}`}
            style={{ touchAction: 'pan-y' }}
            onPointerMove={moveTo}
            onPointerLeave={() => setHover(null)}
          >
            <defs>
              {SERIES.map((series) => (
                <linearGradient key={series.key} id={`pmgr-temp-${series.key}`} x1="0" x2="0" y1="0" y2="1">
                  <stop offset="0%" style={{ stopColor: series.color }} stopOpacity="0.22" />
                  <stop offset="100%" style={{ stopColor: series.color }} stopOpacity="0" />
                </linearGradient>
              ))}
            </defs>
            {hot > low && hot < high && <rect x={PAD.left} y={PAD.top} width={plotW} height={y(hot) - PAD.top} opacity="0.08" style={{ fill: 'var(--color-err)' }} />}
            {ticks.map((tick) => (
              <g key={tick}>
                <line x1={PAD.left} x2={PAD.left + plotW} y1={y(tick)} y2={y(tick)} strokeWidth="1" style={{ stroke: 'var(--color-panel-border)' }} />
                <text x={PAD.left - 6} y={y(tick) + 4} textAnchor="end" fontSize="11" style={{ fill: 'var(--color-text-dim)' }}>{toDisplay(tick)}°</text>
              </g>
            ))}
            {timeTicks.map((time, i) => (
              <text key={time} x={x(time)} y={HEIGHT - 6} textAnchor={i === 0 ? 'start' : i === timeTicks.length - 1 ? 'end' : 'middle'} fontSize="11" style={{ fill: 'var(--color-text-dim)' }}>{clock(time)}</text>
            ))}
            {limits.map(([limit, color, label]) => (
              <g key={color}>
                <line x1={PAD.left} x2={PAD.left + plotW} y1={y(limit)} y2={y(limit)} strokeWidth="1" strokeDasharray="5 4" opacity="0.7" style={{ stroke: color }} />
                <text x={PAD.left + plotW - 4} y={y(limit) - 4} textAnchor="end" fontSize="11" fontWeight="600" style={{ fill: color }}>{t(label, { temp: `${toDisplay(limit)}°` })}</text>
              </g>
            ))}
            {SERIES.map((series) => (
              <polygon key={`${series.key}-area`} points={area(series.key)} fill={`url(#pmgr-temp-${series.key})`} />
            ))}
            {SERIES.map((series) => (
              <polyline key={series.key} fill="none" strokeWidth="2" strokeLinejoin="round" strokeLinecap="round" points={line(series.key)} style={{ stroke: series.color }} />
            ))}
            {SERIES.map((series) => {
              const peak = shown.reduce((best, s) => (s[series.key] >= best[series.key] ? s : best), shown[0])
              return (
                <g key={`${series.key}-peak`}>
                  <circle cx={x(peak.t)} cy={y(peak[series.key])} r="4" strokeWidth="2" style={{ fill: 'var(--color-panel)', stroke: series.color }} />
                </g>
              )
            })}
            {hovered && (
              <g pointerEvents="none">
                <line x1={x(hovered.t)} x2={x(hovered.t)} y1={PAD.top} y2={PAD.top + plotH} strokeWidth="1" style={{ stroke: 'var(--color-text-dim)' }} />
                {SERIES.map((series) => (
                  <circle key={series.key} cx={x(hovered.t)} cy={y(hovered[series.key])} r="4" style={{ fill: series.color }} />
                ))}
              </g>
            )}
          </svg>
        )}
        {hovered && width > 0 && (
          <div
            className="pointer-events-none absolute z-10 rounded-lg border border-panel-border bg-panel px-2.5 py-1.5 text-xs"
            style={{ left: Math.min(Math.max(x(hovered.t) + 10, 0), Math.max(0, width - 120)), top: 6 }}
          >
            <div className="font-semibold text-text">{clock(hovered.t)}</div>
            {SERIES.map((series) => (
              <div key={series.key} className="flex items-center justify-between gap-4 text-text-dim">
                <span className="inline-flex items-center gap-1.5">
                  <span className="h-2 w-2 rounded-full" style={{ background: series.color }} />
                  {series.label}
                </span>
                <span className="pmgr-num font-bold text-text">{toDisplay(hovered[series.key])}°</span>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  )
}
