import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import Panel from '../components/Panel.jsx'
import Button from '../components/Button.jsx'
import EmptyState from '../components/EmptyState.jsx'
import FadeImage from '../components/FadeImage.jsx'
import { ConfirmDialog } from '../components/Dialog.jsx'
import SegmentedControl from '../components/SegmentedControl.jsx'
import { CheckIcon, ChevronRightIcon, CloseIcon, DeleteIcon, DownloadIcon, ImageIcon, RefreshIcon, AlertTriangleIcon } from '../components/icons.jsx'
import { api } from '../lib/api.js'
import { formatDate, formatFileSize } from '../lib/format.js'
import { isPS5 } from '../lib/effects.js'
import { useDismiss } from '../lib/useDismiss.js'
import { useHistoryState } from '../lib/useHistoryState.js'
import { useDialogAccessibility } from '../lib/useDialogAccessibility.js'
import { useI18n } from '../lib/i18n.jsx'
import { useToast } from '../lib/ToastContext.jsx'

const PAGE = isPS5() ? 24 : 60
const UNKNOWN = 'unknown'
const DELETED = 'deleted'
const SYSTEM = 'system'

const PlayBadge = () => (
  <span className="grid h-10 w-10 place-items-center rounded-full bg-black/55 text-white">
    <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M8 5v14l11-7Z" /></svg>
  </span>
)

// Browsers hold back a burst of downloads, so they start one after another.
function downloadAll(list) {
  list.forEach((item, i) => {
    window.setTimeout(() => {
      const a = document.createElement('a')
      a.href = api.media.fileUrl(item.path, true)
      a.download = item.name
      a.click()
    }, i * 600)
  })
}

const Tile = memo(function Tile({ item, gameName, selected, showGame, onOpen, onToggle, label, locale }) {
  const [failed, setFailed] = useState(false)
  const [ready, setReady] = useState(false)
  const video = item.type === 'video'
  return (
    <div className={`group relative overflow-hidden rounded-xl border bg-panel ${selected ? 'border-accent' : 'border-panel-border'}`}>
      <button type="button" onClick={() => onOpen(item)} aria-label={item.name} className="relative block w-full overflow-hidden bg-panel-2 text-left" style={{ paddingBottom: '56.25%' }}>
        {video && !failed && !isPS5() ? (
          // The browser draws the first frame itself; the console cannot decode clips to make a thumbnail.
          <video
            src={`${api.media.fileUrl(item.path)}#t=0.5`}
            muted
            playsInline
            preload="metadata"
            onLoadedData={() => setReady(true)}
            onError={() => setFailed(true)}
            className={`pmgr-img-fade ${ready ? 'is-loaded' : ''} absolute left-0 top-0 h-full w-full object-cover`}
          />
        ) : !video && !failed ? (
          <FadeImage src={api.media.thumbUrl(item.path)} alt="" loading="lazy" decoding="async" onError={() => setFailed(true)} className="absolute left-0 top-0 h-full w-full object-cover" />
        ) : (
          <span className="absolute left-0 top-0 grid h-full w-full place-items-center text-text-dim"><ImageIcon size={26} /></span>
        )}
        {video && <span className="absolute left-0 top-0 grid h-full w-full place-items-center"><PlayBadge /></span>}
        <span className="absolute bottom-0 left-0 right-0 bg-gradient-to-t from-black/80 via-black/40 to-transparent px-3.5 pb-3 pt-12 text-white [text-shadow:0_1px_3px_rgba(0,0,0,0.6)]">
          {showGame && <span className="block truncate text-base font-bold leading-tight">{gameName}</span>}
          <span className="mt-0.5 block truncate text-sm font-semibold opacity-90">{formatDate(item.captured, locale, { timeStyle: 'short' })}</span>
        </span>
      </button>
      <button
        type="button"
        role="checkbox"
        aria-checked={selected}
        aria-label={label}
        onClick={() => onToggle(item.path)}
        className={`absolute left-2 top-2 grid h-6 w-6 place-items-center rounded-md border text-white transition-colors ${selected ? 'border-accent bg-accent' : 'border-white/60 bg-black/40 hover:bg-black/60'}`}
      >
        {selected && <CheckIcon size={14} />}
      </button>
    </div>
  )
})

