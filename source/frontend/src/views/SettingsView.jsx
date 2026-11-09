import { useEffect, useState } from 'react'
import EmojiText from '../components/EmojiText.jsx'
import UpdateDialog from '../components/UpdateDialog.jsx'
import TemperatureChart from '../components/TemperatureChart.jsx'
import HardwareTab from '../components/HardwareTab.jsx'
import { useUpdate } from '../lib/useUpdate.js'
import Panel from '../components/Panel.jsx'
import Button from '../components/Button.jsx'
import Pill from '../components/Pill.jsx'
import Toggle from '../components/Toggle.jsx'
import CollapsibleSection from '../components/CollapsibleSection.jsx'
import { ConfirmDialog } from '../components/Dialog.jsx'
import PathPickerModal from '../components/PathPickerModal.jsx'
import { SectionHero, SettingsSection, SettingsRow, MetricTile, CopyRow } from '../components/SettingsKit.jsx'
import {
  SlidersIcon, PaletteIcon, FlaskIcon, InfoIcon, ThermometerIcon, MoonIcon, SunIcon, YoutubeIcon,
  ExternalIcon, GithubIcon, ShieldIcon, PowerIcon, UserIcon, AlertTriangleIcon,
  FolderIcon, PlusIcon, DeleteIcon, CpuIcon,
} from '../components/icons.jsx'
import { EFFECTS_KEY, applyEffects, isPS5, loadReducedEffects } from '../lib/effects.js'
import { api } from '../lib/api.js'
import { JOBS_AUTOCLEAR_KEY, JOBS_AUTOCLEAR_OPTIONS, loadJobsAutoClear } from '../lib/jobsAutoClear.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'
import { usePolling } from '../lib/usePolling.js'
import { formatFileSize, volumeLabel} from '../lib/format.js'
import { SkeletonRows } from '../components/Skeleton.jsx'

// Third-party lookup tool for the offline-activation help panel — returns a decimal
// Account ID, which still needs converting to hex before it's usable here.
const PSN_ACCOUNT_CHECKER_URL = 'https://www.psntools.com/psn/checker'

// BigInt for the 64-bit wraparound multiply — only used to pre-fill a
// suggested account_id for a never-activated slot; the field stays fully editable.
function suggestAccountId(name) {
  const PRIME = BigInt(0x100000001b3)
  const MASK = BigInt('0xffffffffffffffff')
  let base = BigInt(0)
  for (let i = 0; i < name.length; i++) {
    base = (PRIME * (base ^ BigInt(name.charCodeAt(i)))) & MASK
  }
  return '0x' + base.toString(16).padStart(16, '0')
}

function tempStatusWord(c, warm, hot, t) {
  if (c == null || c < 0) return '—'
  if (c >= hot) return t('settings.tempHot')
  if (c >= warm) return t('settings.tempWarm')
  return t('settings.tempNormal')
}

const FAN_UNIT_KEY = 'pmgr_fan_unit'
const toF = (c) => Math.round((c * 9) / 5 + 32)
const fmtTemp = (c, fahrenheit) => (c == null || c < 0 ? '—' : `${fahrenheit ? toF(c) : c}°${fahrenheit ? 'F' : 'C'}`)

// ok/warn/err as real colors (not just text classes) — the gauge ring and
// the bar fills need a stroke/background color, not a Tailwind text-* class.
function tempColor(c, warm, hot) {
  if (c == null || c < 0) return 'var(--color-text-dim)'
  if (c >= hot) return 'var(--color-err)'
  if (c >= warm) return 'var(--color-warn)'
  return 'var(--color-ok)'
}

function clampPct(value, min, max) {
  if (value == null || value < 0 || max <= min) return 0
  return Math.max(0, Math.min(100, ((value - min) / (max - min)) * 100))
}

// Circular gauge for the hottest sensor, so how close it sits to the warm/hot zone reads at a glance.
function TempGauge({ label, c, min, max, warm, hot, fahrenheit, size = 116 }) {
  const stroke = 9
  const r = (size - stroke) / 2
  const circumference = 2 * Math.PI * r
  const pct = clampPct(c, min, max)
  const color = tempColor(c, warm, hot)
  return (
    <div className="relative grid shrink-0 place-items-center" style={{ width: size, height: size }}>
      <svg width={size} height={size} className="-rotate-90">
        <circle cx={size / 2} cy={size / 2} r={r} fill="none" stroke="var(--color-panel-border)" strokeWidth={stroke} />
        <circle
          cx={size / 2}
          cy={size / 2}
          r={r}
          fill="none"
          stroke={color}
          strokeWidth={stroke}
          strokeLinecap="round"
          strokeDasharray={circumference}
          strokeDashoffset={circumference * (1 - pct / 100)}
          style={{ transition: 'stroke-dashoffset 0.6s ease, stroke 0.3s ease' }}
        />
      </svg>
      <div className="absolute flex flex-col items-center">
        <span className="pmgr-mono text-xl font-extrabold" style={{ color }}>
          {fmtTemp(c, fahrenheit)}
        </span>
        <span className="text-[11px] font-semibold uppercase tracking-wide text-text-dim">{label}</span>
      </div>
    </div>
  )
}

// Slim horizontal readout for a single sensor — position within min..max,
// color-coded, plus a text status word so the state never depends on color alone.
function TempBar({ label, c, min, max, warm, hot, fahrenheit, t }) {
  const pct = clampPct(c, min, max)
  const color = tempColor(c, warm, hot)
  return (
    <div className="rounded-xl border border-panel-border bg-bg/45 px-3 py-2.5">
      <div className="mb-1.5 flex items-center justify-between">
        <span className="text-xs font-semibold uppercase tracking-wide text-text-dim">{label}</span>
        <span className="pmgr-mono text-sm font-bold" style={{ color }}>{fmtTemp(c, fahrenheit)}</span>
      </div>
      <div className="h-1.5 overflow-hidden rounded-full bg-panel-border">
        <div className="h-full rounded-full" style={{ width: `${pct}%`, background: color, transition: 'width 0.6s ease, background 0.3s ease' }} />
      </div>
      <div className="mt-1 text-right text-[11px] font-semibold" style={{ color }}>{tempStatusWord(c, warm, hot, t)}</div>
    </div>
  )
}

