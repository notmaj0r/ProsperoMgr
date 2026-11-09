// A page's own sub-navigation (an open album, a tab) as history steps, so Back and Forward move through it.
// `route` is the page's hash prefix: a page kept mounted while hidden ignores steps taken elsewhere.
import { useCallback, useEffect, useState } from 'react'

const read = (key, initial) => window.history.state?.pmgr?.[key] ?? initial

export function useHistoryState(key, initial, route) {
  const [value, setValue] = useState(() => read(key, initial))

  useEffect(() => {
    const onHistory = () => {
      if (window.location.hash.startsWith(route)) setValue(read(key, initial))
    }
    window.addEventListener('popstate', onHistory)
    return () => window.removeEventListener('popstate', onHistory)
  }, [key, route]) // eslint-disable-line react-hooks/exhaustive-deps

  // `replace` changes the value without adding a step, for changes the page makes by itself.
  const update = useCallback((next, replace = false) => {
    const state = { pmgr: { ...(window.history.state?.pmgr || {}), [key]: next } }
    if (replace) window.history.replaceState({ ...window.history.state, ...state }, '')
    else window.history.pushState(state, '')
    setValue(next)
  }, [key])

  return [value, update]
}
