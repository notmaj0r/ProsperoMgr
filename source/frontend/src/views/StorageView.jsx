import { useCallback, useEffect, useMemo, useRef, useState } from 'react'
import Panel from '../components/Panel.jsx'
import Button from '../components/Button.jsx'
import EmptyState from '../components/EmptyState.jsx'
import SegmentedControl from '../components/SegmentedControl.jsx'
import Treemap from '../components/Treemap.jsx'
import { SkeletonRows } from '../components/Skeleton.jsx'
import { ExternalIcon, FileIcon, FolderIcon, GoUpIcon, RefreshIcon, SearchIcon, StopIcon } from '../components/icons.jsx'
import { api } from '../lib/api.js'
import { formatFileSize, formatNumber, volumeLabel } from '../lib/format.js'
import { timeAgo } from '../lib/time.js'
import { useI18n } from '../lib/i18n.jsx'

const METRIC_KEY = 'pmgr-storage-metric'
// Same key FtpView restores its folder from, so "Open in Files" lands in the right place.
const FILES_PATH_KEY = 'pmgr-files-path'
const TREE_DEPTH = 2
const TREE_LIMIT = 80

const BUCKETS = ['games', 'media', 'saves', 'other']
const BUCKET_COLORS = {
  games: 'var(--color-accent)',
  media: '#a855f7',
  saves: 'var(--color-ok)',
  other: 'var(--color-warn)',
  hidden: 'rgba(var(--color-text-dim-rgb), 0.55)',
  free: 'rgba(var(--color-panel-border-rgb), 1)',
}
const BUCKET_LABEL_KEYS = {
  games: 'analyzer.bucketGames',
  media: 'analyzer.bucketMedia',
  saves: 'analyzer.bucketSaves',
  other: 'analyzer.bucketOther',
  hidden: 'analyzer.hidden',
  free: 'analyzer.free',
}

function joinPath(dir, name) {
  return dir === '/' ? `/${name}` : `${dir}/${name}`
}

function parentPath(path) {
  const cut = path.lastIndexOf('/')
  return cut <= 0 ? '/' : path.slice(0, cut)
}

function savedMetric() {
  try {
    return window.localStorage.getItem(METRIC_KEY) === 'size' ? 'size' : 'alloc'
  } catch {
    return 'alloc'
  }
}

// On-disk size is what actually fills the drive, but some filesystems (exFAT USB sticks)
// report no block counts — fall back to the file size there instead of showing zero.
function makeMetric(mode) {
  return (n) => (mode === 'alloc' && (n.alloc > 0 || !n.size) ? n.alloc || 0 : n.size || 0)
}

function UsageBar({ segments, total }) {
  return (
    <div className="flex h-4 w-full overflow-hidden rounded-full bg-panel-2">
      {segments.map((s) =>
        s.value > 0 && total > 0 ? (
          <div key={s.id} title={s.label} style={{ width: `${(s.value / total) * 100}%`, background: s.color }} />
        ) : null,
      )}
    </div>
  )
}

function ShareBar({ value, total, color = 'var(--color-accent)' }) {
  const pct = total > 0 ? Math.min(100, (value / total) * 100) : 0
  return (
    <div className="h-1.5 w-full overflow-hidden rounded-full bg-panel-2">
      <div className="h-full rounded-full" style={{ width: `${pct}%`, background: color }} />
    </div>
  )
}