function ThermalPanel() {
  const { t } = useI18n()
  const [thermal, setThermal] = useState(null)
  const [history, setHistory] = useState([])
  const [target, setTarget] = useState(70)
  const [adjusting, setAdjusting] = useState(false)
  const [busy, setBusy] = useState(false)
  const [fahrenheit, setFahrenheit] = useState(() => window.localStorage.getItem(FAN_UNIT_KEY) === 'F')
  const showToast = useToast()

  const load = () => {
    api.thermal.history().then((r) => setHistory(r.samples || [])).catch(() => {})
    return api.thermal.status().then(setThermal).catch(() => setThermal({ ok: false }))
  }

  usePolling(load, 4000)

  useEffect(() => {
    if (thermal?.threshold_c && !adjusting) setTarget(thermal.threshold_c)
  }, [thermal?.threshold_c, adjusting])

  const toggleUnit = () => {
    const next = !fahrenheit
    setFahrenheit(next)
    window.localStorage.setItem(FAN_UNIT_KEY, next ? 'F' : 'C')
  }

  const pin = () => {
    setBusy(true)
    api.thermal
      .setFan(target)
      .then(() => {
        showToast(t('settings.fanPinnedToast', { temp: fmtTemp(target, fahrenheit) }))
        setAdjusting(false)
        load()
      })
      .catch((e) => showToast(e.message, 'err'))
      .finally(() => setBusy(false))
  }

  const reset = () => {
    setBusy(true)
    api.thermal
      .resetFan()
      .then(() => {
        showToast(t('settings.fanAutoToast'))
        setAdjusting(false)
        load()
      })
      .catch((e) => showToast(e.message, 'err'))
      .finally(() => setBusy(false))
  }

  return (
    <div>
      <SectionHero
        icon={<ThermometerIcon size={18} />}
        title={t('settings.tabThermal')}
        description={t('settings.thermalDesc')}
        status={
          thermal?.ok && (
            <div className="flex items-center gap-2">
              <span className="flex items-center gap-1.5 text-xs text-text-dim">
                <span className="pmgr-live-dot text-info" />
                {t('settings.live')}
              </span>
              <Button size="sm" tone={fahrenheit ? 'primary' : 'neutral'} onClick={toggleUnit}>
                °{fahrenheit ? 'F' : 'C'}
              </Button>
            </div>
          )
        }
      />

      {!thermal ? (
        <div className="mb-4 flex flex-col gap-4 rounded-2xl border border-panel-border bg-panel p-4 sm:flex-row sm:items-center sm:p-5">
          <div className="pmgr-skeleton h-28 w-28 shrink-0 rounded-full" />
          <div className="flex-1 space-y-2">
            <div className="pmgr-skeleton h-4 w-28 rounded" />
            <div className="pmgr-skeleton h-3 w-full max-w-xs rounded" />
          </div>
        </div>
      ) : !thermal.ok ? (
        <div className="pmgr-empty-state py-10 text-center text-sm text-text-dim">
          <div className="mx-auto mb-3 grid h-12 w-12 place-items-center rounded-2xl border border-panel-border bg-panel-2 text-text-dim">
            <ThermometerIcon size={22} />
          </div>
          <div className="font-semibold text-text">{t('settings.thermalUnavailableTitle')}</div>
          <div className="mt-1">{t('settings.thermalUnavailableDesc')}</div>
        </div>
      ) : (
        <>
          <div className="mb-4 flex flex-col gap-4 rounded-2xl border border-panel-border bg-panel p-4 sm:flex-row sm:items-center sm:p-5">
            <div className="flex justify-center sm:border-r sm:border-panel-border sm:pr-5">
              <TempGauge c={thermal.hottest_c} min={thermal.min_c} max={thermal.max_c} warm={thermal.warm_c} hot={thermal.hot_c} fahrenheit={fahrenheit} label={t('settings.hottest')} />
            </div>
            <div className="min-w-0 flex-1">
              <div className="text-xs font-bold uppercase tracking-wide text-text-dim">{t('settings.thermalStatus')}</div>
              <div className="mt-0.5 text-2xl font-extrabold" style={{ color: tempColor(thermal.hottest_c, thermal.warm_c, thermal.hot_c) }}>
                {tempStatusWord(thermal.hottest_c, thermal.warm_c, thermal.hot_c, t)}
              </div>
              <p className="mt-1 text-sm text-text-dim">{t('settings.thermalMonitoring')}</p>
              {thermal.fan_duty_percent >= 0 && (
                <p className="mt-1 text-sm text-text-dim">
                  {t('settings.fanDuty')} · <span className="pmgr-mono font-semibold text-text">{thermal.fan_duty_percent}%</span>
                </p>
              )}
              <div className="mt-3 grid grid-cols-1 gap-2 sm:grid-cols-3">
                <TempBar label="CPU" c={thermal.cpu_c} min={thermal.min_c} max={thermal.max_c} warm={thermal.warm_c} hot={thermal.hot_c} fahrenheit={fahrenheit} t={t} />
                <TempBar label="SoC" c={thermal.soc_c} min={thermal.min_c} max={thermal.max_c} warm={thermal.warm_c} hot={thermal.hot_c} fahrenheit={fahrenheit} t={t} />
                <TempBar label="M.2" c={thermal.m2_c} min={thermal.min_c} max={thermal.max_c} warm={thermal.warm_c} hot={thermal.hot_c} fahrenheit={fahrenheit} t={t} />
              </div>
            </div>
          </div>

          <SettingsSection title={t('settings.chartTitle')} description={t('settings.chartDesc')}>
            <TemperatureChart
              samples={history}
              warm={thermal.warm_c}
              hot={thermal.hot_c}
              toDisplay={(c) => (fahrenheit ? toF(c) : c)}
              unit={fahrenheit ? 'F' : 'C'}
            />
          </SettingsSection>

          <SettingsSection
            title={t('settings.coolingBehaviorTitle')}
            description={t('settings.coolingBehaviorDesc')}
          >
            <div className="mb-4 flex items-center gap-2 rounded-xl border border-panel-border bg-bg/45 p-1">
              <button
                onClick={reset}
                disabled={busy || !thermal.threshold_c}
                className={`flex-1 rounded-lg px-3 py-2 text-sm font-semibold transition-colors disabled:cursor-default ${
                  !thermal.threshold_c ? 'bg-accent text-white' : 'text-text-dim hover:text-text'
                }`}
              >
                {t('settings.automaticCooling')}
              </button>
              <div className={`flex-1 rounded-lg px-3 py-2 text-center text-sm font-semibold ${thermal.threshold_c ? 'bg-accent text-white' : 'text-text-dim'}`}>
                {t('settings.customThreshold')}{thermal.threshold_c ? ` · ${fmtTemp(thermal.threshold_c, fahrenheit)}` : ''}
              </div>
            </div>

            <div className="mb-3 grid grid-cols-3 gap-2">
              {[
                { id: 'cooler', labelKey: 'settings.presetCoolerLabel', value: 65, descKey: 'settings.presetCoolerDesc' },
                { id: 'balanced', labelKey: 'settings.presetBalancedLabel', value: 70, descKey: 'settings.presetBalancedDesc' },
                { id: 'quieter', labelKey: 'settings.presetQuieterLabel', value: 75, descKey: 'settings.presetQuieterDesc' },
              ].map((p) => (
                <button
                  key={p.id}
                  onClick={() => { setAdjusting(true); setTarget(p.value) }}
                  className={`rounded-lg border px-2.5 py-2 text-left text-xs transition-colors ${
                    target === p.value ? 'border-accent/60 bg-accent/10' : 'border-panel-border hover:border-accent/30'
                  }`}
                >
                  <div className="font-bold text-text">{t(p.labelKey)}</div>
                  <div className="text-text-dim">{t(p.descKey)}</div>
                </button>
              ))}
            </div>

            <div className="mb-1.5 flex items-center justify-between text-sm text-text">
              <span>{t('settings.customFanThreshold')}</span>
              <span className="pmgr-mono font-semibold" style={{ color: tempColor(target, thermal.warm_c, thermal.hot_c) }}>
                {fmtTemp(target, fahrenheit)}
              </span>
            </div>
            <input
              type="range"
              min={thermal.min_c}
              max={thermal.max_c}
              value={target}
              onChange={(e) => {
                setAdjusting(true)
                setTarget(parseInt(e.target.value, 10))
              }}
              className="mb-1.5 w-full"
              style={{ accentColor: tempColor(target, thermal.warm_c, thermal.hot_c) }}
            />
            <div className="mb-2 text-xs text-text-dim">
              {target <= (thermal.min_c + thermal.max_c) / 2 ? t('settings.prioritizeCooling') : t('settings.prioritizeQuiet')}
            </div>
            {target >= thermal.hot_c && (
              <div className="mb-3 flex items-start gap-2 rounded-lg border border-warn/25 bg-warn/5 px-3 py-2 text-xs text-warn">
                <AlertTriangleIcon size={13} className="mt-0.5 shrink-0" />
                <span>{t('settings.higherThresholdWarn')}</span>
              </div>
            )}

            <div className="flex flex-wrap items-center gap-2">
              <Button tone="primary" onClick={pin} disabled={busy}>
                {t(thermal.threshold_c ? 'settings.repinButton' : 'settings.pinButton', { temp: fmtTemp(target, fahrenheit) })}
              </Button>
              {adjusting && target !== thermal.threshold_c && <span className="text-xs text-text-dim">{t('settings.unsavedChange')}</span>}
            </div>
          </SettingsSection>
        </>
      )}
    </div>
  )
}

