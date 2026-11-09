// Upload/download progress lives outside the Files page, so leaving it mid-transfer
// does not lose the bar: the transfer keeps running and the page shows it again on return.
import { useSyncExternalStore } from 'react'

let transfers = []
const listeners = new Set()

function update(next) {
  transfers = next
  listeners.forEach((listener) => listener())
}

export function startTransfer(name, direction, total, onCancel) {
  const id = `${direction}-${name}-${Date.now()}`
  update([...transfers, { id, name, direction, loaded: 0, total, startedAt: performance.now(), speed: 0, done: false, error: '', retries: 0, onCancel }])
  return id
}

export function progressTransfer(id, loaded, total, name) {
  update(
    transfers.map((tr) => {
      if (tr.id !== id) return tr
      const elapsed = (performance.now() - tr.startedAt) / 1000
      const speed = elapsed > 0.2 ? loaded / elapsed : tr.speed
      return { ...tr, loaded, total: total || tr.total, speed, name: name ?? tr.name }
    }),
  )
}

export function finishTransfer(id, error) {
  update(transfers.map((tr) => (tr.id === id ? { ...tr, done: true, error: error || '' } : tr)))
  setTimeout(() => update(transfers.filter((tr) => tr.id !== id)), error ? 4000 : 1500)
}

export function retryTransfer(id) {
  update(transfers.map((tr) => (tr.id === id ? { ...tr, loaded: 0, startedAt: performance.now(), speed: 0, retries: tr.retries + 1 } : tr)))
}

const subscribe = (listener) => {
  listeners.add(listener)
  return () => listeners.delete(listener)
}
const getSnapshot = () => transfers

export function useTransfers() {
  const list = useSyncExternalStore(subscribe, getSnapshot)
  return { transfers: list, start: startTransfer, progress: progressTransfer, finish: finishTransfer, retry: retryTransfer }
}
