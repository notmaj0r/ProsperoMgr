import { useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import Button from './Button.jsx'
import { ConfirmDialog } from './Dialog.jsx'
import { CloseIcon, SaveIcon, AlertTriangleIcon } from './icons.jsx'
import { api } from '../lib/api.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useDismiss } from '../lib/useDismiss.js'
import { useDialogAccessibility } from '../lib/useDialogAccessibility.js'
import { useI18n } from '../lib/i18n.jsx'

// Portaled to document.body, not nested in Files' own view hierarchy, so it can't inherit a
// clipping/stacking context from an ancestor. Same foundation as Dialog.jsx, laid out full-height.
export default function FileEditor({ path, onClose, onSaved }) {
  const { t } = useI18n()
  const [text, setText] = useState('')
  const [original, setOriginal] = useState('')
  const [metadata, setMetadata] = useState(null)
  const [loading, setLoading] = useState(true)
  const [saving, setSaving] = useState(false)
  const [error, setError] = useState('')
  const [confirmDiscard, setConfirmDiscard] = useState(false)
  const showToast = useToast()
  const textareaRef = useRef(null)
  const { closing, close } = useDismiss(onClose)
  const dirty = !loading && text !== original

  const load = () => {
    setLoading(true)
    setError('')
    api.ftp
      .readText(path)
      .then((body) => {
        setText(body.text)
        setOriginal(body.text)
        setMetadata({ version: body.version, newline: body.newline, bom: body.bom })
      })
      .catch((e) => setError(e.message))
      .finally(() => setLoading(false))
  }

  useEffect(load, [path]) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => {
    if (!loading && !error) textareaRef.current?.focus()
  }, [loading, error])

  const requestClose = () => {
    if (dirty) setConfirmDiscard(true)
    else close()
  }
  const containerRef = useDialogAccessibility(requestClose)

  const save = () => {
    if (saving) return
    setSaving(true)
    api.ftp
      .writeText(path, text, metadata)
      .then((result) => {
        showToast(`${t('common.done')} — ${path.split('/').pop()}`)
        setOriginal(text)
        if (result?.version) setMetadata((current) => ({ ...current, version: result.version }))
        onSaved?.()
        close()
      })
      .catch((e) => showToast(e.message, 'err'))
      .finally(() => setSaving(false))
  }

  useEffect(() => {
    const onKeyDown = (e) => {
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 's') {
        e.preventDefault()
        save()
      }
    }
    document.addEventListener('keydown', onKeyDown)
    return () => document.removeEventListener('keydown', onKeyDown)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [text, dirty, saving])

  const filename = path.split('/').pop()
  const dirPath = path.slice(0, path.length - filename.length - 1) || '/'

  return createPortal(
    <>
      <div className={`pmgr-modal-overlay !items-stretch !p-0 sm:!items-center sm:!p-4 ${closing ? 'is-closing' : ''}`} onClick={requestClose}>
        <div
          ref={containerRef}
          role="dialog"
          aria-modal="true"
          aria-label={t('fileEditor.title')}
          className={`pmgr-panel pmgr-modal-pop pmgr-h-dvh-100 pmgr-h-dvh-85-sm flex w-full flex-col overflow-hidden border-panel-border sm:max-w-4xl sm:rounded-2xl sm:border ${closing ? 'is-closing' : ''}`}
          style={{ paddingTop: 'env(safe-area-inset-top)', paddingBottom: 'env(safe-area-inset-bottom)' }}
          onClick={(e) => e.stopPropagation()}
        >
          <div className="flex shrink-0 items-center gap-3 border-b border-panel-border px-4 py-3 sm:px-5">
            <div className="min-w-0 flex-1">
              <div className="flex items-center gap-2">
                <h2 className="truncate text-sm font-bold text-text">{filename}</h2>
                {dirty && (
                  <span className="inline-flex shrink-0 items-center gap-1 rounded-full bg-warn/15 px-2 py-0.5 text-[11px] font-bold text-warn">
                    <span className="h-1.5 w-1.5 rounded-full bg-warn" /> {t('common.unsaved')}
                  </span>
                )}
              </div>
              <p className="mt-0.5 truncate text-xs text-text-dim" dir="ltr" title={dirPath}>{dirPath}</p>
            </div>
            <Button size="icon" tone="ghost" title={t('common.close')} aria-label={t('common.close')} onClick={requestClose} icon={<CloseIcon size={16} />} />
          </div>

          <div className="min-h-0 flex-1 p-3 sm:p-4">
            {loading ? (
              <div className="flex h-full flex-col gap-2 rounded-lg border border-panel-border bg-bg p-3">
                {[0, 1, 2, 3, 4, 5, 6].map((i) => (
                  <div key={i} className="pmgr-skeleton h-3.5 rounded" style={{ width: `${85 - i * 8}%` }} />
                ))}
              </div>
            ) : error ? (
              <div className="flex h-full flex-col items-center justify-center gap-3 rounded-lg border border-err/25 bg-err/5 p-6 text-center">
                <span className="grid h-11 w-11 place-items-center rounded-xl bg-err/15 text-err"><AlertTriangleIcon size={20} /></span>
                <div>
                  <div className="font-bold text-text">{t('fileEditor.loadFailedTitle')}</div>
                  <div className="mt-1 text-sm text-text-dim">{error}</div>
                </div>
                <div className="flex gap-2">
                  <Button tone="neutral" onClick={requestClose}>{t('common.close')}</Button>
                  <Button tone="primary" onClick={load}>{t('common.retry')}</Button>
                </div>
              </div>
            ) : (
              <textarea
                ref={textareaRef}
                value={text}
                onChange={(e) => setText(e.target.value)}
                spellCheck={false}
                dir="ltr"
                className="h-full w-full resize-none rounded-lg border border-panel-border bg-bg p-3.5 font-mono text-sm leading-relaxed text-text outline-none focus:border-accent"
              />
            )}
          </div>

          {!loading && !error && (
            <div className="flex shrink-0 items-center justify-between gap-3 border-t border-panel-border px-4 py-3 sm:px-5">
              <span className="hidden text-xs text-text-dim sm:inline">{t('fileEditor.ctrlSHint')}</span>
              <div className="ml-auto flex gap-2">
                <Button tone="neutral" onClick={requestClose}>{t('fileEditor.cancel')}</Button>
                <Button tone="primary" sheen icon={<SaveIcon size={15} />} onClick={save} disabled={saving}>
                  {saving ? t('fileEditor.saving') : t('fileEditor.save')}
                </Button>
              </div>
            </div>
          )}
        </div>
      </div>

      {confirmDiscard && (
        <ConfirmDialog
          title={t('dialog.discardChangesTitle')}
          description={t('dialog.discardChangesDesc')}
          tone="danger"
          confirmLabel={t('dialog.discard')}
          onClose={() => setConfirmDiscard(false)}
          onConfirm={() => close()}
        />
      )}
    </>,
    document.body,
  )
}
