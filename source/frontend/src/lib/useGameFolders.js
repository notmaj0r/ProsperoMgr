import { useEffect, useState } from 'react'
import { api } from './api.js'

const BATCH = 150

// Maps each game folder and disk image (.exfat, .ffpfsc, .ffpkg) in `entries` to its name and cover. They are recognised by their
// sce_sys metadata, whatever they are named. A folder that is not a game maps to null.
export function useGameFolders(entries, path) {
  const [games, setGames] = useState({})

  useEffect(() => {
    const base = path.endsWith('/') ? path : `${path}/`
    const pending = entries
      .filter((e) => e.is_dir || /\.(exfat|ffpfsc|ffpkg)$/i.test(e.name))
      .map((e) => base + e.name)
      .filter((p) => !(p in games))
    if (pending.length === 0) return
    let cancelled = false
    // Batches keep one request small, and a folder with hundreds of games still gets all of them identified.
    const identifyFrom = (offset) => {
      const batch = pending.slice(offset, offset + BATCH)
      if (batch.length === 0 || cancelled) return
      api.games
        .identify(batch)
        .then((r) => {
          if (cancelled) return
          const found = Object.fromEntries((r.games || []).map((g) => [g.path, g]))
          setGames((prev) => ({ ...prev, ...Object.fromEntries(batch.map((p) => [p, found[p] || null])) }))
          identifyFrom(offset + BATCH)
        })
        .catch(() => {})
    }
    identifyFrom(0)
    return () => { cancelled = true }
  }, [entries, path]) // eslint-disable-line react-hooks/exhaustive-deps

  return games
}
