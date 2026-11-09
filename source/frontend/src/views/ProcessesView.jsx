import { useState } from 'react'
import Panel from '../components/Panel.jsx'
import Button from '../components/Button.jsx'
import SearchField from '../components/SearchField.jsx'
import SegmentedControl from '../components/SegmentedControl.jsx'
import Pill from '../components/Pill.jsx'
import EmptyState from '../components/EmptyState.jsx'
import { SkeletonRows } from '../components/Skeleton.jsx'
import { ConfirmDialog } from '../components/Dialog.jsx'
import { KillIcon, RefreshIcon, ShieldIcon } from '../components/icons.jsx'
import { api } from '../lib/api.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'
import { usePolling } from '../lib/usePolling.js'
import { isPayloadLikeProcessName } from '../lib/processes.js'

// pid 0 is a real row on FreeBSD/PS5 (the kernel/swapper process) that can't actually be
// signaled — kill() on it targets the whole process group, so it's blocked here and server-side.
function isProtected(pid) {
  return pid <= 1
}

export default function ProcessesView() {
  const { t, locale } = useI18n()
  const [processes, setProcesses] = useState([])
  const [loaded, setLoaded] = useState(false)
  const [filter, setFilter] = useState('')
  const [kind, setKind] = useState('all')
  const [error, setError] = useState('')
  // null when idle — using 0 collided with pid 0 ("kernel"), which made that
  // row's button permanently read "Killing…" even before ever being clicked.
  const [busy, setBusy] = useState(null)
  const [killTarget, setKillTarget] = useState(null) // { pid, name }
  const showToast = useToast()

  // The list stays on screen while it refreshes, and is replaced only when something changed.
  const load = () => {
    api.processes
      .list()
      .then((r) => setProcesses((prev) => (JSON.stringify(prev) === JSON.stringify(r.processes) ? prev : r.processes)))
      .catch((e) => setError(e.message))
      .finally(() => setLoaded(true))
  }

  usePolling(load, 5000)

  const doKill = (pid, name) => {
    setBusy(pid)
    setError('')
    return api.processes
      .kill(pid)
      .then(() => {
        load()
        showToast(t('processes.stoppedToast', { name, pid }))
      })
      .catch((e) => {
        setError(e.message)
        throw e
      })
      .finally(() => setBusy(null))
  }

  const payloadCount = processes.filter((p) => isPayloadLikeProcessName(p.name)).length
  const visible = processes
    .filter((p) => kind === 'all' || isPayloadLikeProcessName(p.name))
    .filter((p) => p.name.toLocaleLowerCase(locale).includes(filter.toLocaleLowerCase(locale)))

  return (
    <Panel
      title={t('processes.title')}
      actions={<Button tone="neutral" icon={<RefreshIcon size={15} />} onClick={load}>{t('processes.refresh')}</Button>}
    >
      <p className="mb-3 text-sm text-text-dim">{t('processes.intro')}</p>

      <div className="mb-3 flex flex-col sm:flex-row sm:items-center">
        <SearchField value={filter} onChange={setFilter} placeholder={t('processes.searchPlaceholder')} className="mb-2 sm:mb-0 sm:mr-3 sm:max-w-sm" />
        <SegmentedControl
          options={[
            { id: 'all', label: t('processes.filterAll'), count: processes.length },
            { id: 'payloads', label: t('nav.payloads'), count: payloadCount },
          ]}
          value={kind}
          onChange={setKind}
        />
      </div>

      {error && <div className="pmgr-notice-in mb-3 rounded-md bg-err/10 px-3 py-2 text-sm text-err">{error}</div>}

      {!loaded ? (
        <SkeletonRows count={8} />
      ) : visible.length === 0 ? (
        processes.length === 0 ? (
          <EmptyState size="sm" title={t('processes.emptyTitle')} />
        ) : (
          <EmptyState size="sm" title={t('processes.noMatchTitle')} />
        )
      ) : (
        <div className="overflow-auto rounded-xl border border-panel-border" style={{ maxHeight: '65vh' }}>
          <table className="w-full text-left text-sm">
            <thead className="sticky top-0 z-10 bg-panel">
              <tr className="text-text-dim">
                <th className="px-3 py-2 font-medium">{t('processes.pid')}</th>
                <th className="px-3 py-2 font-medium">{t('processes.ppid')}</th>
                <th className="px-3 py-2 font-medium">{t('processes.name')}</th>
                <th className="px-3 py-2 font-medium"></th>
              </tr>
            </thead>
            <tbody>
              {visible.map((p) => (
                <tr key={p.pid} className="border-t border-panel-border transition-colors hover:bg-panel-2">
                  <td className="pmgr-mono px-3 py-1.5 text-text-dim">{p.pid}</td>
                  <td className="pmgr-mono px-3 py-1.5 text-text-dim">{p.ppid}</td>
                  <td className="px-3 py-1.5 text-text">
                    {p.name}
                    {isPayloadLikeProcessName(p.name) && <span className="ml-2 align-middle"><Pill tone="info">{t('processes.payloadTag')}</Pill></span>}
                  </td>
                  <td className="px-3 py-1.5 text-right">
                    {isProtected(p.pid) ? (
                      <span title={t('processes.protected')} className="inline-flex h-9 w-9 items-center justify-center text-text-dim/60">
                        <ShieldIcon size={15} />
                      </span>
                    ) : (
                      <Button
                        size="icon"
                        tone="danger"
                        title={t('processes.stop', { pid: p.pid })}
                        onClick={() => setKillTarget({ pid: p.pid, name: p.name })}
                        disabled={busy === p.pid}
                        icon={<KillIcon size={15} />}
                      />
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      {loaded && visible.length > 0 && (
        <div className="mt-2 text-xs text-text-dim">{t('processes.showing', { shown: visible.length, total: processes.length })}</div>
      )}

      {killTarget && (
        <ConfirmDialog
          title={t('processes.stopTitle')}
          description={t('processes.stopDesc', { name: killTarget.name, pid: killTarget.pid })}
          detail={t('processes.stopDetail')}
          tone="danger"
          confirmLabel={t('processes.stopConfirm')}
          onClose={() => setKillTarget(null)}
          onConfirm={() => doKill(killTarget.pid, killTarget.name)}
        />
      )}
    </Panel>
  )
}
