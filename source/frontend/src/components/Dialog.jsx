import { useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import Button from './Button.jsx'
import { AlertTriangleIcon, InfoIcon } from './icons.jsx'
import { useI18n } from '../lib/i18n.jsx'
import { useDismiss } from '../lib/useDismiss.js'
import { useDialogAccessibility } from '../lib/useDialogAccessibility.js'

let dialogIdCounter = 0

const TONE_ICON_BG = {
  neutral: 'bg-accent/10 text-accent',
  danger: 'bg-err/10 text-err',
  warn: 'bg-warn/10 text-warn',
}

// Shared modal foundation — portal, backdrop, focus trap, Escape, role="dialog"/aria-modal.
// `children`/`renderFooter` may be `(close) => node`, so a field's own Enter-to-submit reaches the same close.
export function Dialog({
  onClose, title, description, children, renderFooter, icon, tone = 'neutral',
  maxWidth = 'max-w-md', initialFocusRef,
}) {
  const { closing, close } = useDismiss(onClose)
  const containerRef = useDialogAccessibility(close, initialFocusRef)
  const titleId = useRef(`pmgr-dialog-title-${++dialogIdCounter}`).current

  return createPortal(
    <div className={`pmgr-modal-overlay ${closing ? 'is-closing' : ''}`} onClick={close}>
      <div
        ref={containerRef}
        role="dialog"
        aria-modal="true"
        aria-labelledby={titleId}
        tabIndex={-1}
        className={`pmgr-panel pmgr-modal-pop pmgr-max-h-dvh-85 flex w-full ${maxWidth} flex-col overflow-hidden rounded-2xl border border-panel-border ${closing ? 'is-closing' : ''}`}
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex shrink-0 items-start gap-3 px-5 pb-4 pt-5">
          {icon && <span className={`grid h-10 w-10 shrink-0 place-items-center rounded-xl ${TONE_ICON_BG[tone]}`}>{icon}</span>}
          <div className="min-w-0 flex-1">
            <h2 id={titleId} className="text-base font-bold text-text">{title}</h2>
            {description && <p className="mt-1 text-sm leading-relaxed text-text-dim">{description}</p>}
          </div>
        </div>
        {children && <div className="min-h-0 overflow-y-auto px-5 pb-4">{typeof children === 'function' ? children(close) : children}</div>}
        {renderFooter && (
          <div className="flex shrink-0 flex-wrap justify-end gap-2 border-t border-panel-border bg-bg/35 px-5 py-4">{renderFooter(close)}</div>
        )}
      </div>
    </div>,
    document.body,
  )
}

// "Are you sure?" dialog. `detail` holds a technical line such as "PID: 1234" that shouldn't lead the text.
export function ConfirmDialog({
  title, description, detail, confirmLabel, cancelLabel, tone = 'neutral', onConfirm, onClose,
  children,
}) {
  const { t } = useI18n()
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')

  const handleConfirm = async (close) => {
    setError('')
    setBusy(true)
    try {
      await onConfirm()
      close()
    } catch (e) {
      setError(e?.message || 'Something went wrong.')
      setBusy(false)
    }
  }

  return (
    <Dialog
      title={title}
      description={description}
      onClose={onClose}
      tone={tone}
      icon={tone === 'danger' || tone === 'warn' ? <AlertTriangleIcon size={19} /> : <InfoIcon size={19} />}
      renderFooter={(close) => (
        <>
          <Button tone="neutral" onClick={close} disabled={busy}>{cancelLabel ?? t('common.cancel')}</Button>
          <Button
            tone={tone === 'danger' ? 'danger' : tone === 'warn' ? 'warn' : 'primary'}
            sheen={tone !== 'danger' && tone !== 'warn'}
            onClick={() => handleConfirm(close)}
            disabled={busy}
          >
            {busy ? t('common.working') : (confirmLabel ?? t('common.confirm'))}
          </Button>
        </>
      )}
    >
      {(children || detail || error) && (
        <div className="space-y-2">
          {children}
          {detail && <div className="pmgr-mono rounded-lg border border-panel-border bg-bg/45 px-3 py-2 text-xs text-text-dim">{detail}</div>}
          {error && <div className="rounded-lg border border-err/25 bg-err/10 px-3 py-2 text-xs text-err">{error}</div>}
        </div>
      )}
    </Dialog>
  )
}

// New file/folder, rename — replaces window.prompt. Autofocuses, submits on Enter, validates
// inline, and surfaces backend errors inline instead of a second alert.
export function InputDialog({
  title, description, label, defaultValue = '', placeholder, confirmLabel, cancelLabel,
  type = 'text', validate, onConfirm, onClose,
}) {
  const { t } = useI18n()
  const [value, setValue] = useState(defaultValue)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const inputRef = useRef(null)

  const localError = validate ? validate(value) : ''

  const submit = async (close) => {
    if (localError || busy || !value.trim()) return
    setError('')
    setBusy(true)
    try {
      await onConfirm(value.trim())
      close()
    } catch (e) {
      setError(e?.message || 'Something went wrong.')
      setBusy(false)
    }
  }

  return (
    <Dialog
      title={title}
      description={description}
      onClose={onClose}
      initialFocusRef={inputRef}
      renderFooter={(close) => (
        <>
          <Button tone="neutral" onClick={close} disabled={busy}>{cancelLabel ?? t('common.cancel')}</Button>
          <Button tone="primary" sheen onClick={() => submit(close)} disabled={busy || Boolean(localError) || !value.trim()}>
            {busy ? t('common.working') : (confirmLabel ?? t('common.save'))}
          </Button>
        </>
      )}
    >
      {(close) => (
        <div>
          {label && <label className="mb-1.5 block text-xs font-bold uppercase tracking-wide text-text-dim">{label}</label>}
          <input
            ref={inputRef}
            type={type}
            value={value}
            onChange={(e) => setValue(e.target.value)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') {
                e.preventDefault()
                submit(close)
              }
            }}
            placeholder={placeholder}
            className="min-h-10 w-full rounded-lg border border-panel-border bg-bg px-3 py-2 text-sm text-text outline-none transition focus:border-accent focus:ring-2 focus:ring-accent/10"
          />
          {(localError || error) && <div className="mt-1.5 text-xs text-err">{localError || error}</div>}
        </div>
      )}
    </Dialog>
  )
}
