import { useEffect, useRef } from 'react'
import { pushBackLayer } from './backLayer.js'

const FOCUSABLE = 'a[href], button:not(:disabled), textarea:not(:disabled), input:not(:disabled), select:not(:disabled), [tabindex]:not([tabindex="-1"])'
const stack = []

export function useDialogAccessibility(onEscape, initialFocusRef) {
  const containerRef = useRef(null)
  const escapeRef = useRef(onEscape)
  escapeRef.current = onEscape

  useEffect(() => {
    const token = {}
    const trigger = document.activeElement
    stack.push(token)
    const releaseBack = pushBackLayer(() => escapeRef.current?.())

    const frame = window.requestAnimationFrame(() => {
      if (stack[stack.length - 1] !== token) return
      const target = initialFocusRef?.current || containerRef.current?.querySelector(FOCUSABLE) || containerRef.current
      target?.focus()
    })

    const onKeyDown = (event) => {
      if (stack[stack.length - 1] !== token) return
      if (event.key === 'Escape') {
        event.preventDefault()
        escapeRef.current?.()
        return
      }
      if (event.key !== 'Tab') return

      const focusable = Array.from(containerRef.current?.querySelectorAll(FOCUSABLE) || [])
        .filter((element) => element.offsetParent !== null)
      if (focusable.length === 0) {
        event.preventDefault()
        containerRef.current?.focus()
        return
      }

      const first = focusable[0]
      const last = focusable[focusable.length - 1]
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault()
        last.focus()
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault()
        first.focus()
      }
    }

    document.addEventListener('keydown', onKeyDown)
    return () => {
      window.cancelAnimationFrame(frame)
      document.removeEventListener('keydown', onKeyDown)
      releaseBack()
      const index = stack.lastIndexOf(token)
      if (index >= 0) stack.splice(index, 1)
      if (trigger && typeof trigger.focus === 'function' && document.contains(trigger)) trigger.focus()
    }
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  return containerRef
}
