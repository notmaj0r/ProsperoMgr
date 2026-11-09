import { createContext, useCallback, useContext, useRef, useState } from 'react'
import { CheckCircleIcon, AlertTriangleIcon, XCircleIcon, CloseIcon } from '../components/icons.jsx'
import { useI18n } from './i18n.jsx'

const ToastContext = createContext(() => {})
const NotificationsContext = createContext({ notifications: [], unreadCount: 0, markAllRead: () => {}, clearAll: () => {} })

const TOAST_MS = 6000
// Must match .pmgr-toast-out's duration in index.css — the toast is removed
// from state only after its exit animation has actually finished playing.
const TOAST_EXIT_MS = 120

// showToast(message, tone, { action: { label, onClick }, duration }); tone is 'ok', 'err' or 'warn'.
export function useToast() {
  return useContext(ToastContext)
}

// Persistent history of the err/warn toasts, for the notifications bell.
export function useNotifications() {
  return useContext(NotificationsContext)
}

const TONE_CLASSES = {
  ok: 'border-ok/40 bg-panel text-text',
  err: 'border-err/40 bg-err/10 text-err',
  warn: 'border-warn/40 bg-warn/10 text-warn',
}

const TONE_ICONS = {
  ok: CheckCircleIcon,
  err: XCircleIcon,
  warn: AlertTriangleIcon,
}

const TONE_BAR = {
  ok: 'bg-ok',
  err: 'bg-err',
  warn: 'bg-warn',
}

const TONE_ICON_CLASS = {
  ok: 'text-ok',
  err: 'text-err',
  warn: 'text-warn',
}

export function ToastProvider({ children }) {
  const { t } = useI18n()
  const [toasts, setToasts] = useState([])
  const [notifications, setNotifications] = useState([])
  const idRef = useRef(0)
  const notifIdRef = useRef(0)

  // Mark it closing so .pmgr-toast-out can play, then drop it once the exit
  // animation has had time to finish.
  const dismiss = useCallback((id) => {
    setToasts((t) => t.map((x) => (x.id === id ? { ...x, closing: true } : x)))
    setTimeout(() => {
      setToasts((t) => t.filter((x) => x.id !== id))
    }, TOAST_EXIT_MS)
  }, [])

  const showToast = useCallback((message, tone = 'ok', options = {}) => {
    const id = ++idRef.current
    const duration = options.duration || TOAST_MS
    setToasts((t) => [...t, { id, message, tone, closing: false, action: options.action, duration }])
    setTimeout(() => dismiss(id), duration)

    // Only failures/warnings are worth surfacing again later in the bell.
    if (tone === 'err' || tone === 'warn') {
      const nid = ++notifIdRef.current
      setNotifications((n) => [{ id: nid, message, tone, ts: Date.now(), read: false }, ...n].slice(0, 50))
    }
  }, [dismiss])

  const markAllRead = useCallback(() => {
    setNotifications((n) => n.map((x) => ({ ...x, read: true })))
  }, [])

  const clearAll = useCallback(() => setNotifications([]), [])

  const unreadCount = notifications.filter((n) => !n.read).length

  return (
    <ToastContext.Provider value={showToast}>
      <NotificationsContext.Provider value={{ notifications, unreadCount, markAllRead, clearAll }}>
        {children}
        <div aria-live="polite" aria-atomic="false" className="pointer-events-none fixed left-3 right-3 top-3 z-50 flex flex-col items-end gap-2 sm:left-auto sm:right-4 sm:top-4">
          {toasts.map((toast) => {
            const Icon = TONE_ICONS[toast.tone] || TONE_ICONS.ok
            return (
              <div
                key={toast.id}
                role={toast.tone === 'err' ? 'alert' : 'status'}
                className={`pmgr-toast ${toast.closing ? 'is-closing' : ''} pointer-events-auto relative w-full max-w-md overflow-hidden rounded-lg border py-3 pl-3 pr-9 text-sm font-medium shadow-lg backdrop-blur sm:w-auto sm:min-w-72 ${TONE_CLASSES[toast.tone] || TONE_CLASSES.ok}`}
              >
                <div className="flex items-start gap-2">
                  <span className={`mt-0.5 shrink-0 ${TONE_ICON_CLASS[toast.tone] || TONE_ICON_CLASS.ok}`}><Icon size={16} /></span>
                  <span className="whitespace-pre-wrap break-words leading-relaxed">{toast.message}</span>
                  {toast.action && (
                    <button
                      type="button"
                      onClick={() => { toast.action.onClick(); dismiss(toast.id) }}
                      className="pmgr-icon-btn ml-2 shrink-0 rounded-md border border-current px-2 py-0.5 text-xs font-bold hover:bg-panel-border"
                    >
                      {toast.action.label}
                    </button>
                  )}
                </div>
                <button
                  onClick={() => dismiss(toast.id)}
                  aria-label={t('common.close')}
                  className="pmgr-icon-btn !absolute right-2 top-2 text-current opacity-50 hover:opacity-100"
                >
                  <CloseIcon size={13} />
                </button>
                <div
                  className={`pmgr-toast-bar absolute bottom-0 left-0 right-0 h-0.5 origin-left ${TONE_BAR[toast.tone] || TONE_BAR.ok}`}
                  style={{ animationDuration: `${toast.duration}ms` }}
                />
              </div>
            )
          })}
        </div>
      </NotificationsContext.Provider>
    </ToastContext.Provider>
  )
}
