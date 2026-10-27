import { useEffect, useMemo, useState } from 'react'
import { createPortal } from 'react-dom'
import Button from './Button.jsx'
import PasswordInput from './PasswordInput.jsx'
import { ChevronRightIcon, CloseIcon, FileIcon, FolderIcon, LockIcon, ZipIcon } from './icons.jsx'
import { api } from '../lib/api.js'
import { useDismiss } from '../lib/useDismiss.js'
import { useDialogAccessibility } from '../lib/useDialogAccessibility.js'
import { useI18n } from '../lib/i18n.jsx'
import { formatFileSize } from '../lib/format.js'

// Folder -> {folders, files} map from the flat entry list. Ancestor folders are inferred
// from file paths too, not just is_dir entries — not every archive has explicit dir entries.
function buildTree(entries) {
  const dirs = new Map()
  const ensure = (p) => {
    if (!dirs.has(p)) dirs.set(p, { folders: new Set(), files: [] })
    return dirs.get(p)
  }
  ensure('')
  for (const entry of entries) {
    const parts = entry.name.split('/').filter(Boolean)
    const leaf = entry.is_dir ? null : parts.pop()
    let cur = ''
    for (const part of parts) {
      const parent = cur
      cur = cur ? `${cur}/${part}` : part
      ensure(parent).folders.add(part)
      ensure(cur)
    }
    if (leaf) ensure(cur).files.push({ name: leaf, size: entry.uncompressed_size })
  }
  return dirs
}