const THEME_KEY = 'pmgr_theme'

const THEMES = [
  { id: 'dark', labelKey: 'settings.themeDark', descKey: 'settings.themeDarkDesc', swatch: '#0b0e16', Icon: MoonIcon },
  { id: 'light', labelKey: 'settings.themeLight', descKey: 'settings.themeLightDesc', swatch: '#f4f7fb', Icon: SunIcon },
]

function ThemePanel() {
  const { t } = useI18n()
  // A saved theme that no longer exists falls back to dark, since it has no matching CSS.
  const [theme, setTheme] = useState(() => {
    const saved = window.localStorage.getItem(THEME_KEY)
    return THEMES.some((th) => th.id === saved) ? saved : 'dark'
  })

  useEffect(() => {
    // Brief window so the theme cross-fades instead of snapping. Removed right
    // after, so it costs nothing outside a switch.
    const root = document.documentElement
    const reduceMotion = window.matchMedia('(prefers-reduced-motion: reduce)').matches
    if (!reduceMotion) {
      root.classList.add('pmgr-theme-transition')
      setTimeout(() => root.classList.remove('pmgr-theme-transition'), 260)
    }
    if (theme === 'dark') delete root.dataset.pmgrTheme
    else root.dataset.pmgrTheme = theme
    window.localStorage.setItem(THEME_KEY, theme)
  }, [theme])

  return (
    <Panel title={t('settings.appearanceTitle')}>
      <p className="mb-3 text-sm text-text-dim">{t('settings.appearanceDesc')}</p>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        {THEMES.map((th) => (
          <button
            key={th.id}
            onClick={() => setTheme(th.id)}
            className={`pmgr-theme-choice relative overflow-hidden rounded-xl border p-3 text-left ${
              theme === th.id ? 'is-selected border-accent/60 bg-accent/10' : 'border-panel-border bg-bg/45 hover:border-accent/35'
            }`}
          >
            <span className="mb-3 block h-16 rounded-lg border border-panel-border" style={{ background: `linear-gradient(145deg, ${th.swatch}, ${th.id === 'dark' ? '#18243a' : '#dbeafe'})` }}>
              <span className="m-2 block h-2 w-1/2 rounded-full bg-accent/70" />
              <span className="mx-2 mt-2 block h-5 rounded border border-panel-border bg-panel/80" />
            </span>
            <span className="flex items-center gap-2 text-sm font-bold text-text"><th.Icon size={16} />{t(th.labelKey)}</span>
            <span className="mt-0.5 block text-xs text-text-dim">{t(th.descKey)}</span>
            {theme === th.id && <span className="absolute right-3 top-3 rounded-full bg-accent px-2 py-0.5 text-xs font-bold text-white">{t('settings.active')}</span>}
          </button>
        ))}
      </div>
    </Panel>
  )
}

const EFFECTS_OPTIONS = [
  { id: 'full', labelKey: 'settings.effectsFull', descKey: 'settings.effectsFullDesc' },
  { id: 'reduced', labelKey: 'settings.effectsReduced', descKey: 'settings.effectsReducedDesc' },
]

function VisualEffectsPanel() {
  const { t } = useI18n()
  const [mode, setMode] = useState(() => (loadReducedEffects() ? 'reduced' : 'full'))

  useEffect(() => {
    applyEffects(mode === 'reduced')
    window.localStorage.setItem(EFFECTS_KEY, mode)
  }, [mode])

  return (
    <Panel title={t('settings.visualEffectsTitle')}>
      <p className="mb-3 text-sm text-text-dim">{t('settings.visualEffectsDesc')}</p>
      <div className="grid grid-cols-1 gap-3 sm:grid-cols-2">
        {EFFECTS_OPTIONS.map((opt) => (
          <button
            key={opt.id}
            onClick={() => setMode(opt.id)}
            className={`pmgr-theme-choice relative overflow-hidden rounded-xl border p-3 text-left ${
              mode === opt.id ? 'is-selected border-accent/60 bg-accent/10' : 'border-panel-border bg-bg/45 hover:border-accent/35'
            }`}
          >
            <span className="text-sm font-bold text-text">{t(opt.labelKey)}</span>
            <span className="mt-0.5 block text-xs text-text-dim">{t(opt.descKey)}</span>
            {mode === opt.id && <span className="absolute right-3 top-3 rounded-full bg-accent px-2 py-0.5 text-xs font-bold text-white">{t('settings.active')}</span>}
          </button>
        ))}
      </div>
    </Panel>
  )
}

function storageBarClass(pct) {
  if (pct >= 93) return 'bg-err'
  if (pct >= 80) return 'bg-warn'
  return 'bg-accent'
}

