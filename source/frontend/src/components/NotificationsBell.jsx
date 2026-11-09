import { useEffect, useLayoutEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { useNotifications } from '../lib/ToastContext.jsx'
import { timeAgo } from '../lib/time.js'
import { useI18n } from '../lib/i18n.jsx'
import { useAnchoredPosition } from '../lib/useAnchoredPosition.js'
import { useBackLayer } from '../lib/backLayer.js'

export default function NotificationsBell() {
  const { t, locale } = useI18n()
  const TABS = [
    { id: 'all', label: t('notifications.tabAll') },
    { id: 'unread', label: t('notifications.tabUnread') },
  ]
  const { notifications, unreadCount, markAllRead, clearAll } = useNotifications()
  const [open, setOpen] = useState(false)
  useBackLayer(open, () => setOpen(false))
  const [tab, setTab] = useState('all')
  const ref = useRef(null)
  const btnRef = useRef(null)
  const dropdownRef = useRef(null)
  const { pos, measure } = useAnchoredPosition(open)

  useEffect(() => {
    if (!open) return
    const onClick = (e) => {
      if (ref.current?.contains(e.target)) return
      if (dropdownRef.current?.contains(e.target)) return
      setOpen(false)
    }
    const onKeyDown = (event) => {
      if (event.key !== 'Escape') return
      setOpen(false)
      btnRef.current?.focus()
    }
    document.addEventListener('mousedown', onClick)
    document.addEventListener('keydown', onKeyDown)
    return () => {
      document.removeEventListener('mousedown', onClick)
      document.removeEventListener('keydown', onKeyDown)
    }
  }, [open])

  // Portaled to <body>: Header's backdrop-filter makes it the containing block for
  // fixed children, which both mispositioned this dropdown and trapped its z-order.
  useLayoutEffect(() => {
    if (open && dropdownRef.current) measure(btnRef.current, dropdownRef.current, 'right')
  }, [open, measure])

  const toggle = () => setOpen((value) => !value)

  const items = tab === 'unread' ? notifications.filter((n) => !n.read) : notifications

  return (
    <div ref={ref} className="relative">
      <button
        ref={btnRef}
        onClick={toggle}
        title={t('notifications.title')}
        aria-label={t('notifications.title')}
        aria-haspopup="dialog"
        aria-expanded={open}
        className="pmgr-icon-btn relative grid h-9 w-9 place-items-center rounded-lg text-text-dim transition-colors hover:bg-panel-border hover:text-text"
      >
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7">
          <path d="M6 8a6 6 0 1 1 12 0c0 4.5 1.5 6 1.5 6h-15S6 12.5 6 8Z" strokeLinecap="round" strokeLinejoin="round" />
          <path d="M9.5 18a2.5 2.5 0 0 0 5 0" strokeLinecap="round" />
        </svg>
        {unreadCount > 0 && (
          <span
            key={unreadCount}
            className="pmgr-badge-pop absolute -right-0.5 -top-0.5 grid h-4 min-w-4 place-items-center rounded-full bg-accent px-1 text-[10px] font-bold text-white"
          >
            {unreadCount > 9 ? '9+' : unreadCount}
          </span>
        )}
      </button>

      {open && createPortal(
        <div
          ref={dropdownRef}
          role="dialog"
          aria-label={t('notifications.title')}
          style={{ position: 'fixed', top: pos?.top ?? -9999, left: pos?.left ?? -9999, maxHeight: pos?.maxHeight, visibility: pos ? 'visible' : 'hidden' }}
          className="pmgr-panel pmgr-modal-pop z-[100] flex w-[min(26rem,calc(100vw-1rem))] flex-col overflow-hidden rounded-lg border border-panel-border shadow-xl">
          <div className="flex items-center justify-between border-b border-panel-border px-3 py-2.5">
            <span className="text-sm font-semibold text-text">{t('notifications.title')}</span>
            <div className="flex items-center gap-3 text-xs">
              <button onClick={markAllRead} className="text-text-dim hover:text-text">
                {t('notifications.markAllRead')}
              </button>
              <button onClick={clearAll} className="text-err hover:underline">
                {t('notifications.clearAll')}
              </button>
            </div>
          </div>

          <div className="flex gap-1 border-b border-panel-border px-3 py-2">
            {TABS.map((tabItem) => (
              <button
                key={tabItem.id}
                onClick={() => setTab(tabItem.id)}
                className={`rounded-md px-2 py-1 text-xs font-medium ${
                  tab === tabItem.id ? 'bg-accent text-white' : 'text-text-dim hover:bg-panel-border hover:text-text'
                }`}
              >
                {tabItem.label}
              </button>
            ))}
          </div>

          <div className="min-h-0 flex-1 overflow-y-auto">
            {items.length === 0 && (
              <div className="py-6 text-center text-sm text-text-dim">{t('notifications.nothingHere')}</div>
            )}
            {items.map((n) => (
              <div key={n.id} className="flex items-start gap-2 border-b border-panel-border px-3 py-2.5 last:border-b-0">
                <span className={`mt-1 h-2 w-2 shrink-0 rounded-full ${n.tone === 'err' ? 'bg-err' : 'bg-warn'}`} />
                <div className="min-w-0 flex-1">
                  <div className="whitespace-pre-wrap break-words text-sm leading-relaxed text-text">{n.message}</div>
                  <div className="text-xs text-text-dim">{timeAgo(n.ts, locale)}</div>
                </div>
                {!n.read && <span className="ml-auto mt-1.5 h-1.5 w-1.5 shrink-0 rounded-full bg-accent" />}
              </div>
            ))}
          </div>
        </div>,
        document.body,
      )}
    </div>
  )
}