function Viewer({ items, index, gameNames, onIndex, onClose, onDelete }) {
  const { t, locale } = useI18n()
  const { closing, close } = useDismiss(onClose)
  const containerRef = useDialogAccessibility(close)
  const [failed, setFailed] = useState(false)
  const item = items[index]
  const video = item.type === 'video'

  useEffect(() => setFailed(false), [item.path])

  useEffect(() => {
    if (isPS5()) return undefined
    const onKey = (e) => {
      if (e.key === 'ArrowRight' && index < items.length - 1) onIndex(index + 1)
      if (e.key === 'ArrowLeft' && index > 0) onIndex(index - 1)
    }
    document.addEventListener('keydown', onKey)
    return () => document.removeEventListener('keydown', onKey)
  }, [index, items.length, onIndex])

  return createPortal(
    <div className={`pmgr-modal-overlay !items-stretch !p-0 sm:!items-center sm:!p-4 ${closing ? 'is-closing' : ''}`} onClick={close}>
      <div
        ref={containerRef}
        role="dialog"
        aria-modal="true"
        aria-label={item.name}
        className={`pmgr-panel pmgr-modal-pop pmgr-h-dvh-100 pmgr-h-dvh-85-sm flex w-full flex-col overflow-hidden border-panel-border sm:max-w-5xl sm:rounded-2xl sm:border ${closing ? 'is-closing' : ''}`}
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex shrink-0 items-center border-b border-panel-border px-4 py-3">
          <div className="min-w-0 flex-1">
            <h2 className="truncate text-sm font-bold text-text">{gameNames[item.game_id] || t('media.unknownGame')}</h2>
            <div className="truncate text-xs text-text-dim">
              {formatDate(item.captured, locale)} · <span className="pmgr-num">{formatFileSize(item.size, locale)}</span>
            </div>
          </div>
          <a href={api.media.fileUrl(item.path, true)} download={item.name} className="mr-2 inline-flex">
            <Button tone="primary" title={t('media.download')} aria-label={t('media.download')} icon={<DownloadIcon size={15} />}><span className="hidden sm:inline">{t('media.download')}</span></Button>
          </a>
          <Button className="mr-2" tone="danger" title={t('common.delete')} aria-label={t('common.delete')} icon={<DeleteIcon size={15} />} onClick={() => onDelete(item)}><span className="hidden sm:inline">{t('common.delete')}</span></Button>
          <Button size="icon" tone="ghost" title={t('common.close')} aria-label={t('common.close')} onClick={close} icon={<CloseIcon size={16} />} />
        </div>

        <div className="relative flex min-h-0 flex-1 items-center justify-center bg-black/40 p-2 sm:p-4">
          {failed ? (
            <div className="text-center text-sm text-text-dim">
              <span className="mx-auto mb-2 grid h-11 w-11 place-items-center rounded-xl bg-err/15 text-err"><AlertTriangleIcon size={20} /></span>
              {video ? t('media.noPlayback') : t('imagePreview.loadFailed')}
            </div>
          ) : video ? (
            <video key={item.path} src={api.media.fileUrl(item.path)} controls autoPlay playsInline onError={() => setFailed(true)} className="max-h-full max-w-full rounded-lg" />
          ) : (
            <FadeImage key={item.path} src={api.media.fileUrl(item.path)} alt={item.name} onError={() => setFailed(true)} className="max-h-full max-w-full rounded-lg object-contain" />
          )}
          {index > 0 && (
            <button type="button" aria-label={t('media.previous')} onClick={() => onIndex(index - 1)} className="absolute left-3 top-1/2 grid h-11 w-11 -translate-y-1/2 place-items-center rounded-full bg-black/55 text-white hover:bg-black/75">
              <span style={{ transform: 'rotate(180deg)', display: 'inline-flex' }}><ChevronRightIcon size={20} /></span>
            </button>
          )}
          {index < items.length - 1 && (
            <button type="button" aria-label={t('media.next')} onClick={() => onIndex(index + 1)} className="absolute right-3 top-1/2 grid h-11 w-11 -translate-y-1/2 place-items-center rounded-full bg-black/55 text-white hover:bg-black/75">
              <ChevronRightIcon size={20} />
            </button>
          )}
        </div>
      </div>
    </div>,
    document.body,
  )
}