function GeneralTab() {
  const { t, locale } = useI18n()
  const [status, setStatus] = useState(null)
  const [paths, setPaths] = useState(null)
  const [system, setSystem] = useState(null)
  const [confirmingShutdown, setConfirmingShutdown] = useState(false)
  const [payloadsDirPickerOpen, setPayloadsDirPickerOpen] = useState(false)
  const [jobsAutoClear, setJobsAutoClear] = useState(loadJobsAutoClear)
  const [savingPayloadsDir, setSavingPayloadsDir] = useState(false)
  const showToast = useToast()

  useEffect(() => {
    api.status().then(setStatus).catch(() => {})
    api.paths().then(setPaths).catch(() => {})
    api.system.info().then(setSystem).catch(() => {})
  }, [])

  const changeJobsAutoClear = (seconds) => {
    setJobsAutoClear(seconds)
    try { window.localStorage.setItem(JOBS_AUTOCLEAR_KEY, String(seconds)) } catch { /* the choice just will not be remembered */ }
  }

  // payloads_dir above still reports the running process's current value — the new
  // one only takes effect after ProsperoMgr.elf restarts, so it's not reflected here yet.
  const changePayloadsDir = (path) => {
    setSavingPayloadsDir(true)
    return api.setPayloadsDir(path)
      .then(() => showToast(t('settings.payloadsDirSaved')))
      .finally(() => setSavingPayloadsDir(false))
  }

  const bytes = (value) => (!value ? '—' : formatFileSize(value, locale))

  const loading = !status && !system

  return (
    <div>
      <SectionHero
        icon={<SlidersIcon size={18} />}
        title={t('settings.consoleOverviewTitle')}
        description={t('settings.consoleOverviewDesc')}
        status={
          status && (
            <Pill tone={status.privileged ? 'ok' : 'neutral'}>
              <ShieldIcon size={12} /> {t(status.privileged ? 'settings.elevatedPrivileges' : 'settings.limitedPrivileges')}
            </Pill>
          )
        }
      />

      <div className="mb-4 grid grid-cols-2 gap-2 lg:grid-cols-4">
        <MetricTile label={t('settings.metricConsole')} value={system?.platform} loading={loading} />
        <MetricTile label={t('settings.metricFirmware')} value={system?.firmware} loading={loading} />
        <MetricTile label={t('settings.metricCurrentUser')} value={system?.user_name || t('common.unknown')} loading={loading} />
        <MetricTile label={t('settings.metricIpAddress')} value={system?.ip_address} loading={loading} />
      </div>

      {system?.storage?.length > 0 && (
        <SettingsSection title={t('settings.storageTitle')} description={t('settings.storageDesc')}>
          <div className="grid gap-3 sm:grid-cols-2">
            {system.storage.map((volume) => {
              const pct = volume.total > 0 ? ((volume.total - volume.free) / volume.total) * 100 : 0
              return (
                <div key={volume.path} className="rounded-xl border border-panel-border px-3 py-2.5">
                  <div className="flex flex-wrap items-baseline justify-between gap-x-3 gap-y-0.5 text-sm">
                    <span className="font-semibold text-text">{volumeLabel(volume, t)}</span>
                    <span className="pmgr-mono text-xs text-text-dim">{t('settings.storageFreeOfTotal', { free: bytes(volume.free), total: bytes(volume.total) })}</span>
                  </div>
                  <div className="mt-2 h-1.5 overflow-hidden rounded-full bg-panel-border">
                    <div className={`h-full rounded-full transition-[width] duration-500 ${storageBarClass(pct)}`} style={{ width: `${pct}%` }} />
                  </div>
                  <div className="mt-1 pmgr-mono text-xs text-text-dim" dir="ltr">{t('settings.storagePathUsed', { path: volume.path, pct: pct.toFixed(0) })}</div>
                </div>
              )
            })}
          </div>
        </SettingsSection>
      )}

      <SettingsSection title={t('settings.payloadsDirTitle')} description={t('settings.payloadsDirDesc')}>
        <SettingsRow
          title={t('settings.payloadsDirLabel')}
          description={<span className="pmgr-mono" dir="ltr">{paths?.payloads_dir || '—'}</span>}
          control={
            <Button tone="neutral" size="sm" icon={<FolderIcon size={14} />} disabled={savingPayloadsDir}
                    onClick={() => setPayloadsDirPickerOpen(true)}>
              {t('settings.payloadsDirChange')}
            </Button>
          }
        />
      </SettingsSection>

      {payloadsDirPickerOpen && (
        <PathPickerModal
          title={t('settings.payloadsDirChange')}
          description={t('settings.payloadsDirDesc')}
          initialPath={paths?.payloads_dir || '/data'}
          volumes={system?.storage || []}
          mode="folder"
          actionLabel={t('common.save')}
          validateSelection={(value) => (value.startsWith('/') && !value.includes('..') ? '' : t('settings.smpInvalidPath'))}
          onClose={() => setPayloadsDirPickerOpen(false)}
          onSelect={(selected) => changePayloadsDir(selected).then(() => setPayloadsDirPickerOpen(false))}
        />
      )}

      <SettingsSection title={t('settings.jobsAutoClearTitle')} description={t('settings.jobsAutoClearDesc')}>
        <SettingsRow
          title={t('settings.jobsAutoClearLabel')}
          control={
            <select
              value={jobsAutoClear}
              onChange={(e) => changeJobsAutoClear(Number(e.target.value))}
              aria-label={t('settings.jobsAutoClearLabel')}
              className="h-9 rounded-lg border border-panel-border bg-bg/45 px-2 text-sm font-semibold text-text outline-none transition-colors focus:border-accent"
            >
              {JOBS_AUTOCLEAR_OPTIONS.map((seconds) => (
                <option key={seconds} value={seconds}>
                  {seconds === 0 ? t('settings.jobsAutoClearNever') : seconds >= 60 ? t('settings.jobsAutoClearMinutes', { count: seconds / 60 }) : t('settings.jobsAutoClearSeconds', { count: seconds })}
                </option>
              ))}
            </select>
          }
        />
      </SettingsSection>

      <div className="pmgr-surface mb-4 overflow-hidden rounded-2xl border border-panel-border bg-panel">
        <CollapsibleSection title={t('settings.technicalDetails')} defaultOpen={false}>
          <div className="text-sm">
            <CopyRow label={t('settings.copyModel')} value={system?.model} />
            <CopyRow label={t('settings.copyFirmwareSdk')} value={system?.firmware || system?.sdk_version} />
            <CopyRow label={t('settings.copyUserId')} value={system?.user_id} />
            <CopyRow label={t('settings.copyRootDir')} value={paths?.root_dir} />
            <CopyRow label={t('settings.copyPayloadsDir')} value={paths?.payloads_dir} />
            <CopyRow label={t('settings.copyBundledDir')} value={paths?.bundled_dir} />
            <CopyRow label={t('settings.copyAutoloaderIni')} value={paths?.autoloader_ini} />
          </div>
        </CollapsibleSection>
      </div>

      <SettingsSection title={t('settings.dangerZone')} description={t('settings.dangerZoneDesc')} className="border-err/25">
        <SettingsRow
          title={t('settings.shutdown')}
          description={t('settings.shutdownDesc')}
          control={<Button tone="danger" icon={<PowerIcon size={15} />} onClick={() => setConfirmingShutdown(true)}>{t('settings.shutdownConfirm')}</Button>}
        />
      </SettingsSection>

      {confirmingShutdown && (
        <ConfirmDialog
          title={t('settings.shutdownTitle')}
          description={t('settings.shutdownConfirmDesc')}
          tone="danger"
          confirmLabel={t('settings.shutdownConfirm')}
          onClose={() => setConfirmingShutdown(false)}
          onConfirm={() =>
            api.shutdown().then(() => showToast(t('settings.shuttingDownToast'), 'warn'))
          }
        />
      )}
    </div>
  )
}

