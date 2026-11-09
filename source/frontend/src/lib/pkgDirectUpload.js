// Direct-install WebSocket uploader: streams a local .pkg straight into the backend's
// RAM cache (pkg_ws_upload.cpp) so nothing lands on PS5 storage — see pkg_live_cache.hpp.

import { isPS5 } from './effects.js'

const SEG_SIZE = 1024 * 1024
// A refused segment is resent whole, so retries are rare and one at a time.
const BUSY_RETRY_MS = 250
// Slots left free beyond the reader's window, so ordinary jitter never leaves the cache without room.
const CACHE_SLACK = 32
// Segments in flight at once: throughput is WINDOW / ack round trip, and acks slow down
// once the installer is busy on the console, so this needs to be generous.
// Set pmgr-pkg-window in localStorage (8-64) to try other sizes when benchmarking.
const WINDOW = (() => {
  try {
    const tried = Number(window.localStorage.getItem('pmgr-pkg-window'))
    return tried >= 8 && tried <= 64 ? Math.floor(tried) : 24
  } catch {
    return 24
  }
})()
// How much of the file's end is sent first (must stay well under the console's cache size).
const TAIL_SEGS = 64
// Segments sent right behind a requested one, since the installer reads forward from there.
const SEEK_AHEAD = 7
// Segments posted at once; a browser keeps about six connections per address, and the page needs a few of them.
const POSTS = 4

// Module scope, not component state — the install outlives the view that started it.
let activeSocket = null
let activeCancel = null
const activeListeners = new Set()

function setActive(active) {
  for (const cb of activeListeners) cb(active)
}

// True from upload start until the backend closes the socket, well after "complete" —
// lets a caller warn the user not to close the tab early.
export function watchDirectUploadActive(cb) {
  activeListeners.add(cb)
  cb(activeSocket !== null)
  return () => activeListeners.delete(cb)
}

// Resolves once the backend has closed the upload socket (immediately if none is open).
export function waitForDirectUploadIdle() {
  return new Promise((resolve) => {
    if (!activeSocket) {
      resolve()
      return
    }
    const listener = (active) => {
      if (active) return
      activeListeners.delete(listener)
      resolve()
    }
    activeListeners.add(listener)
  })
}

function wsUrl() {
  return `ws://${location.hostname}:9046/ws/upload`
}

function cancelBeacon() {
  try {
    navigator.sendBeacon('/api/pkg/direct/cancel')
  } catch {
    /* best effort */
  }
}

// The install still needs this connection for resends after the tab moves on — warn
// before an accidental close, and tell the backend right away on an actual one.
window.addEventListener('beforeunload', (e) => {
  if (!activeSocket) return
  e.preventDefault()
  e.returnValue = ''
})
window.addEventListener('pagehide', () => {
  if (activeSocket) cancelBeacon()
})

// Aborts the upload in progress, if any; the backend drops the RAM session when the socket closes.
export function cancelDirectUpload() {
  if (activeCancel) activeCancel()
}

