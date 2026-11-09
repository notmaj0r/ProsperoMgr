import { useEffect, useMemo, useState } from 'react'
import { createPortal } from 'react-dom'
import Button from './Button.jsx'
import { InputDialog } from './Dialog.jsx'
import {
  CheckCircleIcon, ChevronRightIcon, CloseIcon, FileIcon, FolderIcon, GoUpIcon, NewFolderIcon,
} from './icons.jsx'
import { api } from '../lib/api.js'
import { useDismiss } from '../lib/useDismiss.js'
import { useDialogAccessibility } from '../lib/useDialogAccessibility.js'
import { volumeLabel } from '../lib/format.js'
import { useI18n } from '../lib/i18n.jsx'
import { SkeletonRows } from './Skeleton.jsx'

function joinPath(dir, name) {
  return dir.endsWith('/') ? dir + name : `${dir}/${name}`
}

function crumbs(path) {
  const parts = path.split('/').filter(Boolean)
  return [
    { label: 'PS5', path: '/' },
    ...parts.map((part, index) => ({ label: part, path: `/${parts.slice(0, index + 1).join('/')}` })),
  ]
}

export default function PathPickerModal({
  title,
  description,
  initialPath = '/data',
  volumes = [],
  mode = 'folder',
  extensions = [],
  multiple = false,
  actionLabel,
  onClose,
  onSelect,
  validateSelection,
}) {
  const { t } = useI18n()
  const [browsePath, setBrowsePath] = useState(initialPath || '/data')
  const [entries, setEntries] = useState([])
  const [selectedFile, setSelectedFile] = useState('')
  const [selectedFiles, setSelectedFiles] = useState(() => new Set())
  const [loading, setLoading] = useState(false)
  const [submitting, setSubmitting] = useState(false)
  const [error, setError] = useState('')
  const [newFolderOpen, setNewFolderOpen] = useState(false)
  const { closing, close } = useDismiss(onClose)
  const dialogRef = useDialogAccessibility(close)

  const allowed = useMemo(() => new Set(extensions.map((ext) => ext.toLowerCase())), [extensions])

  const browse = (nextPath) => {
    setLoading(true)
    setError('')
    setSelectedFile('')
    setSelectedFiles(new Set())
    api.ftp.list(nextPath)
      .then((result) => {
        const visible = (result.entries || []).filter((item) => {
          if (item.is_dir || mode === 'folder') return item.is_dir
          if (allowed.size === 0) return true
          const dot = item.name.lastIndexOf('.')
          return dot >= 0 && allowed.has(item.name.slice(dot).toLowerCase())
        })
        visible.sort((a, b) => (a.is_dir === b.is_dir ? a.name.localeCompare(b.name) : a.is_dir ? -1 : 1))
        setEntries(visible)
        setBrowsePath(nextPath)
      })
      .catch((e) => setError(e.message))
      .finally(() => setLoading(false))
  }

  useEffect(() => browse(initialPath || '/data'), [initialPath]) // eslint-disable-line react-hooks/exhaustive-deps

  const multiFile = multiple && mode === 'file'
  const selection = mode === 'file' ? selectedFile : browsePath
  const validationError = !multiFile && selection && validateSelection ? validateSelection(selection) : ''

  const toggleFile = (fullPath) => {
    setSelectedFiles((prev) => {
      const next = new Set(prev)
      if (next.has(fullPath)) next.delete(fullPath)
      else next.add(fullPath)
      return next
    })
  }

  const createFolder = (name) =>
    api.ftp.mkdir(joinPath(browsePath, name)).then(() => browse(browsePath))

  const validateFolderName = (name) =>
    name.includes('/') || name === '.' || name === '..' ? t('files.invalidNameError', { name }) : ''

  const choose = () => {
    if (multiFile) {
      if (selectedFiles.size === 0 || submitting) return
      setSubmitting(true)
      setError('')
      Promise.resolve(onSelect(Array.from(selectedFiles)))
        .catch((e) => setError(e.message))
        .finally(() => setSubmitting(false))
      return
    }
    if (!selection || validationError || submitting) return
    setSubmitting(true)
    setError('')
    Promise.resolve(onSelect(selection))
      .catch((e) => setError(e.message))
      .finally(() => setSubmitting(false))
  }

  return <>{createPortal(
    <div className={`pmgr-modal-overlay pmgr-path-picker-overlay ${closing ? 'is-closing' : ''}`} onClick={close}>
      <div ref={dialogRef} role="dialog" aria-modal="true" aria-labelledby="pmgr-path-picker-title" tabIndex={-1} className={`pmgr-panel pmgr-modal-pop pmgr-path-picker flex w-full max-w-xl flex-col overflow-hidden rounded-2xl border border-panel-border ${closing ? 'is-closing' : ''}`} onClick={(e) => e.stopPropagation()}>
        <div className="flex shrink-0 items-center gap-3 border-b border-panel-border px-5 py-4">
          <span className="grid h-10 w-10 shrink-0 place-items-center rounded-xl bg-accent/10 text-accent"><FolderIcon size={20} /></span>
          <div className="min-w-0 flex-1">
            <h3 id="pmgr-path-picker-title" className="truncate font-bold text-text">{title}</h3>
            {description && <p className="truncate text-xs text-text-dim">{description}</p>}
          </div>
          <Button size="icon" tone="ghost" title={t('common.close')} aria-label={t('common.close')} onClick={close} icon={<CloseIcon size={16} />} />
        </div>

        {volumes.length > 0 && (
          <div className="pmgr-path-picker-volumes flex shrink-0 gap-2 overflow-x-auto border-b border-panel-border px-4 py-3">
            {volumes.map((volume) => (
              <button type="button" key={volume.path} onClick={() => browse(volume.path)} className={`min-h-[52px] shrink-0 rounded-lg border px-3 py-2 text-left text-xs transition-[color,background-color,border-color] ${browsePath === volume.path ? 'border-accent/50 bg-accent/10 text-accent' : 'border-panel-border bg-bg/45 text-text-dim hover:border-accent/30 hover:text-text'}`}>
                <span className="flex items-center gap-1.5 font-semibold"><FolderIcon size={14} />{volumeLabel(volume, t)}</span>
                <span className="mt-0.5 block font-mono text-xs opacity-75" dir="ltr">{volume.path}</span>
              </button>
            ))}
          </div>
        )}

        <div className="flex min-h-0 flex-1 flex-col overflow-hidden p-4">
          <div className="mb-3 flex items-center gap-1 overflow-x-auto rounded-xl border border-panel-border bg-bg/55 px-2 py-1.5" dir="ltr">
            {crumbs(browsePath).map((crumb, index, all) => (
              <div key={crumb.path} className="flex shrink-0 items-center gap-1">
                <button type="button" onClick={() => browse(crumb.path)} className={`rounded-md px-2 py-1 text-xs font-semibold ${index === all.length - 1 ? 'bg-accent/10 text-accent' : 'text-text-dim hover:bg-panel-border hover:text-text'}`}>{crumb.label}</button>
                {index < all.length - 1 && <ChevronRightIcon size={12} />}
              </div>
            ))}
          </div>

          <div className="mb-2 flex items-center justify-between gap-2">
            <Button size="sm" tone="neutral" icon={<GoUpIcon size={14} />} disabled={browsePath === '/'} onClick={() => browse(browsePath.replace(/\/[^/]+\/?$/, '') || '/')}>{t('files.up')}</Button>
            <Button size="sm" tone="neutral" icon={<NewFolderIcon size={14} />} onClick={() => setNewFolderOpen(true)}>{t('files.newFolder')}</Button>
          </div>

          {error && <div className="mb-2 rounded-lg border border-err/25 bg-err/10 px-3 py-2 text-xs text-err">{error}</div>}
          <div className="min-h-0 flex-1 overflow-y-auto overscroll-contain rounded-xl border border-panel-border bg-bg/35 p-1.5">
            {loading ? (
              <SkeletonRows count={5} />
            ) : entries.length === 0 ? (
              <div className="grid h-40 place-items-center text-center text-sm text-text-dim"><span><FolderIcon size={28} /><span className="mt-2 block">{t('files.emptyFolderTitle')}</span></span></div>
            ) : entries.map((entry, index) => {
              const fullPath = joinPath(browsePath, entry.name)
              const selected = multiFile ? selectedFiles.has(fullPath) : selectedFile === fullPath
              return (
                <button
                  type="button"
                  key={entry.name}
                  onClick={() => {
                    if (entry.is_dir) browse(fullPath)
                    else if (multiFile) toggleFile(fullPath)
                    else setSelectedFile(fullPath)
                  }}
                  onDoubleClick={() => {
                    if (entry.is_dir || multiFile) return
                    setSelectedFile(fullPath)
                    Promise.resolve(onSelect(fullPath)).catch((e) => setError(e.message))
                  }}
                  className={`pmgr-row-in flex w-full items-center gap-3 rounded-lg px-3 py-2.5 text-left text-sm transition-colors ${selected ? 'bg-accent/15 text-accent' : 'text-text hover:bg-panel-border'}`}
                  style={{ '--ti': index }}
                >
                  {multiFile && !entry.is_dir && (
                    selected
                      ? <CheckCircleIcon size={18} className="shrink-0 text-accent" />
                      : <span className="h-[18px] w-[18px] shrink-0 rounded-full border border-panel-border" />
                  )}
                  <span className="grid h-9 w-9 shrink-0 place-items-center rounded-lg bg-accent/10 text-accent">{entry.is_dir ? <FolderIcon size={18} /> : <FileIcon size={17} />}</span>
                  <span className="min-w-0 flex-1 truncate font-medium">{entry.name}</span>
                  {entry.is_dir && <ChevronRightIcon size={15} />}
                </button>
              )
            })}
          </div>
        </div>

        <div className="shrink-0 border-t border-panel-border bg-bg/35 px-5 py-4">
          <div className="mb-3 min-w-0 text-xs text-text-dim">
            {multiFile ? (
              t('common.selectedCount', { count: selectedFiles.size })
            ) : (
              <>
                {mode === 'file' ? t('fileEditor.path') : t('common.name')}: <span className="break-all font-mono text-text" dir="ltr">{selection || '—'}</span>
                {validationError && <span className="mt-1 block text-warn">{validationError}</span>}
              </>
            )}
          </div>
          <div className="flex justify-end gap-2">
            <Button tone="neutral" onClick={close}>{t('common.cancel')}</Button>
            <Button tone="primary" onClick={choose} disabled={multiFile ? (selectedFiles.size === 0 || submitting) : (!selection || Boolean(validationError) || submitting)}>{submitting ? t('common.working') : (actionLabel || t('common.confirm'))}</Button>
          </div>
        </div>
      </div>
    </div>,
    document.body,
  )}
    {newFolderOpen && (
      <InputDialog
        title={t('files.newFolderTitle')}
        label={t('files.newFolderLabel')}
        confirmLabel={t('common.create')}
        validate={validateFolderName}
        onClose={() => setNewFolderOpen(false)}
        onConfirm={createFolder}
      />
    )}
  </>
}