export default function ArchiveInspectorModal({ path, name, onClose, onExtractAll }) {
  const { t, locale } = useI18n()
  const { closing, close } = useDismiss(onClose)
  const dialogRef = useDialogAccessibility(close)
  const [info, setInfo] = useState(null)
  const [error, setError] = useState('')
  const [browsePath, setBrowsePath] = useState('')
  const [passwordNeeded, setPasswordNeeded] = useState(false)
  const [password, setPassword] = useState('')
  const [unlocking, setUnlocking] = useState(false)
  const [passwordError, setPasswordError] = useState('')

  useEffect(() => {
    let cancelled = false
    api.ftp.archiveInfo(path)
      .then((r) => !cancelled && setInfo(r))
      .catch((e) => {
        if (cancelled) return
        if (e.code === 'password_required') setPasswordNeeded(true)
        else setError(e.message)
      })
    return () => { cancelled = true }
  }, [path])

  const unlock = async () => {
    if (!password || unlocking) return
    setUnlocking(true)
    setPasswordError('')
    try {
      const r = await api.ftp.archiveInfo(path, password)
      setInfo(r)
      setPasswordNeeded(false)
    } catch (e) {
      setPasswordError(e.code === 'password_required' ? t('archive.wrongPassword') : e.message)
    } finally {
      setUnlocking(false)
    }
  }

  const tree = useMemo(() => (info ? buildTree(info.entries) : null), [info])
  const node = tree?.get(browsePath) || { folders: new Set(), files: [] }
  const folderRows = useMemo(() => [...node.folders].sort((a, b) => a.localeCompare(b)), [node])
  const fileRows = useMemo(() => [...node.files].sort((a, b) => a.name.localeCompare(b.name)), [node])
  const crumbs = browsePath ? browsePath.split('/') : []

  const ratio = info && info.uncompressed_size > 0
    ? Math.round((1 - info.compressed_size / info.uncompressed_size) * 100)
    : 0

  return createPortal(
    <div className={`pmgr-modal-overlay pmgr-path-picker-overlay ${closing ? 'is-closing' : ''}`} onClick={close}>
      <div
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-label={name}
        tabIndex={-1}
        className={`pmgr-panel pmgr-modal-pop pmgr-path-picker flex w-full max-w-xl flex-col overflow-hidden rounded-2xl border border-panel-border ${closing ? 'is-closing' : ''}`}
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex shrink-0 items-center gap-3 border-b border-panel-border px-5 py-4">
          <span className="grid h-10 w-10 shrink-0 place-items-center rounded-xl bg-accent/10 text-accent"><ZipIcon size={19} /></span>
          <div className="min-w-0 flex-1">
            <h3 className="truncate font-bold text-text" dir="ltr">{name}</h3>
            {info && (
              <p className="truncate text-xs text-text-dim">
                {t('common.itemsCount', { count: info.items })} · {formatFileSize(info.compressed_size, locale)}
                {ratio > 0 ? ` · ${t('archive.compressionRatio', { percent: ratio })}` : ''}
              </p>
            )}
          </div>
          <Button size="icon" tone="ghost" title={t('common.close')} aria-label={t('common.close')} onClick={close} icon={<CloseIcon size={16} />} />
        </div>

        <div className="flex min-h-0 flex-1 flex-col overflow-hidden p-4">
          <div className="mb-3 flex items-center gap-1 overflow-x-auto rounded-xl border border-panel-border bg-bg/55 px-2 py-1.5" dir="ltr">
            <button type="button" onClick={() => setBrowsePath('')} className={`rounded-md px-2 py-1 text-xs font-semibold ${browsePath === '' ? 'bg-accent/10 text-accent' : 'text-text-dim hover:bg-panel-border hover:text-text'}`}>
              {name}
            </button>
            {crumbs.map((part, index) => {
              const full = crumbs.slice(0, index + 1).join('/')
              return (
                <div key={full} className="flex shrink-0 items-center gap-1">
                  <ChevronRightIcon size={12} />
                  <button type="button" onClick={() => setBrowsePath(full)} className={`rounded-md px-2 py-1 text-xs font-semibold ${full === browsePath ? 'bg-accent/10 text-accent' : 'text-text-dim hover:bg-panel-border hover:text-text'}`}>
                    {part}
                  </button>
                </div>
              )
            })}
          </div>

          {error && <div className="mb-2 rounded-lg border border-err/25 bg-err/10 px-3 py-2 text-xs text-err">{error}</div>}
          <div className="min-h-0 flex-1 overflow-y-auto overscroll-contain rounded-xl border border-panel-border bg-bg/35 p-1.5">
            {passwordNeeded ? (
              <div className="flex h-40 flex-col items-center justify-center gap-3 px-6 text-center">
                <span className="grid h-11 w-11 place-items-center rounded-xl bg-warn/15 text-warn"><LockIcon size={20} /></span>
                <div className="text-sm font-semibold text-text">{t('archive.passwordRequired')}</div>
                <div className="flex w-full max-w-xs gap-1.5">
                  <PasswordInput
                    className="min-w-0 flex-1"
                    value={password}
                    onChange={(e) => setPassword(e.target.value)}
                    onKeyDown={(e) => e.key === 'Enter' && (e.preventDefault(), unlock())}
                    placeholder={t('archive.passwordPlaceholder')}
                    autoFocus
                  />
                  <Button tone="neutral" disabled={!password || unlocking} onClick={unlock}>
                    {unlocking ? t('common.working') : t('archive.unlock')}
                  </Button>
                </div>
                {passwordError && <div className="text-xs text-err">{passwordError}</div>}
              </div>
            ) : !info && !error ? (
              <div className="grid h-40 place-items-center text-sm text-text-dim">{t('common.loading')}</div>
            ) : folderRows.length === 0 && fileRows.length === 0 ? (
              <div className="grid h-40 place-items-center text-center text-sm text-text-dim">
                <span><FolderIcon size={28} /><span className="mt-2 block">{t('files.emptyFolderTitle')}</span></span>
              </div>
            ) : (
              <>
                {folderRows.map((folder, index) => (
                  <button
                    type="button"
                    key={`d-${folder}`}
                    onClick={() => setBrowsePath(browsePath ? `${browsePath}/${folder}` : folder)}
                    className="pmgr-row-in flex w-full items-center gap-3 rounded-lg px-3 py-2.5 text-left text-sm text-text transition-colors hover:bg-panel-border"
                    style={{ '--ti': index }}
                  >
                    <span className="grid h-9 w-9 shrink-0 place-items-center rounded-lg bg-accent/10 text-accent"><FolderIcon size={18} /></span>
                    <span className="min-w-0 flex-1 truncate font-medium">{folder}</span>
                    <ChevronRightIcon size={15} />
                  </button>
                ))}
                {fileRows.map((file, index) => (
                  <div key={`f-${file.name}`} className="pmgr-row-in flex w-full items-center gap-3 rounded-lg px-3 py-2.5 text-sm text-text" style={{ '--ti': folderRows.length + index }}>
                    <span className="grid h-9 w-9 shrink-0 place-items-center rounded-lg bg-panel-border text-text-dim"><FileIcon size={17} /></span>
                    <span className="min-w-0 flex-1 truncate">{file.name}</span>
                    <span dir="ltr" className="pmgr-mono shrink-0 text-xs text-text-dim">{formatFileSize(file.size, locale)}</span>
                  </div>
                ))}
              </>
            )}
          </div>
          {info?.entries_truncated && (
            <div className="mt-2 text-xs text-text-dim">{t('archive.entriesTruncated')}</div>
          )}
        </div>

        <div className="flex shrink-0 justify-end gap-2 border-t border-panel-border bg-bg/35 px-5 py-4">
          <Button tone="neutral" onClick={close}>{t('common.close')}</Button>
          <Button tone="primary" sheen onClick={onExtractAll}>{t('archive.extractAll')}</Button>
        </div>
      </div>
    </div>,
    document.body,
  )
}
