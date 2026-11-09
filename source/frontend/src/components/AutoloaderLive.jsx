import { useEffect, useRef, useState } from 'react'
import { SettingsSection } from './SettingsKit.jsx'
import Button from './Button.jsx'
import { CheckIcon, CloseIcon, AlertTriangleIcon } from './icons.jsx'
import { useI18n } from '../lib/i18n.jsx'
import { formatFileSize } from '../lib/format.js'

const RESULT_MS = 12000
const shortName = (name) => (name || '').replace(/\.(elf|bin|self)$/i, '')
const seconds = (ms) => `${(ms / 1000).toFixed(1)} s`

const TONES = {
  info: { text: 'text-info', soft: 'bg-info/10', line: 'bg-info', edge: 'border-info/30' },
  done: { text: 'text-ok', soft: 'bg-ok/10', line: 'bg-ok', edge: 'border-ok/30' },
  failed: { text: 'text-warn', soft: 'bg-warn/10', line: 'bg-warn', edge: 'border-warn/30' },
  aborted: { text: 'text-text-dim', soft: 'bg-panel-2', line: 'bg-panel-border', edge: 'border-panel-border' },
}

// The dot on the rail: hollow while waiting, alive while loading, filled when done.
function Node({ state }) {
  if (state === 'done') {
    return <span className="pmgr-pop grid h-7 w-7 place-items-center rounded-full bg-ok text-white"><CheckIcon size={15} /></span>
  }
  if (state === 'failed') {
    return <span className="pmgr-pop grid h-7 w-7 place-items-center rounded-full bg-warn text-black"><AlertTriangleIcon size={14} /></span>
  }
  if (state === 'active') {
    return (
      <span className="relative grid h-7 w-7 place-items-center rounded-full bg-info/20">
        <span className="pmgr-live-dot text-info" />
      </span>
    )
  }
  return <span className="h-7 w-7 rounded-full border-2 border-panel-border bg-panel" />
}

function Step({ name, info, state, duration, last, locale, t }) {
  const meta = [info?.version, info?.size ? formatFileSize(info.size, locale) : '', info?.origin && t(`payloads.origin.${info.origin}`)].filter(Boolean)
  const status = { done: duration ? t('settings.autoloaderLoadedIn', { time: seconds(duration) }) : t('settings.autoloaderLoaded'), failed: t('common.error'), active: t('settings.autoloaderLoadingNow'), pending: t('settings.autoloaderQueued') }[state]
  const statusTone = { done: 'text-ok', failed: 'text-warn', active: 'text-info', pending: 'text-text-dim' }[state]
  return (
    <li className="flex">
      <div className="mr-3 flex flex-col items-center pt-2.5">
        <Node state={state} />
        {!last && <span className={`my-1 w-0.5 flex-1 rounded-full transition-colors duration-500 ${state === 'done' ? 'bg-ok/60' : 'bg-panel-border'}`} style={{ minHeight: '1.25rem' }} />}
      </div>
      <div className={`mb-2 min-w-0 flex-1 rounded-xl border px-3.5 py-2.5 transition-colors duration-300 ${state === 'active' ? 'border-info/40 bg-info/5' : 'border-transparent'}`}>
        <div className="flex items-center justify-between">
          <span className={`min-w-0 truncate text-base font-bold ${state === 'pending' ? 'text-text-dim' : 'text-text'}`}>{shortName(name)}</span>
          <span className={`ml-3 shrink-0 text-sm font-semibold ${statusTone}`}>{status}</span>
        </div>
        {meta.length > 0 && (
          <div className="mt-0.5 truncate text-xs text-text-dim">
            {meta.map((part, i) => <span key={i}>{i > 0 && ' · '}{i === 0 && info?.version ? <span className="pmgr-mono" dir="ltr">{part}</span> : part}</span>)}
          </div>
        )}
        {state === 'active' && <div className="pmgr-indeterminate mt-2 h-1 rounded-full bg-info/15" />}
      </div>
    </li>
  )
}

