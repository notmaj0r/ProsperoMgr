import { forwardRef, useEffect, useImperativeHandle, useLayoutEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { useDismiss } from '../lib/useDismiss.js'
import { useAnchoredPosition } from '../lib/useAnchoredPosition.js'
import { useI18n } from '../lib/i18n.jsx'
import { pushBackLayer } from '../lib/backLayer.js'
import { isPS5 } from '../lib/effects.js'

// Right-clicking inside an element marked data-menu-host opens the ActionMenu that lives in that
// element, at the pointer, so every list and card gets the same menu as its "more" button.
const hostMenus = new WeakMap()
document.addEventListener('contextmenu', (event) => {
  const host = event.target.closest?.('[data-menu-host]')
  const openAt = host && hostMenus.get(host)
  if (!openAt || event.target.closest('input, textarea, select, a')) return
  event.preventDefault()
  openAt(event.clientX, event.clientY)
})

// "More" menu: a popover on desktop, a bottom sheet on narrow screens. '-' in `items` is a divider.
// The ref's openAt(x, y) opens it at the pointer, for a right-click.
const ActionMenu = forwardRef(function ActionMenu({ items, label, align = 'right', icon, buttonClassName }, ref) {
  const { t } = useI18n()
  const menuLabel = label || t('common.moreActions')
  const [open, setOpen] = useState(false)
  const [mobile, setMobile] = useState(false)
  const btnRef = useRef(null)
  const menuRef = useRef(null)
  const menuFocusedRef = useRef(false)
  const { closing, close } = useDismiss(() => setOpen(false))
  const { pos, measure } = useAnchoredPosition(open && !mobile)

  const [point, setPoint] = useState(null)

  const openMenu = (at) => {
    menuFocusedRef.current = false
    setPoint(at || null)
    setMobile(window.matchMedia('(max-width: 639px)').matches)
    setOpen(true)
  }

  const openAt = (x, y) => openMenu({ getBoundingClientRect: () => ({ top: y, bottom: y, left: x, right: x }) })
  useImperativeHandle(ref, () => ({ openAt }))
  const openAtRef = useRef(openAt)
  openAtRef.current = openAt
  useEffect(() => {
    const host = btnRef.current?.closest('[data-menu-host]')
    if (!host) return undefined
    hostMenus.set(host, (x, y) => openAtRef.current(x, y))
    return () => hostMenus.delete(host)
  }, [])

  // Menu must exist in the DOM (even off-screen) before its real height is
  // known — measure right after that first paint, not before.
  useLayoutEffect(() => {
    if (open && !mobile && menuRef.current) measure(point || btnRef.current, menuRef.current, point ? 'left' : align)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open, mobile])

  useEffect(() => {
    if (!open) return undefined
    const onClickOutside = (e) => {
      if (btnRef.current?.contains(e.target)) return
      if (menuRef.current?.contains(e.target)) return
      close()
    }
    const onKey = (e) => {
      if (e.key === 'Escape') close()
    }
    document.addEventListener('mousedown', onClickOutside)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onClickOutside)
      document.removeEventListener('keydown', onKey)
    }
  }, [open, close])

  const closeRef = useRef(close)
  closeRef.current = close
  useEffect(() => (open ? pushBackLayer(() => closeRef.current()) : undefined), [open])

  useEffect(() => {
    // On the console the cursor picks the item; a focused first row would look like a second pointer.
    if (!open || (!mobile && !pos) || menuFocusedRef.current || isPS5()) return undefined
    menuFocusedRef.current = true
    const frame = window.requestAnimationFrame(() => {
      menuRef.current?.querySelector('[role="menuitem"]:not(:disabled)')?.focus()
    })
    return () => window.cancelAnimationFrame(frame)
  }, [open, mobile, pos])

  useEffect(() => {
    if (!open) return undefined
    return () => btnRef.current?.focus()
  }, [open])

  const onMenuKeyDown = (event) => {
    if (isPS5() || !['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) return
    const items = Array.from(menuRef.current?.querySelectorAll('[role="menuitem"]:not(:disabled)') || [])
    if (items.length === 0) return
    event.preventDefault()
    const current = items.indexOf(document.activeElement)
    const next = event.key === 'Home' ? 0
      : event.key === 'End' ? items.length - 1
        : event.key === 'ArrowDown' ? (current + 1 + items.length) % items.length
          : (current - 1 + items.length) % items.length
    items[next].focus()
  }

  const visible = items.filter(Boolean)
  const ready = mobile || Boolean(pos)

  const rowClass = (item, dense) =>
    `flex w-full items-center gap-2.5 rounded-lg text-left font-medium disabled:opacity-40 ${
      dense ? 'px-2.5 py-2 text-sm' : 'px-3 py-3 text-sm font-semibold'
    } ${item.tone === 'danger' ? 'text-err hover:bg-err/10' : 'text-text hover:bg-panel-2'}`

  return (
    <>
      <button
        ref={btnRef}
        type="button"
        onClick={() => openMenu()}
        title={menuLabel}
        aria-label={menuLabel}
        aria-haspopup="menu"
        aria-expanded={open}
        className={buttonClassName || 'pmgr-icon-btn grid h-9 w-9 shrink-0 place-items-center rounded-lg border border-transparent text-text-dim hover:border-panel-border hover:bg-panel-border hover:text-text'}
      >
        {icon || (
          <svg width="16" height="16" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
            <circle cx="12" cy="5" r="1.8" /><circle cx="12" cy="12" r="1.8" /><circle cx="12" cy="19" r="1.8" />
          </svg>
        )}
      </button>

      {open && createPortal(
        mobile ? (
          <div
            className={`fixed top-0 right-0 bottom-0 left-0 z-50 flex items-end justify-center bg-black/45 pmgr-blur-sheet backdrop-blur-sm transition-opacity duration-150 ${closing ? 'opacity-0' : 'opacity-100'}`}
            onClick={close}
          >
            <div
              ref={menuRef}
              role="menu"
              aria-label={menuLabel}
              onKeyDown={onMenuKeyDown}
              className={`pmgr-sheet pmgr-max-h-dvh-70 flex w-full max-w-md flex-col overflow-y-auto rounded-t-2xl border-t border-panel-border bg-panel p-2 pb-[calc(0.5rem+env(safe-area-inset-bottom))] shadow-2xl ${closing ? 'is-closing' : ''}`}
              onClick={(e) => e.stopPropagation()}
            >
              <div className="mx-auto mb-2 mt-1 h-1 w-10 shrink-0 rounded-full bg-panel-border" />
              {visible.map((item, i) =>
                item === '-' ? (
                  <div key={i} className="my-1 border-t border-panel-border" />
                ) : (
                  <button
                    key={item.label}
                    type="button"
                    role="menuitem"
                    disabled={item.disabled}
                    onClick={() => {
                      close()
                      item.onClick()
                    }}
                    className={`min-h-11 ${rowClass(item, false)}`}
                  >
                    {item.icon}
                    {item.label}
                  </button>
                ),
              )}
            </div>
          </div>
        ) : (
          <div
            ref={menuRef}
            role="menu"
            aria-label={menuLabel}
            onKeyDown={onMenuKeyDown}
            style={{
              position: 'fixed',
              top: pos ? pos.top : -9999,
              left: pos ? pos.left : -9999,
              maxHeight: pos?.maxHeight,
              visibility: ready ? 'visible' : 'hidden',
              transformOrigin: pos?.openUpward ? 'bottom right' : 'top right',
            }}
            className={`pmgr-menu-pop z-50 min-w-48 overflow-y-auto rounded-xl border border-panel-border bg-panel p-1.5 shadow-2xl ${closing ? 'is-closing' : ''}`}
          >
            {visible.map((item, i) =>
              item === '-' ? (
                <div key={i} className="my-1 border-t border-panel-border" />
              ) : (
                <button
                  key={item.label}
                  type="button"
                  role="menuitem"
                  disabled={item.disabled}
                  onClick={() => {
                    close()
                    item.onClick()
                  }}
                  className={rowClass(item, true)}
                >
                  {item.icon}
                  {item.label}
                </button>
              ),
            )}
          </div>
        ),
        document.body,
      )}
    </>
  )
})

export default ActionMenu