function AccountsPanel() {
  const { t } = useI18n()
  const [slots, setSlots] = useState([])
  const [loading, setLoading] = useState(true)
  const [selectedSlot, setSelectedSlot] = useState(null)
  const [mode, setMode] = useState('activate') // 'activate' | 'signin'
  const [accountId, setAccountId] = useState('')
  const [idSource, setIdSource] = useState('existing') // 'existing' | 'suggested' | 'edited'
  const [busy, setBusy] = useState(false)
  const [rebootSlots, setRebootSlots] = useState(() => new Set())
  const [restoreTarget, setRestoreTarget] = useState(null)
  const showToast = useToast()

  const load = () => {
    api.accounts
      .list()
      .then((r) => setSlots(r.slots || []))
      .catch(() => {})
      .finally(() => setLoading(false))
  }
  useEffect(load, [])

  const select = (s, nextMode) => {
    setSelectedSlot(s)
    setMode(nextMode)
    const existing = !/^0x0+$/i.test(s.id_hex)
    setAccountId(existing ? s.id_hex : suggestAccountId(s.name))
    setIdSource(existing ? 'existing' : 'suggested')
  }

  const apply = () => {
    if (!selectedSlot) return
    setBusy(true)
    api.accounts
      .apply(selectedSlot.slot, accountId.trim())
      .then(() => {
        showToast(t('settings.activatedToast', { name: selectedSlot.name, slot: selectedSlot.slot }))
        setSelectedSlot(null)
        load()
      })
      .catch((e) => showToast(t('settings.activateFailedToast', { message: e.message }), 'err'))
      .finally(() => setBusy(false))
  }

  const signIn = () => {
    if (!selectedSlot) return
    setBusy(true)
    api.accounts
      .signIn(accountId.trim())
      .then(() => {
        showToast(t('settings.signedInToast', { name: selectedSlot.name }))
        setRebootSlots((prev) => new Set(prev).add(selectedSlot.slot))
        setSelectedSlot(null)
        load()
      })
      .catch((e) => showToast(t('settings.signInFailedToast', { message: e.message }), 'err'))
      .finally(() => setBusy(false))
  }

  const restore = (s) =>
    api.accounts
      .restore(s.slot)
      .then(() => {
        showToast(t('settings.slotRestoredToast', { slot: s.slot }))
        if (selectedSlot?.slot === s.slot) setSelectedSlot(null)
        load()
      })

  return (
    <div>
      <SectionHero
        icon={<UserIcon size={18} />}
        title={t('settings.offlineActivationTitle')}
        description={t('settings.offlineActivationDesc')}
      />

      <div className="mb-4 grid gap-3 sm:grid-cols-2">
        <div className="rounded-xl border border-panel-border bg-bg/45 p-3">
          <div className="text-sm font-bold text-text">{t('settings.activateCardTitle')}</div>
          <p className="mt-1 text-xs text-text-dim">{t('settings.activateCardDesc')}</p>
        </div>
        <div className="rounded-xl border border-panel-border bg-bg/45 p-3">
          <div className="text-sm font-bold text-text">{t('settings.signInCardTitle')}</div>
          <p className="mt-1 text-xs text-text-dim">{t('settings.signInCardDesc')}</p>
        </div>
      </div>

      {loading ? (
        <div className="space-y-2">
          <div className="pmgr-skeleton h-20 rounded-xl" />
          <div className="pmgr-skeleton h-20 rounded-xl" />
        </div>
      ) : slots.length === 0 ? (
        <div className="pmgr-empty-state py-10 text-center text-sm text-text-dim">
          <div className="mx-auto mb-3 grid h-12 w-12 place-items-center rounded-2xl border border-panel-border bg-panel-2 text-text-dim">
            <UserIcon size={22} />
          </div>
          <div className="font-semibold text-text">{t('settings.noAccountsTitle')}</div>
          <div className="mt-1">{t('settings.noAccountsDesc')}</div>
        </div>
      ) : (
        <div className="space-y-2">
          {slots.map((s) => {
            const isSelected = selectedSlot?.slot === s.slot
            return (
              <div key={s.slot} className="rounded-xl border border-panel-border bg-panel p-3.5">
                <div className="flex flex-wrap items-center justify-between gap-3">
                  <div className="flex min-w-0 items-center gap-3">
                    <span className="grid h-10 w-10 shrink-0 place-items-center rounded-full bg-panel-border text-text-dim">
                      <UserIcon size={18} />
                    </span>
                    <div className="min-w-0">
                      <div className="truncate text-sm font-bold text-text">{s.name}</div>
                      <div className="text-xs text-text-dim">{t('settings.slotLabel', { slot: s.slot })}</div>
                    </div>
                  </div>
                  <div className="flex shrink-0 flex-wrap items-center gap-1.5">
                    {s.is_foreground && <Pill tone="info">{t('settings.currentUserPill')}</Pill>}
                    {s.activated && <Pill tone="ok">{t('settings.activatedPill')}</Pill>}
                    {s.has_backup && <Pill tone="neutral">{t('settings.backupAvailablePill')}</Pill>}
                    {rebootSlots.has(s.slot) && <Pill tone="warn">{t('settings.rebootToApplyPill')}</Pill>}
                  </div>
                </div>

                <div className="mt-3 flex flex-wrap gap-2">
                  <Button
                    size="sm"
                    tone={isSelected && mode === 'activate' ? 'primary' : 'neutral'}
                    onClick={() => (isSelected && mode === 'activate' ? setSelectedSlot(null) : select(s, 'activate'))}
                  >
                    {isSelected && mode === 'activate' ? t('common.cancel') : t('settings.activateEllipsis')}
                  </Button>
                  {s.is_foreground && (
                    <Button
                      size="sm"
                      tone={isSelected && mode === 'signin' ? 'primary' : 'neutral'}
                      onClick={() => (isSelected && mode === 'signin' ? setSelectedSlot(null) : select(s, 'signin'))}
                    >
                      {isSelected && mode === 'signin' ? t('common.cancel') : t('settings.signInEllipsis')}
                    </Button>
                  )}
                  {s.has_backup && (
                    <Button size="sm" tone="neutral" onClick={() => setRestoreTarget(s)}>{t('settings.restoreBackup')}</Button>
                  )}
                </div>

                {isSelected && (
                  <div className="pmgr-row-in mt-3 space-y-2.5 border-t border-panel-border pt-3">
                    <div>
                      <label className="block text-sm font-semibold text-text">{t('settings.accountIdLabel')}</label>
                      <p className="mt-0.5 text-xs text-text-dim">{t('settings.accountIdDesc')}</p>
                    </div>
                    <div className="flex items-center gap-2">
                      <input
                        value={accountId}
                        onChange={(e) => { setAccountId(e.target.value); setIdSource('edited') }}
                        dir="ltr"
                        className="w-full min-w-0 flex-1 rounded-md border border-panel-border bg-bg px-3 py-1.5 text-sm pmgr-mono text-text outline-none focus:border-accent"
                      />
                      <span className="shrink-0 text-xs text-text-dim">
                        {idSource === 'existing' ? t('settings.idSourceExisting') : idSource === 'suggested' ? t('settings.idSourceSuggested') : t('settings.idSourceEdited')}
                      </span>
                    </div>
                    <details className="rounded-lg border border-panel-border bg-bg/45 px-3 py-2">
                      <summary className="cursor-pointer text-xs font-semibold text-text-dim hover:text-text">
                        {t('settings.accountIdHelpTitle')}
                      </summary>
                      <div className="mt-2 space-y-1.5 text-xs leading-relaxed text-text-dim">
                        <p>{t('settings.accountIdHelpIntro')}</p>
                        <ul className="ml-4 list-disc space-y-1">
                          <li>{t('settings.accountIdHelpThisConsole')}</li>
                          <li>{t('settings.accountIdHelpOtherConsole')}</li>
                          <li>{t('settings.accountIdHelpSaveData')}</li>
                          <li>
                            {t('settings.accountIdHelpLookup')}{' '}
                            <a
                              href={PSN_ACCOUNT_CHECKER_URL}
                              target="_blank"
                              rel="noreferrer"
                              className="text-accent underline decoration-accent/40 underline-offset-2 hover:decoration-accent"
                            >
                              {t('settings.accountIdHelpLookupLink')}
                            </a>
                          </li>
                        </ul>
                        <p>{t('settings.accountIdHelpFormat')}</p>
                      </div>
                    </details>
                    <p className="text-xs text-text-dim">{t('settings.backupNoticeBeforeChange')}</p>
                    {mode === 'signin' ? (
                      <Button tone="primary" onClick={signIn} disabled={busy || !accountId.trim()}>
                        {busy ? t('settings.signingIn') : t('settings.signInAs', { name: s.name })}
                      </Button>
                    ) : (
                      <Button tone="primary" onClick={apply} disabled={busy || !accountId.trim()}>
                        {busy ? t('settings.applyingEllipsis') : t('settings.activateSlot', { slot: s.slot })}
                      </Button>
                    )}
                  </div>
                )}

                <div className="-mx-3.5 -mb-3.5 mt-3">
                  <CollapsibleSection title={t('settings.technicalDetails')} defaultOpen={false}>
                    <div className="text-xs">
                      <CopyRow label={t('settings.copyAccountId')} value={s.id_hex} />
                      <CopyRow label={t('settings.copyNpType')} value={s.type || '—'} />
                      <CopyRow label={t('settings.copyFlags')} value={String(s.flags)} />
                      <CopyRow label={t('settings.copySlot')} value={String(s.slot)} />
                    </div>
                  </CollapsibleSection>
                </div>
              </div>
            )
          })}
        </div>
      )}

      {restoreTarget && (
        <ConfirmDialog
          title={t('settings.restoreAccountTitle')}
          description={t('settings.restoreAccountDesc', { name: restoreTarget.name, slot: restoreTarget.slot })}
          tone="warn"
          confirmLabel={t('saves.restore')}
          onClose={() => setRestoreTarget(null)}
          onConfirm={() => restore(restoreTarget)}
        />
      )}
    </div>
  )
}

