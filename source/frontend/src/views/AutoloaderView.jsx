import { useEffect, useRef, useState } from 'react'
import Button from '../components/Button.jsx'
import Pill from '../components/Pill.jsx'
import Toggle from '../components/Toggle.jsx'
import SearchField from '../components/SearchField.jsx'
import CollapsibleSection from '../components/CollapsibleSection.jsx'
import { ConfirmDialog } from '../components/Dialog.jsx'
import { SectionHero, SettingsSection, SettingsRow, CopyRow } from '../components/SettingsKit.jsx'
import { ClockIcon, PowerIcon, LaunchIcon, AlertTriangleIcon, GripIcon, PlusIcon, CheckIcon, CloseIcon } from '../components/icons.jsx'
import { api } from '../lib/api.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'
import { usePolling } from '../lib/usePolling.js'
import AutoloaderLive from '../components/AutoloaderLive.jsx'
import { payloadOrigin } from '../lib/payloadOrigin.js'

function BundledAutostartToggle() {
  const { t } = useI18n()
  const [enabled, setEnabled] = useState(true)
  const [busy, setBusy] = useState(false)
  const showToast = useToast()

  useEffect(() => {
    api.autoloader.getBundledAutostart().then((r) => setEnabled(r.enabled)).catch(() => {})
  }, [])

  const toggle = () => {
    const next = !enabled
    setBusy(true)
    api.autoloader
      .setBundledAutostart(next)
      .then(() => {
        setEnabled(next)
        showToast(t(next ? 'settings.autostartOnToast' : 'settings.autostartOffToast'))
      })
      .catch((e) => showToast(e.message, 'err'))
      .finally(() => setBusy(false))
  }

  return (
    <SettingsRow
      title={t('settings.autostartBundledTitle')}
      description={t('settings.autostartBundledDesc')}
      control={<Toggle checked={enabled} onChange={toggle} disabled={busy} label={t('settings.autostartBundledTitle')} />}
    />
  )
}

const REVEAL_ON_HOVER = '[@media(hover:hover)]:opacity-0 [@media(hover:hover)]:group-hover:opacity-100 [@media(hover:hover)]:group-focus-within:opacity-100'

