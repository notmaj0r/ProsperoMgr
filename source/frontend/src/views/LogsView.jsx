import { useEffect, useRef, useState } from 'react'
import Panel from '../components/Panel.jsx'
import Button from '../components/Button.jsx'
import SearchField from '../components/SearchField.jsx'
import SegmentedControl from '../components/SegmentedControl.jsx'
import EmptyState from '../components/EmptyState.jsx'
import { ConfirmDialog } from '../components/Dialog.jsx'
import { ClipboardIcon, LogsIcon } from '../components/icons.jsx'
import { api } from '../lib/api.js'
import { copyText } from '../lib/clipboard.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'
import { POLL_SCALE } from '../lib/usePolling.js'
import { SkeletonRows } from '../components/Skeleton.jsx'

function logTone(line) {
  if (/failed|error|unreachable/i.test(line)) return 'text-err'
  if (/loaded|listening|reachable/i.test(line)) return 'text-ok'
  return 'text-text-dim'
}

const SOURCES = [
  { id: 'all', labelKey: 'logs.sourceAll' },
  { id: 'prospero', labelKey: 'logs.sourceProspero' },
  { id: 'klog', labelKey: 'logs.sourceKlog' },
]

export default function LogsView({ onNavigate }) {
  const { t, locale } = useI18n()
  const [logs, setLogs] = useState(null)
  const [klogs, setKlogs] = useState(null)
  const [klogConnected, setKlogConnected] = useState(false)
  const [source, setSource] = useState('all')
  const [filter, setFilter] = useState('')
  const [autoScroll, setAutoScroll] = useState(true)
  const [clearOpen, setClearOpen] = useState(false)
  const consoleRef = useRef(null)
  const showToast = useToast()

  useEffect(() => {
    let cancelled = false
    // Keeps the old array when nothing new arrived, so a quiet log does not redraw every line.
    const sameLines = (a, b) => a && a.length === b.length && a[a.length - 1] === b[b.length - 1]
    const poll = () => {
      if (document.visibilityState === 'hidden') return
      api.logs.list().then((r) => !cancelled && setLogs((prev) => (sameLines(prev, r.lines) ? prev : r.lines))).catch(() => {})
      api.klogs
        .list()
        .then((r) => {
          if (cancelled) return
          setKlogs((prev) => (sameLines(prev, r.lines) ? prev : r.lines))
          setKlogConnected(r.connected)
        })
        .catch(() => {})
    }
    poll()
    const id = setInterval(poll, 3000 * POLL_SCALE)
    return () => {
      cancelled = true
      clearInterval(id)
    }
  }, [])

  // Neither side has timestamps that sort reliably against the other, so tag each
  // line with its source and filter rather than interleaving.
  const loading = logs === null || klogs === null
  const tagged = [
    ...(logs || []).map((line) => ({ line, src: 'prospero' })),
    ...(klogs || []).map((line) => ({ line, src: 'klog' })),
  ]

  const visible = tagged
    .filter((l) => source === 'all' || l.src === source)
    .filter((l) => !filter.trim() || l.line.toLocaleLowerCase(locale).includes(filter.trim().toLocaleLowerCase(locale)))

  useEffect(() => {
    if (autoScroll && consoleRef.current) consoleRef.current.scrollTop = consoleRef.current.scrollHeight
  }, [visible.length, autoScroll])

  // Scrolling up stops following; reaching the bottom again resumes it.
  const onConsoleScroll = () => {
    const el = consoleRef.current
    if (el) setAutoScroll(el.scrollHeight - el.scrollTop - el.clientHeight < 24)
  }

  const copyLogs = () => {
    const text = visible.map((l) => l.line).join('\n')
    if (!text) {
      showToast(t('logs.noVisibleLogs'), 'warn')
      return
    }
    copyText(text).then(
      () => showToast(t('logs.copied')),
      () => showToast(t('logs.copyFailed'), 'err'),
    )
  }

  const exportLogs = () => {
    const blob = new Blob([visible.map((l) => l.line).join('\n')], { type: 'text/plain' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = `prosperomgr-logs-${Date.now()}.txt`
    a.click()
    URL.revokeObjectURL(url)
  }

  const clearLogs = () =>
    Promise.all([api.logs.clear(), api.klogs.clear()])
      .then(() => {
        setLogs([])
        setKlogs([])
        showToast(t('logs.cleared'))
      })

  return (
    <Panel title={t('logs.title')}>
      <p className="mb-4 max-w-2xl text-sm text-text-dim">{t('logs.intro')}</p>

      {/* Primary toolbar: finding what you need. */}
      <div className="mb-3 flex flex-col gap-2 sm:flex-row sm:items-center">
        <SearchField value={filter} onChange={setFilter} placeholder={t('logs.filterPlaceholder')} className="sm:max-w-xs" />
        <SegmentedControl options={SOURCES.map((s) => ({ ...s, label: t(s.labelKey) }))} value={source} onChange={setSource} />
        <Button
          tone={autoScroll ? 'primary' : 'neutral'}
          size="sm"
          className="sm:ml-auto"
          onClick={() => setAutoScroll(!autoScroll)}
          aria-pressed={autoScroll}
        >
          {t('logs.autoScroll')}
        </Button>
      </div>

      {source !== 'prospero' && !klogConnected && (
        <div className="mb-3 flex flex-wrap items-center gap-2 rounded-lg border border-panel-border bg-bg/45 px-3 py-2 text-xs text-text-dim">
          <span>{t('logs.klogUnavailable')}</span>
          <Button size="sm" tone="neutral" className="ml-auto" onClick={() => onNavigate?.('payloads')}>{t('logs.openPayloads')}</Button>
        </div>
      )}

      <div ref={consoleRef} onScroll={onConsoleScroll} className="pmgr-log-console" style={{ maxHeight: '60vh' }} dir="ltr">
        {loading ? (
          <SkeletonRows count={4} />
        ) : visible.length === 0 ? (
          <EmptyState size="sm" icon={<LogsIcon size={22} />} title={t('logs.emptyTitle')} />
        ) : (
          visible.map((l, i) => (
            <div key={i} className={logTone(l.line)}>
              {source === 'all' && <span className="text-text-dim/60">[{l.src === 'klog' ? 'klog' : 'pmgr'}] </span>}
              {l.line}
            </div>
          ))
        )}
      </div>

      {/* Secondary/destructive: less prominent than the toolbar above, and
          separated from it so Clear can't be mistaken for a filter action. */}
      <div className="mt-3 flex flex-wrap items-center gap-2 border-t border-panel-border pt-3">
        <Button tone="neutral" size="sm" icon={<ClipboardIcon size={13} />} onClick={copyLogs}>{t('logs.copy')}</Button>
        <Button tone="neutral" size="sm" onClick={exportLogs}>{t('logs.export')}</Button>
        <span className="pmgr-num ml-auto mr-2 text-xs text-text-dim">{t('logs.lines')}: {visible.length}</span>
        <Button tone="danger" size="sm" onClick={() => setClearOpen(true)}>{t('logs.clear')}</Button>
      </div>

      {clearOpen && (
        <ConfirmDialog
          title={t('logs.clear')}
          description={t('logs.clearConfirmDesc')}
          tone="danger"
          confirmLabel={t('logs.clearConfirm')}
          onClose={() => setClearOpen(false)}
          onConfirm={clearLogs}
        />
      )}
    </Panel>
  )
}