// One card for the whole run: a headline, a timeline with a step per payload and a progress line.
// It stays for a while after the run with the final state.
export default function AutoloaderLive({ status, loadNames, payloadInfo, onAbort }) {
  const { t, locale } = useI18n()
  const [startSeconds, setStartSeconds] = useState(0)
  const [final, setFinal] = useState(null)
  const [durations, setDurations] = useState([])
  const wasRunning = useRef(false)
  const stepStart = useRef(0)
  const lastDone = useRef(-1)

  const running = Boolean(status?.running)
  const counting = running && status.remaining_seconds > 0

  useEffect(() => {
    if (counting && status.remaining_seconds > startSeconds) setStartSeconds(status.remaining_seconds)
    if (!running) setStartSeconds(0)
  }, [counting, running, status?.remaining_seconds]) // eslint-disable-line react-hooks/exhaustive-deps

  // A step is timed from the moment the one before it finishes, as seen by the page's polling.
  useEffect(() => {
    if (!running) return
    if (counting) {
      setDurations([])
      lastDone.current = -1
      return
    }
    if (lastDone.current === -1) {
      lastDone.current = 0
      stepStart.current = performance.now()
    }
    if (status.done > lastDone.current) {
      const now = performance.now()
      const took = now - stepStart.current
      const first = lastDone.current
      const count = status.done - first
      setDurations((prev) => {
        const next = [...prev]
        for (let i = first; i < status.done; i++) next[i] = took / count
        return next
      })
      lastDone.current = status.done
      stepStart.current = now
    }
  }, [counting, running, status?.done]) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (wasRunning.current && !running && status) {
      const aborted = status.current === 'ABORTED' || !status.total
      setFinal({
        kind: aborted ? 'aborted' : status.failed > 0 || status.last_error ? 'failed' : 'done',
        done: status.done,
        total: status.total,
        error: status.last_error || '',
      })
    }
    if (running) setFinal(null)
    wasRunning.current = running
  }, [running]) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (!final) return undefined
    const timer = window.setTimeout(() => setFinal(null), RESULT_MS)
    return () => window.clearTimeout(timer)
  }, [final])

  if (!running && !final) return null

  const kind = running ? 'info' : final.kind
  const done = running ? status.done : final.done
  const total = running ? status.total : final.total
  const error = running ? '' : final.error
  const percent = kind === 'done' ? 100 : total ? Math.min(100, Math.round((done / total) * 100)) : 0
  const tone = TONES[kind]

  const stepState = (name, i) => {
    if (error.startsWith(`${name}:`)) return 'failed'
    if (kind === 'done' || i < done) return 'done'
    if (running && !counting && i === done) return 'active'
    return 'pending'
  }

  const title = running
    ? counting ? t('settings.autoloaderStarting', { seconds: status.remaining_seconds }) : t('settings.autoloaderLoading')
    : t(`settings.autoloader${kind === 'done' ? 'Done' : kind === 'failed' ? 'Failed' : 'Aborted'}Title`)
  const detail = running
    ? counting ? t('settings.autoloaderPressPs') : t('settings.stepOfTotal', { done: Math.min(done + 1, total), total })
    : kind === 'aborted' ? '' : t('settings.autoloaderDoneDesc', { done, total })
  const progress = counting ? (startSeconds ? (status.remaining_seconds / startSeconds) * 100 : 100) : percent

  return (
    <SettingsSection className={`relative overflow-hidden ${tone.edge}`}>
      <div className={`-mx-4 -mt-4 mb-4 flex flex-wrap items-center px-4 py-4 sm:-mx-5 sm:-mt-5 sm:px-5 ${tone.soft}`}>
        <div className={`mr-4 grid h-16 w-16 shrink-0 place-items-center rounded-2xl border border-panel-border bg-panel ${tone.text}`} aria-hidden="true">
          {running ? (
            <span key={counting ? status.remaining_seconds : 'run'} className="pmgr-tick pmgr-num text-2xl font-extrabold">{counting ? status.remaining_seconds : `${percent}%`}</span>
          ) : kind === 'done' ? <CheckIcon size={30} /> : kind === 'failed' ? <AlertTriangleIcon size={28} /> : <CloseIcon size={26} />}
        </div>
        <div className="mb-1 min-w-0 flex-1 pr-3" aria-live="polite">
          <div className="truncate text-xl font-extrabold text-text">{title}</div>
          {detail && <div className="mt-0.5 truncate text-sm text-text-dim">{detail}</div>}
          {kind === 'failed' && error && <div className="mt-0.5 truncate text-xs text-warn" dir="ltr">{error}</div>}
        </div>
        {running ? (
          <Button tone="danger" onClick={onAbort}>{t('settings.autoloaderAbort')}</Button>
        ) : (
          <button type="button" onClick={() => setFinal(null)} aria-label={t('common.close')} className="pmgr-icon-btn grid h-9 w-9 place-items-center rounded-full text-text-dim hover:text-text">
            <CloseIcon size={16} />
          </button>
        )}
      </div>

      {loadNames.length > 0 && (
        <ol className="list-none">
          {loadNames.map((name, i) => (
            <Step key={`${i}:${name}`} name={name} info={payloadInfo[name]} state={stepState(name, i)} duration={durations[i]} last={i === loadNames.length - 1} locale={locale} t={t} />
          ))}
        </ol>
      )}

      <div
        className="absolute bottom-0 left-0 h-1 w-full bg-panel-border"
        role="progressbar"
        aria-valuemin={0}
        aria-valuemax={100}
        aria-valuenow={Math.round(progress)}
      >
        <div className={`pmgr-run-bar h-full ${tone.line}`} style={{ width: `${progress}%` }} />
      </div>
    </SettingsSection>
  )
}
