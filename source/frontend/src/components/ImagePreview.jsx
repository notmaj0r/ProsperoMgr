import { useState } from 'react'
import { createPortal } from 'react-dom'
import Button from './Button.jsx'
import { CloseIcon, AlertTriangleIcon } from './icons.jsx'
import { api } from '../lib/api.js'
import { useDismiss } from '../lib/useDismiss.js'
import { useDialogAccessibility } from '../lib/useDialogAccessibility.js'
import { useI18n } from '../lib/i18n.jsx'

// Same portal/dialog shell as FileEditor, just an <img> instead of a textarea —
// no read/write API needed, the existing download endpoint already streams bytes.
export default function ImagePreview({ path, onClose }) {
  const { t } = useI18n()
  const [error, setError] = useState(false)
  const { closing, close } = useDismiss(onClose)
  const containerRef = useDialogAccessibility(close)

  const filename = path.split('/').pop()

  return createPortal(
    <div className={`pmgr-modal-overlay !items-stretch !p-0 sm:!items-center sm:!p-4 ${closing ? 'is-closing' : ''}`} onClick={close}>
      <div
        ref={containerRef}
        role="dialog"
        aria-modal="true"
        aria-label={filename}
        className={`pmgr-panel pmgr-modal-pop pmgr-h-dvh-100 pmgr-h-dvh-85-sm flex w-full flex-col overflow-hidden border-panel-border sm:max-w-4xl sm:rounded-2xl sm:border ${closing ? 'is-closing' : ''}`}
        style={{ paddingTop: 'env(safe-area-inset-top)', paddingBottom: 'env(safe-area-inset-bottom)' }}
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex shrink-0 items-center gap-3 border-b border-panel-border px-4 py-3 sm:px-5">
          <h2 className="min-w-0 flex-1 truncate text-sm font-bold text-text">{filename}</h2>
          <Button size="icon" tone="ghost" title={t('common.close')} aria-label={t('common.close')} onClick={close} icon={<CloseIcon size={16} />} />
        </div>

        <div className="flex min-h-0 flex-1 items-center justify-center overflow-auto p-3 sm:p-4">
          {error ? (
            <div className="flex flex-col items-center gap-3 text-center">
              <span className="grid h-11 w-11 place-items-center rounded-xl bg-err/15 text-err"><AlertTriangleIcon size={20} /></span>
              <div className="text-sm text-text-dim">{t('imagePreview.loadFailed')}</div>
            </div>
          ) : (
            <img
              src={api.ftp.downloadUrl(path)}
              alt={filename}
              onError={() => setError(true)}
              className="max-h-full max-w-full rounded-lg object-contain"
            />
          )}
        </div>
      </div>
    </div>,
    document.body,
  )
}