export default function StorageView({ onNavigate }) {
  const { t, locale } = useI18n()
  const [volumes, setVolumes] = useState([])
  const [choice, setChoice] = useState('/user')
  const [customPath, setCustomPath] = useState('/user/data')
  const [status, setStatus] = useState(null)
  const [summary, setSummary] = useState(null)
  const [fileStats, setFileStats] = useState(null)
  const [node, setNode] = useState(null)
  const [path, setPath] = useState('')
  const [treeLoading, setTreeLoading] = useState(false)
  const [tab, setTab] = useState('folders')
  const [metricMode, setMetricMode] = useState(savedMetric)
  const [error, setError] = useState('')
  const [starting, setStarting] = useState(false)
  const cacheRef = useRef(new Map())
  const explorerRef = useRef(null)
  const lastStateRef = useRef(null)

  const metric = useMemo(() => makeMetric(metricMode), [metricMode])
  const formatSize = useCallback((bytes) => formatFileSize(bytes, locale), [locale])
  const scanning = status?.state === 'scanning' || status?.state === 'paused'

  const describe = useCallback(
    (n) => {
      if (n.type === 'rest') return { name: t('analyzer.itemsMore', { count: n.count }) }
      if (n.type === 'files') return { name: t('analyzer.smallFiles', { count: n.files }) }
      if (n.type === 'mount') return { name: n.name, detail: t('analyzer.mountItem') }
      if (n.type === 'file') return { name: n.name }
      return { name: n.name, detail: t('analyzer.filesCount', { count: n.files }) }
    },
    [t],
  )

  const openTree = useCallback((target) => {
    const cached = cacheRef.current.get(target)
    if (cached) {
      setNode(cached)
      setPath(target)
      return Promise.resolve()
    }
    setTreeLoading(true)
    return api.storage
      .tree(target, TREE_DEPTH, TREE_LIMIT)
      .then((r) => {
        cacheRef.current.set(target, r.node)
        setNode(r.node)
        setPath(target)
        setError('')
      })
      .catch((e) => setError(e.message))
      .finally(() => setTreeLoading(false))
  }, [])

  const loadResults = useCallback(() => {
    cacheRef.current = new Map()
    return Promise.all([api.storage.summary(), api.storage.files(100)])
      .then(([s, f]) => {
        setSummary(s)
        setFileStats(f)
        return openTree(s.root)
      })
      .catch((e) => setError(e.message))
  }, [openTree])

  useEffect(() => {
    api.ftp
      .storage()
      .then((r) => {
        const list = r.volumes || []
        setVolumes(list)
        if (!list.some((v) => v.path === '/user') && list.length) setChoice(list[0].path)
      })
      .catch(() => {})
  }, [])

  // Poll while a scan runs; the result is held by the backend, so leaving the page
  // and coming back picks up where it was.
  useEffect(() => {
    let cancelled = false
    let timer = null
    const poll = () => {
      api.storage
        .scanStatus()
        .then((s) => {
          if (cancelled) return
          setStatus(s)
          const previous = lastStateRef.current
          lastStateRef.current = s.state
          if (s.state === 'done' && previous !== 'done') loadResults()
          if (s.state === 'scanning' || s.state === 'paused') timer = window.setTimeout(poll, 1000)
        })
        .catch((e) => {
          if (cancelled) return
          setError(e.message)
          timer = window.setTimeout(poll, 3000)
        })
    }
    if (status === null || scanning) poll()
    return () => {
      cancelled = true
      if (timer) window.clearTimeout(timer)
    }
    // Restarts only when a scan begins/ends, not on every progress tick.
  }, [scanning, loadResults])

  const startScan = () => {
    const target = choice === 'custom' ? customPath.trim() : choice
    setStarting(true)
    setError('')
    api.storage
      .scan(target)
      .then(() => {
        lastStateRef.current = 'scanning'
        setStatus((s) => ({ ...(s || {}), state: 'scanning', root: target, files: 0, dirs: 0, bytes: 0, current: '' }))
      })
      .catch((e) => setError(e.message))
      .finally(() => setStarting(false))
  }

  const cancelScan = () => {
    api.storage.cancel().catch((e) => setError(e.message))
  }

  const changeMetric = (mode) => {
    setMetricMode(mode)
    try {
      window.localStorage.setItem(METRIC_KEY, mode)
    } catch {
      // Private mode — the choice just won't persist.
    }
  }

  const goTo = (target) => {
    openTree(target).then(() => {
      setTab('folders')
      explorerRef.current?.scrollIntoView({ behavior: 'smooth', block: 'start' })
    })
  }

  const openInFiles = (target) => {
    window.sessionStorage.setItem(FILES_PATH_KEY, target)
    onNavigate?.('ftp')
  }

  // ---- Overview numbers ------------------------------------------------------
  const overview = useMemo(() => {
    if (!summary) return null
    const byBucket = { games: 0, media: 0, saves: 0, other: 0 }
    for (const c of summary.categories) byBucket[c.bucket] += metric(c)
    const scanned = metric(summary.scanned)
    const volume = summary.volume || {}
    // Only a whole drive's used space can be compared with what the walk found.
    const hidden = volume.mount_root ? Math.max(0, (volume.used || 0) - (summary.scanned.alloc || 0)) : 0
    const free = volume.mount_root ? volume.free || 0 : 0
    const segments = [
      ...BUCKETS.map((id) => ({ id, value: byBucket[id] })),
      { id: 'hidden', value: hidden },
      { id: 'free', value: free },
    ].map((s) => ({ ...s, color: BUCKET_COLORS[s.id], label: t(BUCKET_LABEL_KEYS[s.id]) }))
    const total = segments.reduce((sum, s) => sum + s.value, 0)
    const others = summary.categories.filter((c) => c.bucket === 'other' && metric(c) > 0)
    return { segments, total, scanned, hidden, others, otherTotal: byBucket.other + hidden }
  }, [summary, metric, t])

  const crumbs = useMemo(() => {
    if (!summary || !path) return []
    const root = summary.root
    const rest = path === root ? [] : path.slice(root === '/' ? 1 : root.length + 1).split('/')
    const out = [{ label: root, path: root }]
    rest.forEach((part, i) => out.push({ label: part, path: joinPath(out[i].path, part) }))
    return out
  }, [summary, path])

  const children = useMemo(() => {
    if (!node?.children) return []
    return [...node.children].sort((a, b) => metric(b) - metric(a))
  }, [node, metric])

  const nodeTotal = node ? metric(node) : 0
  const elapsed = status && status.started_at ? Math.max(0, (status.finished_at || status.now || 0) - status.started_at) : 0
  const elapsedText = `${Math.floor(elapsed / 60)}:${String(elapsed % 60).padStart(2, '0')}`

  const locationOptions = [
    ...volumes.map((v) => ({ value: v.path, label: `${volumeLabel(v, t)} (${v.path})` })),
    ...(volumes.some((v) => v.path === '/user') ? [] : [{ value: '/user', label: '/user' }]),
    { value: 'custom', label: t('analyzer.customPath') },
  ]

  return (
    <div className="space-y-4">
      <Panel
        title={t('analyzer.title')}
        actions={
          <SegmentedControl
            label={t('analyzer.sizeMode')}
            value={metricMode}
            onChange={changeMetric}
            options={[
              { id: 'alloc', label: t('analyzer.sizeOnDisk') },
              { id: 'size', label: t('analyzer.sizeLogical') },
            ]}
          />
        }
      >
        <p className="mb-4 text-sm text-text-dim">{t('analyzer.intro')}</p>

        <div className="flex flex-wrap items-end gap-3">
          <label className="flex min-w-[220px] flex-1 flex-col gap-1 text-xs font-semibold text-text-dim sm:max-w-sm">
            {t('analyzer.location')}
            <select
              value={choice}
              onChange={(e) => setChoice(e.target.value)}
              disabled={scanning}
              className="min-h-10 rounded-lg border border-panel-border bg-panel-2 px-3 text-sm font-normal text-text"
            >
              {locationOptions.map((o) => (
                <option key={o.value} value={o.value}>{o.label}</option>
              ))}
            </select>
          </label>
          {choice === 'custom' && (
            <input
              value={customPath}
              onChange={(e) => setCustomPath(e.target.value)}
              disabled={scanning}
              placeholder="/user/data"
              aria-label={t('analyzer.customPath')}
              className="pmgr-mono min-h-10 min-w-[200px] flex-1 rounded-lg border border-panel-border bg-panel-2 px-3 text-sm text-text sm:max-w-xs"
            />
          )}
          {scanning ? (
            <Button tone="danger" icon={<StopIcon size={15} />} onClick={cancelScan}>{t('analyzer.cancel')}</Button>
          ) : (
            <Button tone="primary" icon={<SearchIcon size={15} />} onClick={startScan} disabled={starting}>
              {summary ? t('analyzer.rescan') : t('analyzer.scan')}
            </Button>
          )}
        </div>

        {error && <div className="mt-3 rounded-md bg-err/10 px-3 py-2 text-sm text-err">{error}</div>}
        {status?.state === 'error' && status.error && (
          <div className="mt-3 rounded-md bg-err/10 px-3 py-2 text-sm text-err">{status.error}</div>
        )}
        {status?.state === 'canceled' && <div className="mt-3 text-sm text-text-dim">{t('analyzer.canceled')}</div>}

        {scanning && (
          <div className="mt-4 rounded-lg border border-panel-border bg-bg/40 p-3">
            <div className="flex flex-wrap items-center justify-between gap-2 text-sm">
              <span className="flex items-center gap-2 font-semibold text-text">
                <RefreshIcon size={15} className="animate-spin" />
                {status.state === 'paused' ? t('analyzer.paused') : t('analyzer.scanning')}
              </span>
              <span className="pmgr-mono text-xs text-text-dim">
                {t('analyzer.progress', {
                  files: formatNumber(status.files || 0, locale),
                  dirs: formatNumber(status.dirs || 0, locale),
                  size: formatSize(status.bytes || 0),
                })}
                {elapsed > 0 && ` · ${t('analyzer.elapsed', { time: elapsedText })}`}
              </span>
            </div>
            <div className="pmgr-mono mt-2 truncate text-xs text-text-dim" dir="ltr">{status.current || status.root}</div>
          </div>
        )}

        {!scanning && summary && status?.finished_at > 0 && (
          <div className="mt-3 text-xs text-text-dim">
            {t('analyzer.lastScan', {
              path: summary.root,
              when: timeAgo(status.finished_at * 1000, locale),
              files: formatNumber(summary.scanned.files, locale),
              dirs: formatNumber(summary.scanned.dirs, locale),
            })}
          </div>
        )}

        {!scanning && !summary && status && status.state !== 'done' && (
          <EmptyState
            size="sm"
            className="mt-4"
            icon={<SearchIcon size={20} />}
            title={t('analyzer.emptyTitle')}
            description={t('analyzer.emptyDesc')}
          />
        )}
        {status === null && <div className="mt-4"><SkeletonRows count={2} /></div>}
      </Panel>

      {summary && overview && (
        <div className="grid items-start gap-4 xl:grid-cols-2">
          <Panel title={t('analyzer.overviewTitle')}>
            <UsageBar segments={overview.segments} total={overview.total} />
            <div className="mt-4 grid grid-cols-2 gap-x-4 gap-y-2 sm:grid-cols-3">
              {overview.segments.map((s) =>
                s.value > 0 || BUCKETS.includes(s.id) ? (
                  <div key={s.id} className="flex min-w-0 items-start gap-2">
                    <span className="mt-1 h-3 w-3 shrink-0 rounded-sm" style={{ background: s.color }} />
                    <div className="min-w-0">
                      <div className="truncate text-xs text-text-dim">{s.label}</div>
                      <div className="pmgr-mono text-sm font-semibold text-text">{formatSize(s.value)}</div>
                    </div>
                  </div>
                ) : null,
              )}
            </div>
            {!summary.rules && <p className="mt-3 text-xs text-text-dim">{t('analyzer.noRulesNote')}</p>}
            {summary.truncated && <p className="mt-3 text-xs text-text-dim">{t('analyzer.truncatedNote')}</p>}
            {summary.scanned.errors > 0 && (
              <p className="mt-2 text-xs text-warn">{t('analyzer.errorsNote', { count: summary.scanned.errors })}</p>
            )}
            {summary.mounts?.length > 0 && (
              <p className="mt-2 text-xs text-text-dim">
                {t('analyzer.mountsNote', { paths: summary.mounts.join(', ') })}
              </p>
            )}
          </Panel>

          <Panel title={t('analyzer.otherTitle')}>
            <p className="mb-3 text-sm text-text-dim">{t('analyzer.otherIntro')}</p>
            {overview.others.length === 0 && overview.hidden === 0 ? (
              <EmptyState size="sm" title={t('analyzer.otherEmpty')} />
            ) : (
              <ul className="space-y-3">
                {overview.others.map((c) => (
                  <li key={c.id} className="rounded-lg border border-panel-border bg-bg/30 p-3">
                    <div className="flex items-baseline justify-between gap-3">
                      <span className="font-semibold text-text">{t(`analyzer.cat.${c.id}`)}</span>
                      <span className="pmgr-mono shrink-0 text-sm font-semibold text-text">{formatSize(metric(c))}</span>
                    </div>
                    <div className="mt-1.5">
                      <ShareBar value={metric(c)} total={overview.otherTotal} color={BUCKET_COLORS.other} />
                    </div>
                    <p className="mt-1.5 text-xs text-text-dim">{t(`analyzer.catDesc.${c.id}`)}</p>
                    {c.paths.length > 0 && (
                      <div className="mt-2 flex flex-wrap gap-1.5">
                        {[...c.paths].sort((a, b) => metric(b) - metric(a)).map((p) => (
                          <button
                            key={p.path}
                            type="button"
                            onClick={() => goTo(p.type === 'file' ? parentPath(p.path) : p.path)}
                            title={t('analyzer.showInExplorer')}
                            className="pmgr-mono max-w-full truncate rounded-md border border-panel-border bg-panel-2 px-2 py-1 text-left text-xs text-text-dim hover:border-accent/40 hover:text-text"
                            dir="ltr"
                          >
                            {p.path} <span className="text-text-dim/70">· {formatSize(metric(p))}</span>
                          </button>
                        ))}
                        {c.entries > c.paths.length && (
                          <span className="px-1 py-1 text-xs text-text-dim">
                            {t('analyzer.itemsMore', { count: c.entries - c.paths.length })}
                          </span>
                        )}
                      </div>
                    )}
                  </li>
                ))}
                {overview.hidden > 0 && (
                  <li className="rounded-lg border border-dashed border-panel-border p-3">
                    <div className="flex items-baseline justify-between gap-3">
                      <span className="font-semibold text-text">{t('analyzer.hidden')}</span>
                      <span className="pmgr-mono shrink-0 text-sm font-semibold text-text">{formatSize(overview.hidden)}</span>
                    </div>
                    <div className="mt-1.5">
                      <ShareBar value={overview.hidden} total={overview.otherTotal} color={BUCKET_COLORS.hidden} />
                    </div>
                    <p className="mt-1.5 text-xs text-text-dim">{t('analyzer.hiddenDesc')}</p>
                  </li>
                )}
              </ul>
            )}
          </Panel>
        </div>
      )}

      {summary && (
        <div ref={explorerRef}>
          <Panel
            title={t('analyzer.explorerTitle')}
            actions={
              <SegmentedControl
                label={t('analyzer.explorerTitle')}
                value={tab}
                onChange={setTab}
                options={[
                  { id: 'folders', label: t('analyzer.tabFolders') },
                  { id: 'largest', label: t('analyzer.tabLargest') },
                  { id: 'types', label: t('analyzer.tabTypes') },
                ]}
              />
            }
          >
            {tab === 'folders' && (
              <>
                <div className="mb-3 flex flex-wrap items-center gap-2">
                  <Button
                    size="sm"
                    tone="neutral"
                    icon={<GoUpIcon size={14} />}
                    onClick={() => openTree(parentPath(path))}
                    disabled={!path || path === summary.root || treeLoading}
                  >
                    {t('analyzer.up')}
                  </Button>
                  <nav className="pmgr-mono flex min-w-0 flex-1 flex-wrap items-center gap-1 text-xs" dir="ltr" aria-label={t('analyzer.explorerTitle')}>
                    {crumbs.map((c, i) => (
                      <span key={c.path} className="flex items-center gap-1">
                        {i > 0 && <span className="text-text-dim/60">/</span>}
                        <button
                          type="button"
                          onClick={() => openTree(c.path)}
                          className={`rounded px-1 py-0.5 hover:bg-panel-border ${i === crumbs.length - 1 ? 'font-semibold text-text' : 'text-text-dim'}`}
                        >
                          {c.label}
                        </button>
                      </span>
                    ))}
                  </nav>
                  <Button size="sm" tone="ghost" icon={<ExternalIcon size={14} />} onClick={() => openInFiles(path)} disabled={!path}>
                    {t('analyzer.openInFiles')}
                  </Button>
                </div>

                <div className="grid gap-4 lg:grid-cols-12">
                  <div className="lg:col-span-7 lg:order-2">
                    <Treemap
                      className={`h-[320px] md:h-[480px] ${treeLoading ? 'opacity-60' : ''}`}
                      node={node}
                      metric={metric}
                      formatSize={formatSize}
                      describe={describe}
                      onOpen={(chain) => openTree(chain.reduce((p, n) => joinPath(p, n.name), path))}
                    />
                    <p className="mt-2 text-xs text-text-dim">{t('analyzer.treemapHint')}</p>
                  </div>
                  <div className="lg:col-span-5 lg:order-1">
                    {node && (
                      <div className="mb-2 flex items-baseline justify-between text-xs text-text-dim">
                        <span>{t('analyzer.filesCount', { count: node.files || 0 })}</span>
                        <span className="pmgr-mono font-semibold text-text">{formatSize(nodeTotal)}</span>
                      </div>
                    )}
                    <div className="max-h-[480px] overflow-y-auto rounded-lg border border-panel-border">
                      {children.length === 0 ? (
                        <EmptyState size="sm" title={t('analyzer.emptyFolder')} />
                      ) : (
                        <ul>
                          {children.map((c, i) => {
                            const openable = c.type === 'dir'
                            const label = describe(c).name
                            return (
                              <li key={`${c.type}-${c.name || i}`} className="border-t border-panel-border first:border-t-0">
                                <button
                                  type="button"
                                  disabled={!openable}
                                  onClick={() => openTree(joinPath(path, c.name))}
                                  className="flex w-full items-center gap-3 px-3 py-2 text-left transition-colors hover:bg-panel-2 disabled:cursor-default disabled:hover:bg-transparent"
                                >
                                  <span className={`shrink-0 ${c.type === 'dir' ? 'text-accent' : 'text-text-dim'}`}>
                                    {c.type === 'file' ? <FileIcon size={16} /> : <FolderIcon size={16} />}
                                  </span>
                                  <span className="min-w-0 flex-1">
                                    <span className="flex items-center gap-2">
                                      <span className={`truncate text-sm ${c.type === 'rest' || c.type === 'files' ? 'italic text-text-dim' : 'text-text'}`}>
                                        {label}
                                      </span>
                                      {c.title && <span className="shrink-0 rounded bg-accent/15 px-1.5 text-[11px] font-semibold text-accent">{t('analyzer.titleBadge')}</span>}
                                      {c.type === 'mount' && <span className="shrink-0 rounded bg-panel-border px-1.5 text-[11px] text-text-dim">{t('analyzer.mountItem')}</span>}
                                      {c.unreadable && <span className="shrink-0 rounded bg-err/10 px-1.5 text-[11px] text-err">{t('analyzer.unreadable')}</span>}
                                    </span>
                                    <span className="mt-1 block">
                                      <ShareBar value={metric(c)} total={nodeTotal} />
                                    </span>
                                  </span>
                                  <span className="w-20 shrink-0 text-right">
                                    <span className="pmgr-mono block text-sm text-text">{formatSize(metric(c))}</span>
                                    <span className="pmgr-mono block text-[11px] text-text-dim">
                                      {nodeTotal > 0 ? `${((metric(c) / nodeTotal) * 100).toFixed(1)}%` : ''}
                                    </span>
                                  </span>
                                </button>
                              </li>
                            )
                          })}
                        </ul>
                      )}
                    </div>
                  </div>
                </div>
              </>
            )}

            {tab === 'largest' && (
              <div className="overflow-x-auto">
                <table className="w-full text-left text-sm">
                  <thead>
                    <tr className="text-text-dim">
                      <th className="pb-2 font-medium">{t('analyzer.colPath')}</th>
                      <th className="pb-2 text-right font-medium">{t('analyzer.colSize')}</th>
                      <th className="pb-2" />
                    </tr>
                  </thead>
                  <tbody>
                    {(fileStats?.largest || []).map((f) => (
                      <tr key={f.path} className="border-t border-panel-border hover:bg-panel-2">
                        <td className="pmgr-mono max-w-0 truncate py-1.5 pr-3 text-xs text-text" dir="ltr" title={f.path}>{f.path}</td>
                        <td className="pmgr-mono whitespace-nowrap py-1.5 text-right text-text">{formatSize(metric(f))}</td>
                        <td className="w-24 whitespace-nowrap py-1.5 text-right">
                          <Button size="sm" tone="ghost" onClick={() => goTo(parentPath(f.path))}>{t('analyzer.show')}</Button>
                        </td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )}

            {tab === 'types' && (
              <div className="overflow-x-auto">
                <table className="w-full text-left text-sm">
                  <thead>
                    <tr className="text-text-dim">
                      <th className="pb-2 font-medium">{t('analyzer.colType')}</th>
                      <th className="pb-2 font-medium">{t('analyzer.colShare')}</th>
                      <th className="pb-2 text-right font-medium">{t('analyzer.colFiles')}</th>
                      <th className="pb-2 text-right font-medium">{t('analyzer.colSize')}</th>
                    </tr>
                  </thead>
                  <tbody>
                    {[...(fileStats?.types || [])].sort((a, b) => metric(b) - metric(a)).map((x) => (
                      <tr key={x.ext} className="border-t border-panel-border">
                        <td className="pmgr-mono py-1.5 pr-3 text-text">
                          {x.ext === '' ? t('analyzer.noExtension') : x.ext === '*' ? t('analyzer.otherTypes') : `.${x.ext}`}
                        </td>
                        <td className="w-1/3 py-1.5 pr-3"><ShareBar value={metric(x)} total={overview?.scanned || 0} /></td>
                        <td className="pmgr-mono py-1.5 text-right text-text-dim">{formatNumber(x.files, locale)}</td>
                        <td className="pmgr-mono whitespace-nowrap py-1.5 text-right text-text">{formatSize(metric(x))}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
              </div>
            )}
          </Panel>
        </div>
      )}
    </div>
  )
}