// Edits ShadowMountPlus's own scanpath= lines in its config.ini — Prospero
// doesn't run ShadowMountPlus itself, this just manages where it looks.
function ShadowMountPanel() {
  const { t } = useI18n()
  const showToast = useToast()
  const [scanPaths, setScanPaths] = useState([])
  const [usingDefaults, setUsingDefaults] = useState(true)
  const [loading, setLoading] = useState(true)
  const [addOpen, setAddOpen] = useState(false)
  const [busy, setBusy] = useState(false)
  const [volumes, setVolumes] = useState([])

  const load = () => {
    api.experimental
      .getShadowMountScanPaths()
      .then((r) => {
        setScanPaths(r.scanPaths || [])
        setUsingDefaults(Boolean(r.usingDefaults))
      })
      .catch(() => {})
      .finally(() => setLoading(false))
  }
  useEffect(load, [])
  useEffect(() => { api.ftp.storage().then((r) => setVolumes(r.volumes || [])).catch(() => {}) }, [])

  const save = (next) => {
    setBusy(true)
    return api.experimental
      .setShadowMountScanPaths(next)
      .then((r) => {
        setScanPaths(next)
        setUsingDefaults(Boolean(r.usingDefaults))
      })
      .finally(() => setBusy(false))
  }

  const addPath = (value) => save([...scanPaths, value.trim()]).then(() => showToast(t('settings.smpPathAdded')))
  const removePath = (path) => save(scanPaths.filter((p) => p !== path))
  const resetToDefaults = () => save([]).then(() => showToast(t('settings.smpResetToast')))

  const validatePath = (value) => {
    const v = value.trim()
    if (!v.startsWith('/') || v.includes('..')) return t('settings.smpInvalidPath')
    if (scanPaths.includes(v)) return t('settings.smpDuplicatePath')
    return ''
  }

  return (
    <div className="mt-6 border-t border-panel-border pt-6">
      <SectionHero icon={<FolderIcon size={18} />} title={t('settings.smpTitle')} description={t('settings.smpDesc')} />
      {loading ? (
        <SkeletonRows count={2} />
      ) : (
        <div className="rounded-xl border border-panel-border bg-bg/45 p-3.5">
          {usingDefaults ? (
            <div className="text-xs text-text-dim">
              <div className="mb-1.5 text-sm font-semibold text-text">{t('settings.smpUsingDefaults')}</div>
              <p>{t('settings.smpDefaultsDesc')}</p>
            </div>
          ) : (
            <div className="space-y-1.5">
              {scanPaths.map((path) => (
                <div key={path} className="flex items-center justify-between gap-2 rounded-lg border border-panel-border bg-bg px-3 py-2 text-sm">
                  <span className="min-w-0 truncate pmgr-mono" dir="ltr">{path}</span>
                  <Button size="icon" tone="ghost" title={t('common.remove')} disabled={busy} onClick={() => removePath(path)} icon={<DeleteIcon size={14} />} />
                </div>
              ))}
            </div>
          )}
          <div className="mt-3 flex flex-wrap gap-2">
            <Button tone="neutral" size="sm" icon={<PlusIcon size={14} />} disabled={busy} onClick={() => setAddOpen(true)}>
              {t('settings.smpAddPath')}
            </Button>
            {!usingDefaults && (
              <Button tone="ghost" size="sm" disabled={busy} onClick={resetToDefaults}>
                {t('settings.smpResetDefaults')}
              </Button>
            )}
          </div>
        </div>
      )}

      {addOpen && (
        <PathPickerModal
          title={t('settings.smpAddPathTitle')}
          description={t('settings.smpDesc')}
          initialPath="/mnt"
          volumes={volumes}
          mode="folder"
          actionLabel={t('common.add')}
          validateSelection={validatePath}
          onClose={() => setAddOpen(false)}
          onSelect={(selected) => addPath(selected).then(() => setAddOpen(false))}
        />
      )}
    </div>
  )
}