export function directUploadPkg(file, onProgress, title = '') {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(wsUrl())
    ws.binaryType = 'arraybuffer'
    activeSocket = ws
    activeCancel = () => {
      settle(() => reject(new Error('canceled')))
      cancelBeacon()
      ws.close()
    }
    setActive(true)

    const segCount = Math.ceil(file.size / SEG_SIZE)
    const ackedOnce = new Set()
    const inFlight = new Set() // segments sent but not yet acked
    let nextToSend = 0
    let finishSent = false
    let settled = false
    const sendQueue = [] // segments waiting for a free post
    let posting = 0
    const requests = new Set()
    const stat = { reads: 0, readMs: 0, acks: 0, ackMs: 0, queuedKb: 0, queuedSum: 0, sends: 0 }
    const sentAt = new Map()

    const settle = (fn) => {
      if (settled) return
      settled = true
      fn()
    }

    // Blob.arrayBuffer() is slow on the PS5's WebKit build, so only desktop browsers use it.
    const segmentBytes = (index) => {
      const start = index * SEG_SIZE
      const blob = file.slice(start, Math.min(start + SEG_SIZE, file.size))
      if (!isPS5() && blob.arrayBuffer) return blob.arrayBuffer()
      return new Promise((res, rej) => {
        const reader = new FileReader()
        reader.onload = () => res(reader.result)
        reader.onerror = () => rej(reader.error)
        reader.readAsArrayBuffer(blob)
      })
    }

    // Reads for the whole window start immediately and run concurrently — only the
    // wire sends are serialized, so disk I/O overlaps instead of gating one at a time.
    const prepared = new Map()
    const prepare = (index) => {
      if (!prepared.has(index)) {
        const began = performance.now()
        prepared.set(
          index,
          segmentBytes(index).then((buf) => {
            stat.reads++
            stat.readMs += performance.now() - began
            return buf
          }),
        )
      }
      return prepared.get(index)
    }

    // Posts one segment over HTTP: it reaches the console faster than a WebSocket message does.
    const postSegment = (index, buf) =>
      new Promise((resolve) => {
        const xhr = new XMLHttpRequest()
        requests.add(xhr)
        xhr.open('POST', `/api/pkg/live/segment?seg=${index}`)
        xhr.setRequestHeader('Content-Type', 'application/octet-stream')
        xhr.onload = () => {
          requests.delete(xhr)
          try {
            resolve(JSON.parse(xhr.responseText))
          } catch {
            resolve({ op: 'error' })
          }
        }
        xhr.onerror = () => {
          requests.delete(xhr)
          resolve({ op: 'error' })
        }
        xhr.send(buf)
      })

    const pump = () => {
      while (posting < POSTS && sendQueue.length && ws.readyState === WebSocket.OPEN) {
        const index = sendQueue.shift()
        posting++
        sentAt.set(index, performance.now())
        stat.queuedKb = Math.max(stat.queuedKb, sendQueue.length * (SEG_SIZE / 1024))
        stat.queuedSum += sendQueue.length * (SEG_SIZE / 1024)
        stat.sends++
        prepare(index)
          .then((buf) => {
            prepared.delete(index)
            return postSegment(index, buf)
          })
          .then((msg) => {
            posting--
            if (msg.op === 'ack') onAck(index)
            else if (msg.op === 'busy') onBusy(index)
            else settle(() => reject(new Error('The console refused part of the package.')))
            pump()
          })
          .catch((err) => settle(() => reject(err instanceof Error ? err : new Error('could not read the file'))))
      }
    }

    // A segment the installer is waiting on goes to the front of the line.
    const enqueueSegment = (index, urgent = false) => {
      prepare(index)
      if (urgent) sendQueue.unshift(index)
      else sendQueue.push(index)
      pump()
    }

    const enqueueFinish = () => {
      if (finishSent) return
      finishSent = true
      if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ op: 'finish' }))
    }

    // The installer reads the package tail (its entry tables) before anything else, so the tail goes
    // out right after segment 0 instead of waiting behind the whole file as seeks.
    let order = null
    let tailStart = segCount
    let readerSeg = 0 // where the installer is reading, as reported by the console
    const busyQueue = [] // segments the console had no room for, oldest position first
    let busyTimer = null
    let ahead = Infinity // how far past the reader the console's cache can hold
    const buildOrder = (from) => {
      if (from > 0) return Array.from({ length: segCount - from }, (_, i) => from + i)
      tailStart = Math.max(1, segCount - TAIL_SEGS)
      const list = segCount > 0 ? [0] : []
      for (let i = tailStart; i < segCount; i++) list.push(i)
      for (let i = 1; i < tailStart; i++) list.push(i)
      return list
    }

    const retryBusy = () => {
      window.clearTimeout(busyTimer)
      busyTimer = null
      const seg = busyQueue.shift()
      if (seg !== undefined) enqueueSegment(seg)
      scheduleBusyRetry(BUSY_RETRY_MS)
    }
    const scheduleBusyRetry = (delay) => {
      if (busyQueue.length && busyTimer === null) busyTimer = window.setTimeout(retryBusy, delay)
    }

    // Keeps up to WINDOW segments outstanding; each ack that clears one tops it back up,
    // so sending overlaps the ack round trip instead of waiting on it.
    const fillWindow = () => {
      while (inFlight.size < WINDOW && nextToSend < order.length) {
        const s = order[nextToSend]
        // Stay within what the cache can hold ahead of the reader; the head and tail are always sent.
        if (s > 0 && s < tailStart && s >= readerSeg + ahead) break
        nextToSend++
        // A seek may already have sent this one.
        if (ackedOnce.has(s) || inFlight.has(s)) continue
        inFlight.add(s)
        enqueueSegment(s)
      }
      if (nextToSend >= order.length && inFlight.size === 0) enqueueFinish()
    }

    // Averages since the last report, so the console log shows where the time goes.
    const statsTimer = window.setInterval(() => {
      if (ws.readyState !== WebSocket.OPEN || (!stat.reads && !stat.acks)) return
      const avg = (total, count) => (count ? (total / count).toFixed(1) : '0')
      ws.send(
        JSON.stringify({
          op: 'stats',
          read_ms: avg(stat.readMs, stat.reads),
          ack_ms: avg(stat.ackMs, stat.acks),
          queued_kb: String(stat.queuedKb),
          queued_avg_kb: stat.sends ? String(Math.round(stat.queuedSum / stat.sends)) : '0',
          in_flight: String(inFlight.size),
        }),
      )
      Object.assign(stat, { reads: 0, readMs: 0, acks: 0, ackMs: 0, queuedKb: 0, queuedSum: 0, sends: 0 })
    }, 6000)

    ws.onopen = () => {
      ws.send(JSON.stringify({ op: 'init', filename: file.name, total: file.size, title }))
    }

    const onAck = (seg) => {
      if (sentAt.has(seg)) {
        stat.acks++
        stat.ackMs += performance.now() - sentAt.get(seg)
        sentAt.delete(seg)
      }
      if (!ackedOnce.has(seg)) {
        ackedOnce.add(seg)
        onProgress(Math.min(ackedOnce.size * SEG_SIZE, file.size), file.size)
      }
      if (inFlight.delete(seg)) fillWindow()
    }

    const onBusy = (seg) => {
      if (!busyQueue.includes(seg)) busyQueue.push(seg)
      busyQueue.sort((a, b) => a - b)
      scheduleBusyRetry(BUSY_RETRY_MS)
    }

    ws.onmessage = (event) => {
      let msg
      try {
        msg = JSON.parse(event.data)
      } catch {
        return
      }
      if (msg.op === 'ready') {
        order = buildOrder(Math.min(Math.floor((msg.offset || 0) / SEG_SIZE), segCount))
        nextToSend = 0
        // A cache that holds the whole file never needs holding back.
        if (msg.slots && msg.slots < segCount + 4) ahead = Math.max(4, msg.slots - TAIL_SEGS - CACHE_SLACK)
        fillWindow()
      } else if (msg.op === 'pos') {
        readerSeg = msg.r || 0
        if (busyQueue.length) retryBusy()
        fillWindow()
      } else if (msg.op === 'seek') {
        // A segment already on its way needs no second copy, but one the console refused is resent now.
        const waiting = busyQueue.indexOf(msg.seg)
        if (waiting >= 0) busyQueue.splice(waiting, 1)
        else if (inFlight.has(msg.seg)) return
        inFlight.add(msg.seg)
        enqueueSegment(msg.seg, true)
        // The installer is reading here now, so the next few segments are what it will ask for next.
        // The requested one always goes out; the read-ahead only fills what the window has left.
        readerSeg = Math.max(readerSeg, msg.seg)
        for (let k = 1; k <= SEEK_AHEAD; k++) {
          const next = msg.seg + k
          if (inFlight.size >= WINDOW) break
          if (next < segCount && !ackedOnce.has(next) && !inFlight.has(next)) {
            inFlight.add(next)
            enqueueSegment(next)
          }
        }
      } else if (msg.op === 'complete') {
        settle(resolve)
        // Stay connected: the console keeps pulling from this session well past
        // "complete" — the socket only closes once the backend is done with it.
      } else if (msg.op === 'error') {
        settle(() => reject(new Error(msg.error || 'direct install failed')))
        ws.close()
      }
    }

    // The backend closes the socket once the console is done reading, which can beat the "complete" reply.
    const lost = () => {
      if (segCount > 0 && ackedOnce.size >= segCount) settle(resolve)
      else settle(() => reject(new Error('Connection to Prospero Manager was interrupted.')))
    }

    ws.onerror = lost

    ws.onclose = () => {
      window.clearInterval(statsTimer)
      requests.forEach((xhr) => xhr.abort())
      if (activeSocket === ws) {
        activeSocket = null
        activeCancel = null
        setActive(false)
      }
      lost()
    }
  })
}