export default function AutoloaderView({ onNavigate }) {
  const { t } = useI18n()
  const [enabled, setEnabled] = useState(false)
  const [delay, setDelay] = useState(5)
  const [delayDraft, setDelayDraft] = useState('5')
  const [settleMs, setSettleMs] = useState(500)
  const [retries, setRetries] = useState(0)
  const [continueOnError, setContinueOnError] = useState(true)
  const [sequence, setSequence] = useState([])
  const [payloadList, setPayloadList] = useState([])
  const [addSearch, setAddSearch] = useState('')
  const [loadState, setLoadState] = useState('loading') // 'loading' | 'ready' | 'error'
  const [loadError, setLoadError] = useState('')
  const [status, setStatus] = useState(null)
  const [paths, setPaths] = useState(null)
  const [saveState, setSaveState] = useState('idle') // 'idle' | 'saving' | 'saved' | 'error'
  const [running, setRunning] = useState(false)
  const [confirmRunAgain, setConfirmRunAgain] = useState(false)
  const [error, setError] = useState('')
  const [draggingIndex, setDraggingIndex] = useState(-1)
  const [dragOverIndex, setDragOverIndex] = useState(-1)
  const dragIndexRef = useRef(-1)
  const showToast = useToast()

  // Autosave bookkeeping that must survive renders without causing one: whether the
  // first GET landed, the current and last-confirmed config, and any save in flight.
  const hydratedRef = useRef(false)
  const latestConfigRef = useRef(null)
  const savedKeyRef = useRef(null)
  const inFlightRef = useRef(null)
  const savedFadeRef = useRef(null)

  const currentConfig = { enabled, delay, settle_ms: settleMs, retries, continue_on_error: continueOnError, sequence }
  latestConfigRef.current = currentConfig
  const configKey = JSON.stringify(currentConfig)

  // Nothing is editable until the config and the payload list have both arrived, so a
  // late answer can never overwrite what the user just changed.
  const load = () => {
    setLoadState('loading')
    Promise.all([api.autoloader.getConfig(), api.payloads.list()])
      .then(([c, list]) => {
        hydratedRef.current = false // the state updates below are hydration, not a user edit
        setEnabled(c.enabled)
        setDelay(c.delay)
        setDelayDraft(String(c.delay))
        setSettleMs(c.settle_ms ?? 500)
        setRetries(c.retries ?? 0)
        setContinueOnError(c.continue_on_error ?? true)
        setSequence(c.sequence)
        setPayloadList(list.payloads)
        savedKeyRef.current = JSON.stringify({
          enabled: c.enabled, delay: c.delay, settle_ms: c.settle_ms ?? 500,
          retries: c.retries ?? 0, continue_on_error: c.continue_on_error ?? true, sequence: c.sequence,
        })
        setLoadState('ready')
        // Flip back only once the state above has committed, or the next render
        // autosaves the config we just loaded.
        window.setTimeout(() => { hydratedRef.current = true }, 0)
      })
      .catch((e) => {
        setLoadError(e.message)
        setLoadState('error')
      })
    api.autoloader.status().then(setStatus).catch(() => {})
  }

  useEffect(() => {
    load()
    api.paths().then(setPaths).catch(() => {})
  }, [])

  // Quick while it runs, so the countdown shows every second even on the console.
  usePolling(() => api.autoloader.status().then(setStatus).catch(() => {}), status?.running ? 350 : 1200)

  // Never more than one save in flight: overlapping requests can land out of order
  // and silently revert. A change during a save is picked up by its `finally`.
  const trySave = () => {
    if (inFlightRef.current) return inFlightRef.current
    const configToSave = latestConfigRef.current
    const keyBeingSaved = JSON.stringify(configToSave)
    if (keyBeingSaved === savedKeyRef.current) return Promise.resolve()

    setSaveState('saving')
    const promise = api.autoloader.setConfig(configToSave)
      .then(() => {
        savedKeyRef.current = keyBeingSaved
        setSaveState('saved')
        setError('')
        window.clearTimeout(savedFadeRef.current)
        savedFadeRef.current = window.setTimeout(() => setSaveState((s) => (s === 'saved' ? 'idle' : s)), 2000)
      })
      .catch((e) => {
        setSaveState('error')
        setError(e.message)
        throw e
      })
      .finally(() => {
        inFlightRef.current = null
        if (JSON.stringify(latestConfigRef.current) !== savedKeyRef.current) trySave()
      })
    inFlightRef.current = promise
    return promise
  }

  // Debounced autosave — waits for a quiet moment after the last edit
  // instead of saving on every keystroke/click.
  useEffect(() => {
    if (!hydratedRef.current) return
    const timer = window.setTimeout(() => { trySave().catch(() => {}) }, 550)
    return () => window.clearTimeout(timer)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [configKey])

  const commitDelay = (raw) => {
    const parsed = parseInt(raw, 10)
    // Clearing a field mid-edit must not autosave 0, so only a valid number
    // updates the saved value.
    if (Number.isFinite(parsed) && parsed >= 0) setDelay(Math.min(parsed, 300))
  }

  const [customDelay, setCustomDelay] = useState('1') // in seconds
  const [flashStep, setFlashStep] = useState(-1)

  const removeStep = (i) => setSequence((current) => current.filter((_, idx) => idx !== i))
  const moveStep = (i, dir) => {
    const j = i + dir
    setSequence((current) => {
      if (j < 0 || j >= current.length) return current
      const next = [...current]
      ;[next[i], next[j]] = [next[j], next[i]]
      return next
    })
  }
  const addLoad = (name) => setSequence((current) => [...current, { type: 'load', value: name }])
  const addDelay = (ms) => setSequence((current) => [...current, { type: 'delay', value: String(ms) }])

  const endDrag = () => {
    dragIndexRef.current = -1
    setDraggingIndex(-1)
    setDragOverIndex(-1)
  }

  const dropStep = (toIndex) => {
    const fromIndex = dragIndexRef.current
    if (fromIndex >= 0 && fromIndex !== toIndex) {
      setSequence((current) => {
        if (fromIndex >= current.length || toIndex >= current.length) return current
        const next = [...current]
        const [step] = next.splice(fromIndex, 1)
        next.splice(toIndex, 0, step)
        return next
      })
    }
    endDrag()
  }

  // Drop a queued payload from Available so it can't be added twice by a stray
  // double-click.
  const payloadNames = payloadList.map((p) => p.name)
  const payloadInfo = Object.fromEntries(payloadList.map((p) => [p.name, { version: p.installed_version, size: p.size_bytes, origin: payloadOrigin(p) }]))
  const disabledPayloadNames = payloadList.filter((p) => !p.enabled).map((p) => p.name)
  const usedLoadNames = new Set(sequence.filter((s) => s.type === 'load').map((s) => s.value))
  const available = payloadList.filter((p) => p.enabled && !usedLoadNames.has(p.name))
  const addQuery = addSearch.trim().toLocaleLowerCase()
  const shownAvailable = addQuery ? available.filter((p) => p.name.toLocaleLowerCase().includes(addQuery)) : available
  const missing = [...usedLoadNames].filter((name) => !payloadNames.includes(name))
  const disabledQueued = [...usedLoadNames].filter((name) => disabledPayloadNames.includes(name))
  const kstuffCount = sequence.filter((s) => s.type === 'load' && /kstuff/i.test(s.value)).length
  const hasEtaHen = sequence.some((s) => s.type === 'load' && /etahen/i.test(s.value))
  const firstStep = (match) => sequence.findIndex((step) => step.type === 'load' && match(step.value))
  // Each warning points at the step to fix, so it can take you there.
  const issues = [
    ...(missing.length ? [{ text: t('settings.issueMissingPayloads', { count: missing.length, list: missing.join(', ') }), step: firstStep((name) => missing.includes(name)) }] : []),
    ...(disabledQueued.length ? [{ text: t('settings.issueEnableBeforeAutoloading', { list: disabledQueued.join(', ') }), step: firstStep((name) => disabledQueued.includes(name)) }] : []),
    ...(kstuffCount > 1 ? [{ text: t('settings.issueMultipleKstuff'), step: firstStep((name) => /kstuff/i.test(name)) }] : []),
    ...(kstuffCount > 0 && hasEtaHen ? [{ text: t('settings.issueEtaHenKstuff'), step: firstStep((name) => /etahen/i.test(name)) }] : []),
    ...(sequence.every((s) => s.type !== 'load') ? [{ text: t('settings.issueNoPayload'), step: -1 }] : []),
  ]

  // Why a payload cannot be added right now, so the conflict is prevented instead of reported later.
  const conflictFor = (name) => {
    if (/kstuff/i.test(name)) {
      if (kstuffCount > 0) return t('settings.conflictKstuffTwice')
      if (hasEtaHen) return t('settings.conflictKstuffEta')
    }
    if (/etahen/i.test(name) && kstuffCount > 0) return t('settings.conflictEtaKstuff')
    return ''
  }

  const showStep = (index) => {
    if (index < 0) return
    document.getElementById(`autoloader-step-${index}`)?.scrollIntoView({ block: 'center' })
    setFlashStep(index)
    window.setTimeout(() => setFlashStep((current) => (current === index ? -1 : current)), 1600)
  }

  const secondsOf = (ms) => Number((Number(ms) / 1000).toFixed(1))
  const customSeconds = Number(customDelay)
  const customValid = customDelay !== '' && Number.isFinite(customSeconds) && customSeconds >= 0.5 && customSeconds <= 600

  // A manual run bypasses the startup guard and launches everything. Flushes any
  // pending autosave first and refuses to run if that save fails.
  const runSequence = async () => {
    setError('')
    if (JSON.stringify(latestConfigRef.current) !== savedKeyRef.current) {
      try {
        await trySave()
      } catch (e) {
        setError(e.message)
        return
      }
      // Covers a change that raced in right as the flush above resolved.
      if (JSON.stringify(latestConfigRef.current) !== savedKeyRef.current) {
        try {
          await trySave()
        } catch (e) {
          setError(e.message)
          return
        }
      }
    }
    setRunning(true)
    try {
      await api.autoloader.run()
      showToast(t('settings.sequenceStartedToast'))
      setStatus(await api.autoloader.status())
    } catch (e) {
      setError(e.message)
    } finally {
      setRunning(false)
    }
  }

  return (
    <div>
      <SectionHero
        icon={<PowerIcon size={18} />}
        title={t('nav.autoloader')}
        description={t('settings.autoloaderDesc')}
        status={
          <div className="flex flex-wrap items-center justify-end gap-2">
            {saveState !== 'idle' && (
              <span className="flex items-center gap-1.5 text-xs text-text-dim">
                {saveState === 'saving' && <><span className="pmgr-live-dot text-info" /> {t('settings.savingEllipsis')}</>}
                {saveState === 'saved' && <span className="inline-flex items-center gap-1 text-ok"><CheckIcon size={13} /> {t('settings.savedStatus')}</span>}
                {saveState === 'error' && (
                  <span className="flex items-center gap-1.5 text-err">
                    {t('settings.couldntSave')}
                    <button type="button" onClick={() => trySave().catch(() => {})} className="font-semibold underline underline-offset-2">
                      {t('common.retry')}
                    </button>
                  </span>
                )}
              </span>
            )}
            {status?.already_loaded && !status?.running && <Pill tone="ok">{t('settings.alreadyLoadedPill')}</Pill>}
            {loadState === 'ready' && (
              <Pill tone={status?.running ? 'info' : enabled ? 'ok' : 'neutral'} live={status?.running}>
                {status?.running ? t('common.running') : t(enabled ? 'settings.autoloaderPillEnabled' : 'settings.autoloaderPillDisabled')}
              </Pill>
            )}
          </div>
        }
      />

      <AutoloaderLive status={status} payloadInfo={payloadInfo} loadNames={sequence.filter((step) => step.type === 'load').map((step) => step.value)} onAbort={() => api.autoloader.abort().then(() => api.autoloader.status().then(setStatus))} />

      {error && <div className="pmgr-notice-in mb-4 rounded-xl border border-err/25 bg-err/10 px-3 py-2 text-sm text-err">{error}</div>}

      {loadState !== 'ready' ? (
        loadState === 'error' ? (
          <SettingsSection className="border-err/30">
            <div className="flex flex-wrap items-center justify-between gap-3">
              <div className="flex items-start gap-2 text-sm text-err" role="alert">
                <AlertTriangleIcon size={14} className="mt-0.5 shrink-0" />
                <span>{t('settings.autoloaderLoadFailed')}{loadError ? ` (${loadError})` : ''}</span>
              </div>
              <Button size="sm" tone="neutral" onClick={load}>{t('common.retry')}</Button>
            </div>
          </SettingsSection>
        ) : (
          <div className="space-y-3" aria-busy="true">
            <div className="pmgr-skeleton h-16 rounded-xl border border-panel-border bg-panel" />
            <div className="pmgr-skeleton h-56 rounded-xl border border-panel-border bg-panel" />
          </div>
        )
      ) : (
      <>
      <SettingsSection>
        <SettingsRow
          title={t('autoloader.rowTitle')}
          description={enabled ? t('autoloader.summaryOn', { delay }) : t('autoloader.summaryOff')}
          control={<Toggle checked={enabled} onChange={setEnabled} label={t('settings.autoloaderEnabledLabel')} />}
        />
      </SettingsSection>

      {issues.length > 0 && (
        <SettingsSection title={t('settings.sequenceNeedsAttention')} className="border-warn/30">
          <div className="space-y-2">
            {issues.map((issue) => (
              <div key={issue.text} className="flex flex-wrap items-start justify-between gap-2 text-sm text-warn">
                <span className="flex min-w-0 items-start gap-2">
                  <AlertTriangleIcon size={14} className="mt-0.5 shrink-0" />
                  <span>{issue.text}</span>
                </span>
                {issue.step >= 0 && <Button size="sm" tone="ghost" onClick={() => showStep(issue.step)}>{t('settings.showStep')}</Button>}
              </div>
            ))}
          </div>
        </SettingsSection>
      )}
      {!status?.running && status?.last_error && (
        <SettingsSection title={t('settings.lastRunFailed')} className="border-err/30">
          <div className="flex items-start gap-2 text-sm text-err">
            <AlertTriangleIcon size={14} className="mt-0.5 shrink-0" />
            <span>{status.last_error}</span>
          </div>
        </SettingsSection>
      )}

      <SettingsSection title={t('settings.sequenceTitle')} description={t('settings.sequenceDesc')}>
        <div className="mb-3 flex flex-wrap items-center justify-end gap-3">
          <span className="text-xs text-text-dim">{t('settings.runOnceHint')}</span>
          <Button
            tone="primary"
            sheen
            icon={<LaunchIcon size={14} />}
            onClick={() => (status?.already_loaded ? setConfirmRunAgain(true) : runSequence())}
            disabled={running || status?.running || issues.length > 0}
          >
            {status?.running ? t('settings.sequenceRunning') : t(status?.already_loaded ? 'settings.runSequenceAgain' : 'settings.runSequenceNow')}
          </Button>
        </div>
        <div className="grid grid-cols-1 gap-4 lg:grid-cols-2 lg:items-start">
        <div>
        {sequence.length === 0 ? (
          <div className="pmgr-empty-state py-8 text-center text-sm text-text-dim">
            <div className="font-semibold text-text">{t('settings.noStepsTitle')}</div>
            <div className="mt-1">{t('settings.noStepsDesc')}</div>
          </div>
        ) : (
          <div>
            {sequence.map((step, i) => {
              const isLoad = step.type === 'load'
              const invalid = isLoad && !payloadNames.includes(step.value)
              const disabledStep = isLoad && !invalid && disabledPayloadNames.includes(step.value)
              // Only load steps count towards the run's progress.
              const loadPosition = sequence.slice(0, i).filter((other) => other.type === 'load').length
              const runState = !isLoad ? '' : status?.last_error?.startsWith(`${step.value}:`) ? 'failed'
                : status?.running && loadPosition < status.done ? 'done'
                  : status?.running && status.current === step.value ? 'running' : ''
              return (
                <div key={i} id={`autoloader-step-${i}`}>
                  {/* A thin connector between steps — an ordered workflow,
                      not just an unordered list of rows. */}
                  {i > 0 && <div className="ml-[19px] h-2 w-px bg-panel-border" />}
                  <div
                    onDragOver={(event) => {
                      if (dragIndexRef.current < 0) return
                      event.preventDefault()
                      event.dataTransfer.dropEffect = 'move'
                      setDragOverIndex(i)
                    }}
                    onDrop={(event) => { event.preventDefault(); dropStep(i) }}
                    className={`pmgr-row-in group flex flex-wrap items-center gap-2.5 rounded-lg border px-2.5 py-2 ${
                      invalid ? 'border-err/30 bg-err/5' : disabledStep ? 'border-warn/30 bg-warn/5' : 'border-panel-border'
                    } ${flashStep === i ? 'ring-2 ring-accent' : ''} ${draggingIndex === i ? 'opacity-40' : ''} ${dragOverIndex === i && draggingIndex !== i ? 'border-t-2 border-t-accent bg-accent/[0.06]' : ''}`}
                    style={{ '--ti': i }}
                  >
                    <span
                      draggable="true"
                      role="img"
                      aria-label={t('settings.dragToReorder')}
                      title={t('settings.dragToReorder')}
                      onDragStart={(event) => {
                        dragIndexRef.current = i
                        setDraggingIndex(i)
                        setDragOverIndex(-1)
                        event.dataTransfer.effectAllowed = 'move'
                        event.dataTransfer.setData('text/plain', String(i))
                      }}
                      onDragEnd={endDrag}
                      className="-ml-1 grid h-7 w-5 shrink-0 cursor-grab place-items-center text-text-dim opacity-60 hover:opacity-100 active:cursor-grabbing"
                    >
                      <GripIcon size={15} />
                    </span>
                    <span className="grid h-5 w-5 shrink-0 place-items-center rounded-full bg-panel-border text-[10px] font-bold text-text-dim">
                      {i + 1}
                    </span>
                    <span className={`grid h-7 w-7 shrink-0 place-items-center rounded-lg ${
                      invalid ? 'bg-err/10 text-err' : disabledStep ? 'bg-warn/10 text-warn' : isLoad ? 'bg-info/10 text-info' : 'bg-panel-border text-text-dim'
                    }`}>
                      {isLoad ? <LaunchIcon size={13} /> : <ClockIcon size={13} />}
                    </span>
                    <span className="min-w-0 flex-[1_1_8rem]">
                      <span className="block truncate text-sm font-medium text-text" dir={isLoad ? 'ltr' : undefined}>{isLoad ? step.value : t('settings.waitSecondsStep', { seconds: secondsOf(step.value) })}</span>
                      {invalid && <span className="text-xs text-err">{t('settings.payloadNotFound')}</span>}
                      {disabledStep && <span className="text-xs text-warn">{t('settings.disabledEnableFirst')}</span>}
                    </span>
                    {runState === 'done' && <span className="inline-flex shrink-0 items-center gap-1 text-xs font-semibold text-ok"><CheckIcon size={13} />{t('common.done')}</span>}
                    {runState === 'running' && <span className="inline-flex shrink-0 items-center gap-1.5 text-xs font-semibold text-info"><span className="pmgr-live-dot" />{t('common.running')}</span>}
                    {runState === 'failed' && <span className="shrink-0 text-xs font-semibold text-err">{t('common.error')}</span>}
                    <span className="ml-auto flex shrink-0 items-center gap-1">
                      {/* With a mouse the reorder arrows appear on hover or focus; touch screens always show them. */}
                      <Button size="icon" tone="ghost" className={`${REVEAL_ON_HOVER} !h-9 !w-9`} title={t('settings.moveUp')} onClick={() => moveStep(i, -1)} disabled={i === 0}>↑</Button>
                      <Button size="icon" tone="ghost" className={`${REVEAL_ON_HOVER} !h-9 !w-9`} title={t('settings.moveDown')} onClick={() => moveStep(i, 1)} disabled={i === sequence.length - 1}>↓</Button>
                      <Button size="icon" tone="ghost" className="!h-9 !w-9 hover:!border-err/30 hover:!bg-err/10 hover:!text-err" title={t('common.remove')} onClick={() => removeStep(i)}><CloseIcon size={14} /></Button>
                    </span>
                  </div>
                </div>
              )
            })}
          </div>
        )}

        </div>

        <div className="mt-4 rounded-xl border border-dashed border-panel-border bg-bg/30 p-3 lg:mt-0">
          <div className="mb-2 text-xs font-bold uppercase tracking-wide text-text-dim">{t('autoloader.addStepTitle')}</div>
          <div className="mb-1 text-xs text-text-dim">{t('settings.addToSequenceTitle')}</div>
          {available.length > 8 && (
            <SearchField value={addSearch} onChange={setAddSearch} placeholder={t('payloads.searchPlaceholder')} className="mb-2 sm:max-w-64" />
          )}
          <div className="grid grid-cols-1 gap-1.5 sm:grid-cols-2">
            {shownAvailable.map((p) => {
              const conflict = conflictFor(p.name)
              return (
              <button
                key={p.name}
                type="button"
                onClick={() => addLoad(p.name)}
                disabled={Boolean(conflict)}
                className="pmgr-icon-btn flex min-w-0 items-center gap-2 rounded-lg border border-panel-border px-3 py-2 text-left transition-colors hover:border-accent/50 hover:bg-accent/10 disabled:cursor-not-allowed disabled:hover:border-panel-border disabled:hover:bg-transparent"
              >
                <span className="min-w-0 flex-1">
                  <span className="block truncate text-sm font-medium text-text" dir="ltr" title={p.name}>{p.name.replace(/\.(elf|bin|self)$/i, '')}</span>
                  {conflict ? (
                    <span className="block text-xs text-warn">{conflict}</span>
                  ) : (
                    <span className="block truncate text-xs text-text-dim">
                      {p.installed_version && <span className="pmgr-mono" dir="ltr">{p.installed_version} · </span>}
                      {t(`payloads.origin.${payloadOrigin(p)}`)}
                    </span>
                  )}
                </span>
                {!conflict && <PlusIcon size={14} />}
              </button>
              )
            })}
            {available.length === 0 && (
              <span className="text-xs text-text-dim">
                {t(payloadNames.length === 0 ? 'settings.noPayloadsInstalled' : 'settings.allPayloadsQueued')}
              </span>
            )}
            {available.length > 0 && shownAvailable.length === 0 && (
              <span className="text-xs text-text-dim">{t('payloads.noMatchTitle')}</span>
            )}
          </div>
          <button
            type="button"
            onClick={() => onNavigate?.('payloads')}
            className="mt-2 text-xs font-semibold text-accent hover:underline"
          >
            {t('autoloader.missingPayload')}
          </button>
          <div className="mb-1 mt-3 text-xs text-text-dim">{t('settings.addWaitTitle')}</div>
          <div className="flex flex-wrap items-center gap-1.5">
            {[1000, 3000, 5000].map((ms) => (
              <Button key={ms} size="sm" tone="neutral" icon={<ClockIcon size={13} />} onClick={() => addDelay(ms)}>{t('settings.addSeconds', { seconds: ms / 1000 })}</Button>
            ))}
            <input
              type="number"
              min="0.5"
              max="600"
              step="0.5"
              value={customDelay}
              onChange={(e) => setCustomDelay(e.target.value)}
              aria-label={t('settings.addWaitTitle')}
              aria-invalid={!customValid}
              className={`w-20 rounded-md border bg-bg px-2 py-1.5 text-xs text-text outline-none focus:border-accent ${customValid ? 'border-panel-border' : 'border-err/50'}`}
            />
            <span className="text-xs text-text-dim">{t('settings.seconds')}</span>
            <Button size="sm" tone="neutral" disabled={!customValid} onClick={() => addDelay(Math.round(customSeconds * 1000))}>
              {t('settings.addAction')}
            </Button>
          </div>
          {!customValid && <div className="mt-1 text-xs text-err">{t('settings.waitRange')}</div>}
        </div>
        </div>
        {issues.length > 0 && !status?.running && (
          <div className="mt-2 text-right text-xs text-text-dim">{t('settings.resolveIssuesFirst')}</div>
        )}
      </SettingsSection>

      <CollapsibleSection title={t('autoloader.advancedTitle')} defaultOpen={false}>
        <div>
        <SettingsRow
          title={t('settings.startupDelayTitle')}
          description={t('settings.startupDelayDesc')}
          control={
            <div className="flex items-center gap-2">
              <input
                type="number"
                min="0"
                value={delayDraft}
                onChange={(e) => { setDelayDraft(e.target.value); commitDelay(e.target.value) }}
                onBlur={() => setDelayDraft(String(delay))}
                className="w-16 rounded-md border border-panel-border bg-bg px-2 py-1.5 text-sm text-text outline-none focus:border-accent"
              />
              <span className="text-sm text-text-dim">{t('settings.seconds')}</span>
            </div>
          }
        />
        <SettingsRow
          title={t('settings.retriesTitle')}
          description={t('settings.retriesDesc')}
          control={
            <select value={retries} onChange={(e) => setRetries(Number(e.target.value))} className="rounded-md border border-panel-border bg-bg px-2 py-1.5 text-sm outline-none focus:border-accent">
              <option value={0}>{t('settings.none')}</option><option value={1}>1</option><option value={2}>2</option><option value={3}>3</option>
            </select>
          }
        />
        <SettingsRow
          title={t('settings.timeBetweenTitle')}
          description={t('settings.timeBetweenDesc')}
          control={
            <select value={settleMs} onChange={(e) => setSettleMs(Number(e.target.value))} className="rounded-md border border-panel-border bg-bg px-2 py-1.5 text-sm outline-none focus:border-accent">
              <option value={0}>{t('settings.none')}</option><option value={500}>0.5s</option><option value={1000}>1s</option><option value={2000}>2s</option>
            </select>
          }
        />
        <SettingsRow
          title={t('settings.continueOnFailureTitle')}
          description={t('settings.continueOnFailureDesc')}
          control={<Toggle checked={continueOnError} onChange={setContinueOnError} label={t('settings.continueOnFailureLabel')} />}
        />
        <BundledAutostartToggle />
        </div>
      </CollapsibleSection>

      {confirmRunAgain && (
        <ConfirmDialog
          title={t('settings.runAgainTitle')}
          description={t('settings.runAgainDesc')}
          confirmLabel={t('settings.runAgainConfirm')}
          onClose={() => setConfirmRunAgain(false)}
          onConfirm={runSequence}
        />
      )}

      <CollapsibleSection title={t('settings.technicalDetails')} defaultOpen={false}>
        <div className="text-sm">
          <CopyRow label={t('settings.copyAutoloaderConfigFile')} value={paths?.autoloader_ini} />
        </div>
      </CollapsibleSection>
      </>
      )}
    </div>
  )
}

