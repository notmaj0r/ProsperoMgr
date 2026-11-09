import { useCallback, useRef, useState } from 'react'
import { api } from './api.js'
import { usePolling } from './usePolling.js'

const WORKING = ['checking', 'downloading', 'launching']

// Polls the self-update state; reloads the page once the restarted backend reports a new version.
export function useUpdate() {
  const [status, setStatus] = useState(null)
  const loadedVersion = useRef(null)

  const refresh = useCallback(() =>
    api.update.status().then((s) => {
      if (loadedVersion.current === null) loadedVersion.current = s.current
      else if (s.current !== loadedVersion.current) location.reload()
      setStatus(s)
      return s
    }).catch(() => null), [])

  const working = Boolean(status && WORKING.includes(status.state))
  usePolling(refresh, working ? 1000 : 60000)

  return { status, refresh, working }
}