function ExperimentalPanel() {
  const { t } = useI18n()
  return (
    <div>
      <SectionHero
        icon={<FlaskIcon size={18} />}
        title={t('settings.tabExperimental')}
        description={t('settings.experimentalDesc')}
      />
      <AccountsPanel />
      <ShadowMountPanel />
    </div>
  )
}

const THANKS_CHANNELS = [
  { name: 'Explosão do Game', url: 'https://www.youtube.com/@Explosaodogame' },
  { name: 'Henrique Louro', url: 'https://www.youtube.com/@HenriqueLouroYT' },
  { name: 'BX-AM', url: 'https://www.youtube.com/@GAMERAMBX' },
  { name: 'MODDED WARFARE', url: 'https://www.youtube.com/@MODDEDWARFARE' },
]

const REFERENCE_CREDITS = [
  {
    author: 'Ps5-Payload-Dev',
    url: 'https://github.com/ps5-payload-dev',
    projects: [
      { name: 'SDK', url: 'https://github.com/ps5-payload-dev/sdk' },
      { name: 'elfldr', url: 'https://github.com/ps5-payload-dev/elfldr' },
      { name: 'ftpsrv', url: 'https://github.com/ps5-payload-dev/ftpsrv' },
      { name: 'klogsrv', url: 'https://github.com/ps5-payload-dev/klogsrv' },
      { name: 'offact', url: 'https://github.com/ps5-payload-dev/offact' },
    ],
  },
  {
    author: 'Sonic-Iso',
    url: 'https://git.etawen.dev/soniciso',
    projects: [{ name: 'elf-arsenal', url: 'https://git.etawen.dev/soniciso/elf-arsenal' }],
  },
  {
    author: 'VoidWhisper',
    projects: [{ name: 'VoidShell' }],
  },
  {
    author: 'PLK',
    url: 'https://github.com/itsPLK',
    projects: [
      { name: 'PS5-Lapy-JB-Daemon', url: 'https://github.com/itsPLK/PS5-Lapy-JB-Daemon' },
      { name: 'ps5-payload-manager', url: 'https://github.com/itsPLK/ps5-payload-manager' },
      { name: 'ps5-pkg-manager', url: 'https://github.com/itsPLK/ps5-pkg-manager' },
    ],
  },
  {
    author: 'Earthonion',
    url: 'https://git.etawen.dev/earthonion',
    projects: [
      { name: 'np-fake-signin', url: 'https://git.etawen.dev/earthonion/np-fake-signin' },
      { name: 'garlic-savemgr', url: 'https://git.etawen.dev/earthonion/garlic-savemgr' },
    ],
  },
  {
    author: 'phantomptr',
    url: 'https://github.com/phantomptr',
    projects: [{ name: 'ps5upload', url: 'https://github.com/phantomptr/ps5upload' }],
  },
  {
    author: 'owendswang',
    url: 'https://github.com/owendswang',
    projects: [
      { name: 'PS5 Web File Manager', url: 'https://github.com/owendswang/ps5-web-file-manager' },
      { name: 'ps5-fan-control', url: 'https://github.com/owendswang/ps5-fan-control' },
    ],
  },
  {
    author: 'seregonwar',
    url: 'https://github.com/seregonwar',
    projects: [{ name: 'zftpd', url: 'https://github.com/seregonwar/zftpd' }],
  },
  {
    author: 'drakmor',
    url: 'https://github.com/drakmor',
    projects: [
      { name: 'fan_target', url: 'https://github.com/drakmor/fan_target' },
      { name: 'ps5-hwinfo', url: 'https://github.com/drakmor/ps5-hwinfo' },
    ],
  },
  { author: 'Libmicrohttpd', url: 'https://www.gnu.org/software/libmicrohttpd/', projects: [] },
  {
    author: 'Alexander Roshal / RARLab',
    url: 'https://www.rarlab.com/',
    projects: [{ name: 'UnRAR', url: 'https://www.rarlab.com/rar_add.htm' }],
  },
]

function UpdatesSection() {
  const { t } = useI18n()
  const showToast = useToast()
  const { status, refresh, working } = useUpdate()
  const [dialogOpen, setDialogOpen] = useState(false)

  const check = () => api.update.check().then(refresh).catch((e) => showToast(e.message, 'err'))
  const setAuto = (enabled) => api.update.setAuto(enabled).then(refresh).catch((e) => showToast(e.message, 'err'))

  const summary = !status ? '…'
    : status.state === 'checking' ? t('update.checking')
    : status.available ? t('update.newVersion', { version: status.latest })
    : status.error ? status.error
    : status.latest ? t('update.upToDate', { current: status.current })
    : t('update.notChecked', { current: status.current })

  return (
    <SettingsSection title={t('update.sectionTitle')} description={t('update.sectionDesc')}>
      <SettingsRow
        title={summary}
        control={
          status?.available ? (
            <Button tone="primary" sheen onClick={() => setDialogOpen(true)}>{t('update.now')}</Button>
          ) : (
            <Button tone="neutral" onClick={check} disabled={working}>{working ? t('update.checking') : t('update.check')}</Button>
          )
        }
      />
      <SettingsRow
        title={t('update.autoTitle')}
        description={t('update.autoDesc')}
        control={<Toggle checked={Boolean(status?.auto_update)} onChange={setAuto} disabled={!status} label={t('update.autoTitle')} />}
      />
      {dialogOpen && status && <UpdateDialog status={status} refresh={refresh} onClose={() => setDialogOpen(false)} />}
    </SettingsSection>
  )
}

