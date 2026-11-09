import { useEffect, useLayoutEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { useBackLayer } from '../lib/backLayer.js'
import { useDismiss } from '../lib/useDismiss.js'
import { useAnchoredPosition } from '../lib/useAnchoredPosition.js'
import { useI18n, LOCALES } from '../lib/i18n.jsx'
import { isPS5 } from '../lib/effects.js'

// Compact flag button — desktop gets an anchored popover (same placement logic as ActionMenu),
// narrow layouts a bottom sheet. Native language name is always in the DOM too, not just the flag.
export default function LanguagePicker() {
  const { locale, setLocale, t } = useI18n()
  const [open, setOpen] = useState(false)
  const [mobile, setMobile] = useState(false)
  const btnRef = useRef(null)
  const menuRef = useRef(null)
  const menuFocusedRef = useRef(false)
  const { closing, close } = useDismiss(() => setOpen(false))
  useBackLayer(open, close)
  const { pos, measure } = useAnchoredPosition(open && !mobile)

  const current = LOCALES.find((l) => l.id === locale) || LOCALES[0]

  const openMenu = () => {
    menuFocusedRef.current = false
    setMobile(window.matchMedia('(max-width: 639px)').matches)
    setOpen(true)
  }

  useLayoutEffect(() => {
    if (open && !mobile && menuRef.current) measure(btnRef.current, menuRef.current, 'right')
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [open, mobile])

  useEffect(() => {
    if (!open) return undefined
    const onClickOutside = (e) => {
      if (btnRef.current?.contains(e.target)) return
      if (menuRef.current?.contains(e.target)) return
      close()
    }
    const onKey = (e) => e.key === 'Escape' && close()
    document.addEventListener('mousedown', onClickOutside)
    document.addEventListener('keydown', onKey)
    return () => {
      document.removeEventListener('mousedown', onClickOutside)
      document.removeEventListener('keydown', onKey)
    }
  }, [open, close])

  useEffect(() => {
    if (!open || (!mobile && !pos) || menuFocusedRef.current) return undefined
    menuFocusedRef.current = true
    const frame = window.requestAnimationFrame(() => {
      menuRef.current?.querySelector('[role="menuitemradio"]')?.focus()
    })
    return () => window.cancelAnimationFrame(frame)
  }, [open, mobile, pos])

  useEffect(() => {
    if (!open) return undefined
    return () => btnRef.current?.focus()
  }, [open])

  const onMenuKeyDown = (event) => {
    if (isPS5() || !['ArrowDown', 'ArrowUp', 'Home', 'End'].includes(event.key)) return
    const items = Array.from(menuRef.current?.querySelectorAll('[role="menuitemradio"]') || [])
    if (items.length === 0) return
    event.preventDefault()
    const currentIndex = items.indexOf(document.activeElement)
    const next = event.key === 'Home' ? 0
      : event.key === 'End' ? items.length - 1
        : event.key === 'ArrowDown' ? (currentIndex + 1 + items.length) % items.length
          : (currentIndex - 1 + items.length) % items.length
    items[next].focus()
  }

  const choose = (id) => {
    setLocale(id)
    close()
  }

  const ready = mobile || Boolean(pos)

  const list = (dense) => (
    <>
      {LOCALES.map((l) => (
        <button
          key={l.id}
          type="button"
          role="menuitemradio"
          aria-checked={l.id === locale}
          onClick={() => choose(l.id)}
          className={`flex w-full items-center gap-3 rounded-lg text-left transition-colors ${
            dense ? 'px-2.5 py-2 text-sm' : 'px-3 py-3 text-sm font-semibold'
          } ${l.id === locale ? 'bg-accent/10 text-accent' : 'text-text hover:bg-panel-2'}`}
        >
          <img src={l.flag} alt="" aria-hidden="true" className="h-3.5 w-5 shrink-0 rounded-[2px] object-cover" />
          <span className="min-w-0 flex-1 truncate">{l.name}</span>
          {l.id === locale && (
            <svg width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" strokeLinejoin="round" className="shrink-0">
              <path d="M5 13l4 4L19 7" />
            </svg>
          )}
        </button>
      ))}
    </>
  )

  return (
    <>
      <button
        ref={btnRef}
        type="button"
        onClick={openMenu}
        title={t('language.current', { name: current.name })}
        aria-label={t('language.picker')}
        aria-haspopup="menu"
        aria-expanded={open}
        className="pmgr-icon-btn grid h-9 min-w-9 place-items-center rounded-lg border border-transparent px-1.5 text-base leading-none hover:border-panel-border hover:bg-panel-border"
      >
        <img src={current.flag} alt="" aria-hidden="true" className="h-3.5 w-5 rounded-[2px] object-cover" />
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
              aria-label={t('language.select')}
              onKeyDown={onMenuKeyDown}
              className={`pmgr-sheet pmgr-max-h-dvh-75 flex w-full max-w-md flex-col overflow-y-auto rounded-t-2xl border-t border-panel-border bg-panel p-2 pb-[calc(0.5rem+env(safe-area-inset-bottom))] shadow-2xl ${closing ? 'is-closing' : ''}`}
              onClick={(e) => e.stopPropagation()}
            >
              <div className="mx-auto mb-2 mt-1 h-1 w-10 shrink-0 rounded-full bg-panel-border" />
              <div className="px-2 pb-2 text-xs font-bold uppercase tracking-wide text-text-dim">{t('language.select')}</div>
              {list(false)}
            </div>
          </div>
        ) : (
          <div
            ref={menuRef}
            role="menu"
            aria-label={t('language.select')}
            onKeyDown={onMenuKeyDown}
            style={{
              position: 'fixed',
              top: pos ? pos.top : -9999,
              left: pos ? pos.left : -9999,
              maxHeight: pos?.maxHeight,
              visibility: ready ? 'visible' : 'hidden',
            }}
            className={`pmgr-menu-pop z-50 max-h-96 w-56 overflow-y-auto rounded-xl border border-panel-border bg-panel p-1.5 shadow-2xl ${closing ? 'is-closing' : ''}`}
          >
            {list(true)}
          </div>
        ),
        document.body,
      )}
    </>
  )
}