export default function MediaView() {
  const { t, locale } = useI18n()
  const showToast = useToast()
  const [items, setItems] = useState(null)
  const [leftovers, setLeftovers] = useState(0)
  const [titles, setTitles] = useState({})
  const [error, setError] = useState('')
  const [kind, setKind] = useState('all')
  // The tab and the open album are history steps, so Back leaves an album before it leaves the page.
  const [nav, setNav] = useHistoryState('media', { tab: 'all', album: 'all' }, '#/media')
  const { tab, album } = nav
  const [limit, setLimit] = useState(PAGE)
  const [selected, setSelected] = useState(() => new Set())
  const [viewing, setViewing] = useState(null)
  const [deleting, setDeleting] = useState(null)
  const [zipping, setZipping] = useState(false)

  const load = () => {
    setError('')
    Promise.all([api.media.list(), api.titles.list().catch(() => ({ titles: [] }))])
      .then(([media, library]) => {
        setItems(media.items || [])
        setLeftovers(media.leftovers || 0)
        setTitles(Object.fromEntries((library.titles || []).map((title) => [title.title_id, title])))
      })
      .catch((e) => setError(e.message))
  }
  useEffect(load, [])
  useEffect(() => setLimit(PAGE), [kind, album])
  useEffect(() => setSelected(new Set()), [tab, album])

  // Captures of games with no known name share one album, like the console gallery.
  const hasLibrary = Object.keys(titles).length > 0
  const gameKey = (item) => (!item.game_id ? UNKNOWN : item.game_id.startsWith('NPXS') ? SYSTEM : hasLibrary && !titles[item.game_id] && !item.game_name ? DELETED : item.game_id)
  const gameNames = useMemo(() => {
    const names = { [UNKNOWN]: t('media.unknownGame'), [DELETED]: t('media.deletedGames'), [SYSTEM]: t('media.systemScreens') }
    for (const item of items || []) {
      const key = gameKey(item)
      if (key !== DELETED && key !== UNKNOWN && key !== SYSTEM) names[key] = titles[item.game_id]?.name || item.game_name || item.game_id
    }
    return names
  }, [items, titles, t])

  const albums = useMemo(() => {
    const byGame = new Map()
    for (const item of items || []) {
      const key = gameKey(item)
      if (!byGame.has(key)) byGame.set(key, { key, id: key === DELETED || key === SYSTEM ? '' : item.game_id, photos: 0, videos: 0, size: 0, latest: item.captured, cover: item })
      const entry = byGame.get(key)
      entry.size += item.size || 0
      if (item.captured > entry.latest) { entry.latest = item.captured; entry.cover = item }
      if (item.type === 'video') entry.videos++
      else entry.photos++
    }
    return [...byGame.values()].sort((a, b) => b.latest.localeCompare(a.latest))
  }, [items])

  const shown = useMemo(
    () => (items || []).filter((i) => (album === 'all' || gameKey(i) === album) && (kind === 'all' || i.type === kind)),
    [items, album, kind],
  )
  const visible = shown.slice(0, limit)

  const days = useMemo(() => {
    const groups = []
    for (const item of visible) {
      const day = item.captured.slice(0, 10)
      if (!groups.length || groups[groups.length - 1].day !== day) groups.push({ day, items: [] })
      groups[groups.length - 1].items.push(item)
    }
    return groups
  }, [visible])

  const counts = {
    all: (items || []).filter((i) => album === 'all' || gameKey(i) === album).length,
    photo: (items || []).filter((i) => (album === 'all' || gameKey(i) === album) && i.type === 'photo').length,
    video: (items || []).filter((i) => (album === 'all' || gameKey(i) === album) && i.type === 'video').length,
  }

  const pickingAlbum = tab === 'albums' && album === 'all'
  const changeTab = (next) => setNav({ tab: next, album: 'all' })

  // Stable handlers, so selecting one tile does not re-render the whole grid.
  const toggle = useCallback((path) => setSelected((prev) => {
    const next = new Set(prev)
    if (next.has(path)) next.delete(path)
    else next.add(path)
    return next
  }), [])
  const shownRef = useRef(shown)
  shownRef.current = shown
  const open = useCallback((item) => setViewing(shownRef.current.findIndex((i) => i.path === item.path)), [])
  const confirmDelete = async () => {
    const paths = deleting.map((i) => i.path)
    const result = await api.media.remove(paths)
    if (result.failed > 0) showToast(t('media.deleteFailed', { count: result.failed }), 'err')
    else showToast(t('media.deleted', { count: result.deleted }))
    // Reloading keeps anything the console refused to delete in the list.
    if (result.failed > 0) load()
    else setItems((prev) => prev.filter((i) => !paths.includes(i.path)))
    setSelected(new Set())
    setViewing(null)
  }
  // The console builds the archive as a job; the browser gets it once the job is done.
  const downloadZip = async (list) => {
    setZipping(true)
    showToast(t('media.preparingZip'))
    try {
      const job = await api.ftp.downloadZip(list.map((i) => i.path), `Media-${new Date().toISOString().slice(0, 10)}.zip`)
      for (;;) {
        await new Promise((resolve) => window.setTimeout(resolve, 1000))
        const mine = ((await api.ftp.jobs()).jobs || []).find((j) => j.id === job.job_id)
        if (!mine || ['error', 'attention', 'cancelled'].includes(mine.state)) throw new Error(t('media.zipFailed'))
        if (mine.state === 'done') break
      }
      const a = document.createElement('a')
      a.href = api.ftp.downloadUrl(job.download_path)
      a.download = job.download_path.split('/').pop()
      a.click()
    } catch (e) {
      showToast(e.message, 'err')
    } finally {
      setZipping(false)
    }
  }
  const allSelected = shown.length > 0 && shown.every((i) => selected.has(i.path))
  const cleanUp = async () => {
    try {
      const result = await api.media.cleanup()
      showToast(t(result.failed > 0 ? 'media.deleteFailed' : 'media.cleanedUp', { count: result.failed > 0 ? result.failed : result.deleted }), result.failed > 0 ? 'err' : undefined)
    } catch (e) {
      showToast(e.message, 'err')
    }
    load()
  }
  const downloadSelected = () => {
    downloadAll(shown.filter((i) => selected.has(i.path)))
    showToast(t('media.downloading', { count: selected.size }))
    setSelected(new Set())
  }

  return (
    <Panel
      title={t('nav.media')}
      actions={<Button tone="neutral" icon={<RefreshIcon size={15} />} onClick={load}>{t('common.refresh')}</Button>}
    >
      {error && (
        <div className="pmgr-notice-in mb-3 flex items-center justify-between rounded-xl border border-err/20 bg-err/10 px-3 py-2.5 text-sm text-err">
          <span className="mr-3">{error}</span>
          <Button size="sm" tone="danger" onClick={load}>{t('common.tryAgain')}</Button>
        </div>
      )}

      {leftovers > 0 && (
        <div className="pmgr-notice-in mb-3 flex flex-wrap items-center justify-between rounded-xl border border-warn/30 bg-warn/10 px-3 py-2.5 text-sm text-warn">
          <span className="mr-3">{t('media.leftoversNotice', { count: leftovers })}</span>
          <Button size="sm" tone="neutral" onClick={cleanUp}>{t('media.cleanUp')}</Button>
        </div>
      )}

      {items === null && !error ? (
        <div className="grid gap-3" style={{ gridTemplateColumns: 'repeat(auto-fill, minmax(280px, 1fr))' }}>
          {Array.from({ length: 8 }, (_, i) => <div key={i} className="pmgr-skeleton rounded-xl" style={{ paddingBottom: '56.25%' }} />)}
        </div>
      ) : items !== null && items.length === 0 ? (
        <EmptyState icon={<ImageIcon size={22} />} title={t('media.emptyTitle')} description={t('media.emptyDesc')} />
      ) : items !== null && (
        <>
          <div className="mb-4">
            <SegmentedControl
              options={[
                { id: 'all', label: t('media.all') },
                { id: 'albums', label: t('media.albums'), count: albums.length },
              ]}
              value={tab}
              onChange={changeTab}
            />
          </div>

          {pickingAlbum && (
            <div className="grid gap-3" style={{ gridTemplateColumns: `repeat(auto-fill, minmax(${isPS5() ? 280 : 250}px, 1fr))` }}>
              {albums.map((a, i) => (
                <button
                  key={a.key}
                  style={{ paddingBottom: '100%', '--ti': i }}
                  type="button"
                  onClick={() => setNav({ tab: 'albums', album: a.key })}
                  className="pmgr-tile-in relative block overflow-hidden rounded-xl border border-panel-border bg-panel-2 text-left transition-colors hover:border-accent/50"
                >
                  {a.id && titles[a.id]?.has_icon ? (
                    <FadeImage src={api.titles.iconUrl(a.id)} alt="" loading="lazy" className="absolute left-0 top-0 h-full w-full object-cover" />
                  ) : a.cover.type === 'photo' ? (
                    <FadeImage src={api.media.thumbUrl(a.cover.path)} alt="" loading="lazy" decoding="async" className="absolute left-0 top-0 h-full w-full object-cover" />
                  ) : (
                    <span className="absolute left-0 top-0 grid h-full w-full place-items-center text-text-dim"><ImageIcon size={32} /></span>
                  )}
                  <span className="absolute bottom-0 left-0 right-0 bg-gradient-to-t from-black/85 to-transparent px-3 pb-2.5 pt-10 text-white">
                    <span className="block truncate text-sm font-bold">{gameNames[a.key]}</span>
                    <span className="pmgr-num mt-0.5 block truncate text-xs opacity-85">{a.photos + a.videos} · {formatFileSize(a.size)}</span>
                  </span>
                </button>
              ))}
            </div>
          )}

          {tab === 'albums' && album !== 'all' && (
            <div className="mb-4 flex items-center">
              <Button size="sm" tone="neutral" onClick={() => setNav({ tab: 'albums', album: 'all' })}>{t('media.backToAlbums')}</Button>
              <h3 className="ml-3 truncate text-sm font-bold text-text">{gameNames[album]}</h3>
            </div>
          )}

          <div className={`mb-4 flex flex-wrap items-center ${pickingAlbum ? 'hidden' : ''}`}>
            <SegmentedControl className="mr-3"
              options={[
                { id: 'all', label: t('media.all'), count: counts.all },
                { id: 'photo', label: t('media.photos'), count: counts.photo },
                { id: 'video', label: t('media.videos'), count: counts.video },
              ]}
              value={kind}
              onChange={setKind}
            />
            <Button size="sm" tone="neutral" icon={<CheckIcon size={14} />} onClick={() => setSelected(allSelected ? new Set() : new Set(shown.map((i) => i.path)))}>
              {allSelected ? t('common.clearSelection') : t('common.selectAll')}
            </Button>
          </div>

          {selected.size > 0 && (
            <div className="pmgr-notice-in sticky top-2 z-20 mb-4 flex flex-wrap items-center rounded-xl border border-accent/40 bg-panel px-3 py-2.5 shadow-lg shadow-black/30">
              <span className="mr-auto text-sm font-bold text-text">{t('common.selectedCount', { count: selected.size })}</span>
              <Button className="mr-2" size="sm" tone="primary" icon={<DownloadIcon size={14} />} disabled={zipping} onClick={() => downloadZip(shown.filter((i) => selected.has(i.path)))}>{t('media.downloadZip')}</Button>
              <Button className="mr-2" size="sm" tone="neutral" icon={<DownloadIcon size={14} />} onClick={downloadSelected}>{t('media.downloadSelected')}</Button>
              <Button className="mr-2" size="sm" tone="danger" icon={<DeleteIcon size={14} />} onClick={() => setDeleting(shown.filter((i) => selected.has(i.path)))}>{t('media.deleteSelected')}</Button>
              <Button size="sm" tone="ghost" onClick={() => setSelected(new Set())}>{t('common.clearSelection')}</Button>
            </div>
          )}

          {pickingAlbum ? null : shown.length === 0 ? (
            <EmptyState size="sm" icon={<ImageIcon size={20} />} title={t('media.emptyTitle')} description={t('media.emptyFilter')} />
          ) : (
            days.map((group) => (
              <section key={group.day} className="mb-5">
                <h3 className="mb-2 text-sm font-semibold text-text">
                  {formatDate(`${group.day}T00:00:00`, locale, { dateStyle: 'long' })}
                  <span className="pmgr-num ml-2 text-xs font-normal text-text-dim">{group.items.length}</span>
                </h3>
                <div className="grid gap-3" style={{ gridTemplateColumns: `repeat(auto-fill, minmax(${isPS5() ? 320 : 280}px, 1fr))` }}>
                  {group.items.map((item) => (
                    <Tile
                      key={item.path}
                      item={item}
                      gameName={gameNames[gameKey(item)]}
                      showGame={album === 'all'}
                      selected={selected.has(item.path)}
                      onOpen={open}
                      onToggle={toggle}
                      label={t('files.selectRow', { name: item.name })}
                      locale={locale}
                    />
                  ))}
                </div>
              </section>
            ))
          )}

          {!pickingAlbum && shown.length > limit && (
            <div className="flex justify-center">
              <Button tone="neutral" onClick={() => setLimit((n) => n + PAGE)}>{t('common.showMore', { count: Math.min(PAGE, shown.length - limit) })}</Button>
            </div>
          )}
        </>
      )}

      {viewing !== null && viewing >= 0 && (
        <Viewer items={shown} index={viewing} gameNames={Object.fromEntries(shown.map((i) => [i.game_id, gameNames[gameKey(i)]]))} onIndex={setViewing} onClose={() => setViewing(null)} onDelete={(item) => setDeleting([item])} />
      )}

      {deleting && (
        <ConfirmDialog
          title={t('media.deleteTitle')}
          description={t('media.deleteDesc', { count: deleting.length })}
          tone="danger"
          confirmLabel={t('common.delete')}
          cancelLabel={t('common.cancel')}
          onConfirm={confirmDelete}
          onClose={() => setDeleting(null)}
        />
      )}
    </Panel>
  )
}
