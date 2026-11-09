import { useEffect, useMemo, useRef, useState } from 'react'
import Panel from '../components/Panel.jsx'
import Button from '../components/Button.jsx'
import SearchField from '../components/SearchField.jsx'
import EmptyState from '../components/EmptyState.jsx'
import ActionMenu from '../components/ActionMenu.jsx'
import { ConfirmDialog, InputDialog } from '../components/Dialog.jsx'
import DownloadPayloadDialog from '../components/DownloadPayloadDialog.jsx'
import { RefreshIcon, ExternalIcon, GithubIcon, PlusIcon, DeleteIcon, DownloadIcon, ShieldIcon } from '../components/icons.jsx'
import { api } from '../lib/api.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'
import { formatDate } from '../lib/format.js'

function hostBadge(url) {
  try {
    return new URL(url).hostname === 'github.com' ? 'GitHub' : 'Gitea'
  } catch {
    return null
  }
}

// Hand-curated, developer-grouped directory — Open links out to the upstream repo, Download
// installs a release into the Payloads tab. GITHUB_TOKEN_URL is where a token is created (rate-limit hint).
const GITHUB_TOKEN_URL = 'https://github.com/settings/tokens'

export default function RepositoryView() {
  const { t, locale } = useI18n()
  const showToast = useToast()
  const [entries, setEntries] = useState([])
  const [error, setError] = useState('')
  const [search, setSearch] = useState('')
  const [loading, setLoading] = useState(false)
  const [refreshInfo, setRefreshInfo] = useState(null)
  const [addOpen, setAddOpen] = useState(false)
  const [removeTarget, setRemoveTarget] = useState(null)
  const [downloadTarget, setDownloadTarget] = useState(null)
  const [checking, setChecking] = useState(() => new Set())
  const [hasToken, setHasToken] = useState(false)
  const [tokenOpen, setTokenOpen] = useState(false)
  const pollRef = useRef(null)

  useEffect(() => {
    api.catalog.getGithubToken().then((r) => setHasToken(Boolean(r.hasToken))).catch(() => {})
  }, [])

  const saveToken = (token) =>
    api.catalog.setGithubToken(token).then(() => {
      setHasToken(true)
      showToast(t('repositories.tokenSaved'))
    })

  const clearToken = () =>
    api.catalog.clearGithubToken().then(() => {
      setHasToken(false)
      showToast(t('repositories.tokenCleared'))
    })

  const load = (refresh, polling = false) => {
    if (!polling) {
      clearTimeout(pollRef.current)
      setLoading(true)
      setError('')
    }
    api.catalog.list(refresh).then((result) => {
      setEntries(result.entries || [])
      setLoading(Boolean(result.refreshing))
      setRefreshInfo(result.refreshing ? result : null)
      if (result.refreshing) {
        pollRef.current = window.setTimeout(() => load(false, true), 750)
      }
    }).catch((requestError) => {
      setLoading(false)
      setError(requestError.message)
    })
  }

  useEffect(() => load(false), [])
  useEffect(() => () => clearTimeout(pollRef.current), [])

  const groups = useMemo(() => {
    const byDeveloper = new Map()
    const query = search.trim().toLocaleLowerCase(locale)
    for (const entry of entries) {
      if (query && !`${entry.developer} ${entry.name} ${entry.description}`.toLocaleLowerCase(locale).includes(query)) continue
      if (!byDeveloper.has(entry.developer)) byDeveloper.set(entry.developer, [])
      byDeveloper.get(entry.developer).push(entry)
    }
    return [...byDeveloper.entries()].sort((a, b) => {
      if (a[0] === 'maj0r') return -1
      if (b[0] === 'maj0r') return 1
      return a[0].localeCompare(b[0], locale, { sensitivity: 'base' })
    })
  }, [entries, search, locale])

  const visibleCount = groups.reduce((total, [, items]) => total + items.length, 0)

  const addRepository = (url) =>
    api.catalog.addSource(url).then((result) => {
      showToast(t('repositories.addedToast', { name: result.entry?.name || url }))
      load(true)
    })

  // One entry, one upstream request — the whole-catalog sweep is what burns
  // GitHub's unauthenticated hourly budget, so it stays behind Check all.
  const refreshOne = (entry) => {
    setChecking((current) => new Set(current).add(entry.url))
    api.catalog.refreshEntry(entry.url)
      .then((result) => {
        setEntries((current) =>
          current.map((item) =>
            item.url === entry.url
              ? { ...item, version: result.version, version_state: result.state, version_error: result.error, checked_at: result.checked_at }
              : item,
          ),
        )
        if (result.state !== 'ok' && result.error) showToast(result.error, 'err')
      })
      .catch((e) => showToast(e.message, 'err'))
      .finally(() =>
        setChecking((current) => {
          const next = new Set(current)
          next.delete(entry.url)
          return next
        }),
      )
  }

  const removeRepository = () => {
    const entry = removeTarget
    return api.catalog.removeSource(entry.url).then(() => {
      showToast(t('repositories.removedToast', { name: entry.name }))
      load(false)
    })
  }

  return (
    <div className="space-y-4">
      <Panel
        title={t('repositories.title')}
        actions={
          <div className="mt-3 grid w-full grid-cols-1 gap-2 sm:mt-0 sm:flex sm:w-auto sm:items-center">
            <Button tone="neutral" icon={<PlusIcon size={15} />} onClick={() => setAddOpen(true)}>
              {t('repositories.addRepository')}
            </Button>
            <Button tone="neutral" icon={<RefreshIcon size={15} />} disabled={loading} title={t('repositories.checkAllHint')} onClick={() => load(true)}>
              {loading ? t('repositories.checking') : t('repositories.checkForUpdates')}
            </Button>
          </div>
        }
      >
        <div className="mb-4 max-w-3xl">
          <h2 className="text-lg font-bold text-text">{t('repositories.heroTitle')}</h2>
          <p className="mt-1 text-sm leading-relaxed text-text-dim">{t('repositories.heroDesc')}</p>
        </div>

        <div className="mb-4 flex flex-wrap items-center justify-between gap-3 rounded-xl border border-panel-border bg-bg/45 px-3.5 py-3 text-sm">
          <div className="flex min-w-0 items-center gap-2.5 text-text-dim">
            <ShieldIcon size={15} className={hasToken ? 'text-ok' : 'text-text-dim'} />
            <span>{hasToken ? t('repositories.tokenActive') : t('repositories.tokenHint')}</span>
          </div>
          {hasToken ? (
            <Button size="sm" tone="ghost" onClick={clearToken}>{t('common.remove')}</Button>
          ) : (
            <div className="flex items-center gap-2">
              <Button
                size="sm"
                tone="ghost"
                icon={<ExternalIcon size={13} />}
                onClick={() => window.open(GITHUB_TOKEN_URL, '_blank', 'noopener,noreferrer')}
              >
                {t('repositories.tokenCreateLink')}
              </Button>
              <Button size="sm" tone="neutral" onClick={() => setTokenOpen(true)}>{t('repositories.addToken')}</Button>
            </div>
          )}
        </div>

        {loading && (
          <div className="mb-4 flex items-start gap-3 rounded-xl border border-accent/25 bg-accent/10 px-3.5 py-3 text-sm text-accent">
            <span className="pmgr-live-dot mt-1 shrink-0" />
            <div className="min-w-0">
              <div className="font-semibold">{t('repositories.checkingReleases')}</div>
              <div className="mt-0.5 truncate text-xs opacity-80">
                {refreshInfo?.total > 0 ? t('repositories.checkedOfTotal', { completed: refreshInfo.completed, total: refreshInfo.total }) : t('repositories.preparing')}
              </div>
            </div>
          </div>
        )}

        {error && <div className="pmgr-notice-in mb-4 rounded-xl border border-err/25 bg-err/10 px-3.5 py-3 text-sm text-err">{error}</div>}

        <div className="mb-5 flex flex-col gap-2 sm:flex-row sm:items-center">
          <SearchField value={search} onChange={setSearch} placeholder={t('repositories.searchPlaceholder')} className="flex-1" />
          <span className="w-fit rounded-full border border-panel-border bg-bg/55 px-3 py-1.5 text-xs font-semibold text-text-dim">
            {t('repositories.showingOfTotal', { shown: visibleCount, total: entries.length })}
          </span>
        </div>

        <div className="space-y-3">
          {groups.map(([developer, items], groupIndex) => (
            <section
              key={developer}
              className="pmgr-row-in overflow-hidden rounded-xl border border-panel-border bg-bg/25"
              style={{ '--ti': groupIndex }}
            >
              <div className="flex items-center gap-3 border-b border-panel-border bg-panel-2/40 px-4 py-3">
                <span className="grid h-9 w-9 shrink-0 place-items-center rounded-lg bg-accent/10 text-accent">
                  <svg width="17" height="17" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" aria-hidden="true">
                    <path d="M12 3 4 7v10l8 4 8-4V7l-8-4Z" /><path d="m4 7 8 4 8-4M12 11v10" />
                  </svg>
                </span>
                <div className="min-w-0 flex-1">
                  <h3 className="truncate text-sm font-bold text-text">{developer}</h3>
                  {developer === 'maj0r' && (
                    <div className="text-xs font-semibold text-accent">{t('repositories.creatorBio')}</div>
                  )}
                  <div className="text-xs text-text-dim">{t('repositories.payloadsCount', { count: items.length })}</div>
                </div>
              </div>

              <div className="grid gap-2 p-3 lg:grid-cols-2">
                {items.map((entry) => (
                  <article
                    key={entry.url}
                    data-menu-host
                    className="group flex min-w-0 flex-col rounded-xl border border-panel-border bg-panel p-3.5 transition-[border-color,box-shadow,transform] duration-200 hover:-translate-y-0.5 hover:border-accent/35 hover:shadow-[0_10px_26px_-14px_rgba(0,0,0,0.5)] focus-within:-translate-y-0.5 focus-within:border-accent/35"
                  >
                    <div className="flex min-w-0 items-start justify-between gap-2">
                      <div className="min-w-0 flex-1">
                        <div className="flex min-w-0 items-center gap-1.5">
                          <span className="truncate font-bold text-text">{entry.name}</span>
                          {entry.custom && (
                            <span className="shrink-0 rounded-md border border-accent/30 bg-accent/10 px-1.5 py-0.5 text-[10px] font-bold uppercase tracking-wide text-accent">
                              {t('common.custom')}
                            </span>
                          )}
                        </div>
                        <p className="mt-1 text-xs leading-relaxed text-text-dim">{entry.description}</p>
                      </div>
                      {entry.custom && (
                        <ActionMenu
                          label={t('common.moreActions')}
                          align="right"
                          items={[
                            { label: t('common.remove'), icon: <DeleteIcon size={15} />, tone: 'danger', onClick: () => setRemoveTarget(entry) },
                          ]}
                        />
                      )}
                    </div>

                    <div className="mt-3 flex flex-wrap items-center gap-1.5 text-xs text-text-dim">
                      <VersionBadge entry={entry} busy={checking.has(entry.url)} t={t} />
                      <button
                        type="button"
                        onClick={() => refreshOne(entry)}
                        disabled={checking.has(entry.url)}
                        title={t('repositories.checkThis')}
                        aria-label={t('repositories.checkThis')}
                        className="pmgr-icon-btn grid h-7 w-7 shrink-0 place-items-center rounded-lg text-text-dim hover:text-text disabled:opacity-50"
                      >
                        <RefreshIcon size={13} />
                      </button>
                      {hostBadge(entry.url) && (
                        <span className="inline-flex items-center gap-1 rounded-md bg-bg px-2 py-1">
                          {hostBadge(entry.url) === 'GitHub' && <GithubIcon size={11} />} {hostBadge(entry.url)}
                        </span>
                      )}
                      <span className="ml-auto text-[11px] opacity-70">
                        {entry.checked_at
                          ? t('repositories.checkedAgo', { date: formatDate(entry.checked_at * 1000, locale, { dateStyle: 'short', timeStyle: 'short' }) })
                          : t('repositories.neverChecked')}
                      </span>
                    </div>

                    <div className="mt-3 flex justify-end gap-2">
                      <Button
                        tone="ghost"
                        icon={<ExternalIcon size={15} className="transition-transform duration-200 group-hover:translate-x-0.5" />}
                        onClick={() => window.open(entry.url, '_blank', 'noopener,noreferrer')}
                      >
                        {t('repositories.open')}
                      </Button>
                      <Button tone="primary" icon={<DownloadIcon size={15} />} onClick={() => setDownloadTarget(entry)}>
                        {t('common.download')}
                      </Button>
                    </div>
                  </article>
                ))}
              </div>
            </section>
          ))}

          {!loading && groups.length === 0 && (
            <EmptyState title={t('repositories.nothingMatchesTitle')} description={t('repositories.nothingMatchesDesc')} />
          )}
          {loading && entries.length === 0 && (
            <div className="grid gap-2 lg:grid-cols-2">
              {[0, 1, 2, 3].map((index) => <div key={index} className="pmgr-skeleton h-28 rounded-xl" />)}
            </div>
          )}
        </div>
      </Panel>

      {addOpen && (
        <InputDialog
          title={t('repositories.addRepositoryTitle')}
          description={t('repositories.addRepositoryDesc')}
          label={t('repositories.urlLabel')}
          placeholder={t('repositories.urlPlaceholder')}
          confirmLabel={t('repositories.addConfirm')}
          cancelLabel={t('common.cancel')}
          onClose={() => setAddOpen(false)}
          onConfirm={addRepository}
        />
      )}

      {removeTarget && (
        <ConfirmDialog
          title={t('repositories.removeTitle')}
          description={t('repositories.removeDesc')}
          detail={removeTarget.name}
          confirmLabel={t('repositories.removeConfirm')}
          cancelLabel={t('common.cancel')}
          tone="danger"
          onClose={() => setRemoveTarget(null)}
          onConfirm={removeRepository}
        />
      )}

      {downloadTarget && (
        <DownloadPayloadDialog
          entry={downloadTarget}
          onClose={() => setDownloadTarget(null)}
          onDownloaded={() => showToast(t('repositories.downloadStarted', { name: downloadTarget.name }))}
        />
      )}

      {tokenOpen && (
        <InputDialog
          title={t('repositories.addTokenTitle')}
          description={t('repositories.addTokenDesc')}
          label={t('repositories.tokenLabel')}
          placeholder="ghp_..."
          type="password"
          confirmLabel={t('common.save')}
          cancelLabel={t('common.cancel')}
          onClose={() => setTokenOpen(false)}
          onConfirm={saveToken}
        />
      )}
    </div>
  )
}

function VersionBadge({ entry, busy, t }) {
  if (busy) {
    return <span className="pmgr-mono rounded-md bg-bg px-2 py-1 opacity-60">{t('repositories.checking')}</span>
  }
  // No state means nobody has checked yet — never an in-flight request, since
  // checks only happen when the user asks for one.
  if (!entry.version_state) {
    return <span className="rounded-md bg-bg px-2 py-1 opacity-60">{t('repositories.versionUnknown')}</span>
  }
  if (entry.version_state === 'ok') {
    return <span className="pmgr-mono rounded-md bg-bg px-2 py-1" dir="ltr">{entry.version}</span>
  }
  return (
    <span className="rounded-md bg-bg px-2 py-1 opacity-70" title={entry.version_error}>
      {t('repositories.versionUnknown')}
    </span>
  )
}
