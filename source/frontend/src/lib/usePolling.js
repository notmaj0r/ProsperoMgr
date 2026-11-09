import { useEffect, useRef } from 'react'
import { isPS5 } from './effects.js'

// The console's browser is slow enough that background polling is felt, so it polls 3x less often.
export const POLL_SCALE = isPS5() ? 3 : 1

// Runs `task` now and then every `ms`, skipping ticks while the tab is hidden and catching up on return.
export function usePolling(task, ms, enabled = true) {
  const taskRef = useRef(task)
  taskRef.current = task

  useEffect(() => {
    if (!enabled) return undefined
    const run = () => {
      if (document.visibilityState !== 'hidden') taskRef.current()
    }
    run()
    const id = setInterval(run, ms * POLL_SCALE)
    document.addEventListener('visibilitychange', run)
    return () => {
      clearInterval(id)
      document.removeEventListener('visibilitychange', run)
    }
  }, [ms, enabled])
}