function AboutPanel() {
  const { t } = useI18n()
  return (
    <Panel title={t('settings.tabAbout')}>
      <UpdatesSection />
      <div className="text-sm leading-relaxed text-text-dim">
        <p className="text-text">{t('settings.aboutTagline')}</p>
        <p className="mt-1">
          {t('settings.aboutMadeBy', { name: 'maj0r' })} — <EmojiText text="🇧🇷" />
        </p>
        <p className="mt-1">{t('settings.aboutForUsers')}</p>
        <p className="mt-3">
          {t('settings.aboutSource')}{' '}
          <a
            href="https://github.com/notmaj0r/ProsperoMgr"
            target="_blank"
            rel="noreferrer"
            className="inline-flex items-center gap-1.5 align-middle text-accent hover:underline"
          >
            <GithubIcon size={15} /> ProsperoMgr
          </a>{' '}
          <EmojiText text={t('settings.aboutStarPrompt')} />
        </p>
      </div>

      <div className="mt-5 border-t border-panel-border pt-4">
        <div className="mb-2 text-xs font-bold uppercase tracking-[0.14em] text-text-dim">{t('settings.specialThanks')}</div>
        <div className="space-y-2">
          {THANKS_CHANNELS.map((channel) => (
            <a
              key={channel.url}
              href={channel.url}
              target="_blank"
              rel="noreferrer"
              className="pmgr-thanks-card group flex items-center gap-3 rounded-xl border border-panel-border bg-bg/45 p-3 text-text"
            >
              <span className="grid h-11 w-11 shrink-0 place-items-center rounded-xl bg-red-600 text-white shadow-[0_10px_28px_-12px_#dc2626]"><YoutubeIcon size={23} /></span>
              <span className="min-w-0 flex-1"><strong className="block truncate">{channel.name}</strong><small className="text-text-dim">YouTube</small></span>
              <ExternalIcon size={17} />
            </a>
          ))}
        </div>
      </div>

      <div className="mt-5 border-t border-panel-border pt-4">
        <div className="mb-2 text-xs font-bold uppercase tracking-[0.14em] text-text-dim">{t('settings.creditsReferences')}</div>
        <ul className="space-y-2 text-sm leading-relaxed text-text-dim">
          {REFERENCE_CREDITS.map((credit) => (
            <li key={credit.url || credit.author} className="flex items-start gap-2">
              <span aria-hidden="true" className="mt-[0.05em] text-accent">•</span>
              <p>
                {credit.url ? (
                  <a href={credit.url} target="_blank" rel="noreferrer" className="font-bold text-text hover:text-accent hover:underline">
                    {credit.author}
                  </a>
                ) : (
                  <strong className="text-text">{credit.author}</strong>
                )}
                {credit.projects.length > 0 && <span> — </span>}
                {credit.projects.map((project, index) => (
                  <span key={project.url || project.name}>
                    {index > 0 && ', '}
                    {project.url ? (
                      <a href={project.url} target="_blank" rel="noreferrer" className="text-accent hover:underline">
                        {project.name}
                      </a>
                    ) : (
                      <span>{project.name}</span>
                    )}
                  </span>
                ))}
              </p>
            </li>
          ))}
          <li className="flex items-start gap-2">
            <span aria-hidden="true" className="mt-[0.05em] text-accent">•</span>
            <p>{t('settings.communityCredit')}</p>
          </li>
        </ul>
      </div>

    </Panel>
  )
}

const TABS = [
  { id: 'general', labelKey: 'settings.tabGeneral', icon: SlidersIcon },
  { id: 'appearance', labelKey: 'settings.tabAppearance', icon: PaletteIcon },
  { id: 'thermal', labelKey: 'settings.tabThermal', icon: ThermometerIcon },
  { id: 'hardware', labelKey: 'hardware.title', icon: CpuIcon },
  { id: 'experimental', labelKey: 'settings.tabExperimental', icon: FlaskIcon },
  { id: 'about', labelKey: 'settings.tabAbout', icon: InfoIcon },
]

export default function SettingsView({ settingsTab, onNavigate }) {
  const { t } = useI18n()
  const [tab, setTab] = useState(settingsTab || 'general')

  useEffect(() => setTab(settingsTab || 'general'), [settingsTab])

  const selectTab = (id) => {
    setTab(id)
    onNavigate?.('settings', id)
  }

  const moveTabFocus = (event, index) => {
    if (isPS5()) return
    let next = null
    if (event.key === 'ArrowRight') next = (index + 1) % TABS.length
    if (event.key === 'ArrowLeft') next = (index - 1 + TABS.length) % TABS.length
    if (event.key === 'Home') next = 0
    if (event.key === 'End') next = TABS.length - 1
    if (next === null) return
    event.preventDefault()
    selectTab(TABS[next].id)
    window.requestAnimationFrame(() => document.getElementById(`pmgr-settings-tab-${TABS[next].id}`)?.focus())
  }

  return (
    <div>
      <div role="tablist" aria-label={t('nav.settings')} className="pmgr-settings-tabs mb-4 flex gap-1.5 overflow-x-auto rounded-2xl border border-panel-border bg-panel p-1.5">
        {TABS.map((tabItem, index) => {
          const Icon = tabItem.icon
          return (
            <button
              key={tabItem.id}
              id={`pmgr-settings-tab-${tabItem.id}`}
              role="tab"
              aria-selected={tab === tabItem.id}
              aria-controls="pmgr-settings-panel"
              tabIndex={tab === tabItem.id ? 0 : -1}
              onClick={() => selectTab(tabItem.id)}
              onKeyDown={(event) => moveTabFocus(event, index)}
              className={`flex shrink-0 items-center gap-2 rounded-xl px-3.5 py-2 text-sm font-semibold transition-[color,background-color,box-shadow] duration-200 ${
                tab === tabItem.id
                  ? 'bg-accent text-white shadow-[0_6px_18px_-8px_var(--color-accent)]'
                  : 'text-text-dim hover:bg-panel-border hover:text-text'
              }`}
            >
              <Icon size={15} />
              {t(tabItem.labelKey)}
            </button>
          )
        })}
      </div>

      <div key={tab} id="pmgr-settings-panel" role="tabpanel" aria-labelledby={`pmgr-settings-tab-${tab}`} className="pmgr-view-enter">
        {tab === 'general' && <GeneralTab />}
        {tab === 'appearance' && (
          <>
            <ThemePanel />
            <VisualEffectsPanel />
          </>
        )}
        {tab === 'thermal' && <ThermalPanel />}
        {tab === 'hardware' && <HardwareTab />}
        {tab === 'experimental' && <ExperimentalPanel />}
        {tab === 'about' && <AboutPanel />}
      </div>
    </div>
  )
}
