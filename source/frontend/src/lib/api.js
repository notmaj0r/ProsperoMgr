// Thin fetch wrapper for the /api/* routes in backend/src/api_routes.cpp.
// Plain functions, no client-state library — views manage their own loading/error state.

import { cancelDirectUpload, directUploadPkg } from './pkgDirectUpload.js'

// fetch() throws a bare TypeError when the connection itself drops, not when the server
// answers with an HTTP error — name it plainly instead of the browser's opaque "Failed to fetch".
function isNetworkLayerError(e) {
  return e instanceof TypeError
}
const NETWORK_ERROR_HINT =
  'Connection to Prospero Manager was interrupted. The app will reconnect automatically; retry this action if it did not finish.'

async function request(method, path, body) {
  const opts = { method, headers: {} }
  if (body !== undefined) {
    opts.headers['Content-Type'] = 'application/json'
    opts.body = JSON.stringify(body)
  }
  let res
  const attempts = method === 'GET' ? 2 : 1
  for (let attempt = 0; attempt < attempts; attempt++) {
    try {
      res = await fetch(path, opts)
      break
    } catch (e) {
      if (attempt + 1 < attempts && isNetworkLayerError(e)) {
        await new Promise((resolve) => window.setTimeout(resolve, 350))
        continue
      }
      throw new Error(isNetworkLayerError(e) ? NETWORK_ERROR_HINT : e.message)
    }
  }
  const data = await res.json().catch(() => ({}))
  if (!res.ok) {
    // Attach the rest of the error body to the Error; callers that only read
    // .message are unaffected.
    throw Object.assign(new Error(data.error || `${method} ${path} failed (${res.status})`), data)
  }
  return data
}

// Raw-body POST, used for file uploads (drag-and-drop and the file picker both land here).
async function uploadRaw(path, file) {
  let res
  try {
    res = await fetch(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body: file,
    })
  } catch (e) {
    throw new Error(isNetworkLayerError(e) ? NETWORK_ERROR_HINT : e.message)
  }
  const data = await res.json().catch(() => ({}))
  if (!res.ok) throw new Error(data.error || 'upload failed')
  return data
}

// Coalesces XHR progress events to roughly `intervalMs` apart — firing every one into React
// state fights the browser for CPU. Never drops the completing event, so the bar reaches 100%.
function throttleProgress(onProgress, intervalMs = 120) {
  let lastFired = 0
  let pending = null
  let timer = null
  const flush = () => {
    timer = null
    if (!pending) return
    lastFired = performance.now()
    const [loaded, total] = pending
    pending = null
    onProgress(loaded, total)
  }
  return (loaded, total) => {
    if (loaded >= total) {
      if (timer) {
        clearTimeout(timer)
        timer = null
      }
      pending = null
      lastFired = performance.now()
      onProgress(loaded, total)
      return
    }
    pending = [loaded, total]
    const elapsed = performance.now() - lastFired
    if (elapsed >= intervalMs) flush()
    else if (!timer) timer = setTimeout(flush, intervalMs - elapsed)
  }
}

// XHR instead of fetch because fetch has no upload-progress event — this is
// what actually drives the speed/ETA readout on the Files transfer bar.
function uploadWithProgress(path, file, onProgress, options = {}) {
  return new Promise((resolve, reject) => {
    let attempt = 0
    const retries = options.retries || 0
    const reportProgress = throttleProgress(onProgress)

    const run = () => {
      const xhr = new XMLHttpRequest()
      xhr.open('POST', path)
      xhr.setRequestHeader('Content-Type', 'application/octet-stream')
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) reportProgress(e.loaded, e.total)
      }
      xhr.onload = () => {
        let data = {}
        try {
          data = JSON.parse(xhr.responseText)
        } catch {
          /* non-JSON error body — fall through with empty data */
        }
        if (xhr.status >= 200 && xhr.status < 300) resolve(data)
        else reject(new Error(data.error || `upload failed (${xhr.status})`))
      }
      const networkFailure = () => {
        if (attempt < retries) {
          attempt++
          options.onRetry?.(attempt)
          window.setTimeout(run, 500 * attempt)
          return
        }
        reject(new Error(NETWORK_ERROR_HINT))
      }
      xhr.onerror = networkFailure
      xhr.ontimeout = networkFailure
      xhr.send(file)
    }
    run()
  })
}

const CHUNK_STALL_MS = 30000

// Chunk size scales with file size — big enough for TCP to reach steady state on a large
// transfer, capped under the backend's 264 MiB limit and matched to its 256 MiB fsync checkpoint.
function chunkSizeFor(fileSize) {
  const MiB = 1024 * 1024
  if (fileSize >= 4 * 1024 * MiB) return 256 * MiB
  if (fileSize >= 512 * MiB) return 128 * MiB
  if (fileSize >= 128 * MiB) return 64 * MiB
  return 16 * MiB
}

// A game-sized upload shouldn't depend on one hours-long connection — each
// chunk commits to its checkpoint independently and retries are idempotent.
function uploadResumable(path, file, onProgress, onRetry, overwrite = false, signal) {
  const overwriteQuery = overwrite ? '&overwrite=1' : ''
  if (file.size === 0) return uploadRaw(`/api/ftp/upload?path=${encodeURIComponent(path)}${overwriteQuery}`, file)

  // Keep each HTTP request short enough to survive the PS5 browser/network
  // stack while retaining large sequential writes and resumability.
  const chunkSize = chunkSizeFor(file.size)
  const uploadId = `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`
  let offset = 0
  const reportProgress = throttleProgress(onProgress)

  return new Promise((resolve, reject) => {
    let activeXhr = null
    // Frees the console's reservation and partial file whenever the upload ends early.
    const abandon = (error) => {
      activeXhr?.abort()
      request('POST', '/api/ftp/upload/cancel', { path, upload_id: uploadId }).catch(() => {})
      reject(error)
    }
    signal?.addEventListener('abort', () => abandon(new Error('canceled')), { once: true })
    const sendChunk = () => {
      const end = Math.min(file.size, offset + chunkSize)
      const chunk = file.slice(offset, end)
      let attempt = 0

      const run = () => {
        if (signal?.aborted) return
        const xhr = new XMLHttpRequest()
        activeXhr = xhr
        const query = `path=${encodeURIComponent(path)}&upload_id=${encodeURIComponent(uploadId)}&offset=${offset}&total=${file.size}${overwriteQuery}`
        xhr.open('POST', `/api/ftp/upload/chunk?${query}`)
        xhr.setRequestHeader('Content-Type', 'application/octet-stream')
        let stallTimer = 0
        const retryChunk = () => {
          window.clearTimeout(stallTimer)
          if (attempt < 3) {
            attempt++
            onRetry?.(attempt)
            window.setTimeout(run, 400 * attempt)
          } else {
            abandon(new Error(NETWORK_ERROR_HINT))
          }
        }
        // A stalled connection raises no error event, so retry the chunk after a quiet spell.
        const armStall = () => {
          window.clearTimeout(stallTimer)
          stallTimer = window.setTimeout(() => {
            xhr.abort()
            retryChunk()
          }, CHUNK_STALL_MS)
        }
        xhr.upload.onprogress = (event) => {
          armStall()
          reportProgress(offset + event.loaded, file.size)
        }
        xhr.onload = () => {
          window.clearTimeout(stallTimer)
          let data = {}
          try {
            data = JSON.parse(xhr.responseText)
          } catch {
            /* handled by status below */
          }
          if (xhr.status < 200 || xhr.status >= 300) {
            abandon(new Error(data.error || `upload failed (${xhr.status})`))
            return
          }
          const next = Number(data.next_offset)
          if (!Number.isFinite(next) || next <= offset || next > file.size) {
            abandon(new Error('server returned an invalid upload checkpoint'))
            return
          }
          offset = next
          reportProgress(offset, file.size)
          if (data.complete || offset === file.size) resolve(data)
          else sendChunk()
        }
        xhr.onerror = retryChunk
        xhr.ontimeout = retryChunk
        armStall()
        xhr.send(chunk)
      }
      run()
    }
    sendChunk()
  })
}

export const api = {
  status: () => request('GET', '/api/status'),
  tasks: () => request('GET', '/api/tasks'),
  paths: () => request('GET', '/api/paths'),
  setPayloadsDir: (path) => request('POST', '/api/paths/payloads-dir', { path }),
  system: {
    info: () => request('GET', '/api/system/info'),
  },

  payloads: {
    list: () => request('GET', '/api/payloads'),
    launch: (name) => request('POST', '/api/payloads/launch', { name }),
    delete: (name) => request('POST', '/api/payloads/delete', { name }),
    setEnabled: (name, enabled) => request('POST', '/api/payloads/enable', { name, enabled }),
    setFavorite: (name, favorite) => request('POST', '/api/payloads/favorite', { name, favorite }),
    rename: (name, newName) => request('POST', '/api/payloads/rename', { name, new_name: newName }),
    importPath: (path) => request('POST', '/api/payloads/import-path', { path }),
    upload: (file, onProgress, onRetry, name = file.name) =>
      uploadWithProgress(`/api/payloads/upload?name=${encodeURIComponent(name)}`, file, onProgress || (() => {}), {
        retries: 1,
        onRetry,
      }),
    // Direct-URL install job — used by the Payload Repositories tab's
    // Download action. Progress is reported under /api/tasks as "payload".
    download: (url, version, launch, repoUrl) =>
      request('GET', `/api/payloads/download?url=${encodeURIComponent(url)}${version ? `&version=${encodeURIComponent(version)}` : ''}${launch ? '&launch=1' : ''}${repoUrl ? `&repo_url=${encodeURIComponent(repoUrl)}` : ''}`),
    downloadStatus: () => request('GET', '/api/payloads/download/status'),
  },

  autoloader: {
    status: () => request('GET', '/api/autoloader/status'),
    abort: () => request('POST', '/api/autoloader/abort'),
    run: () => request('POST', '/api/autoloader/run'),
    getConfig: () => request('GET', '/api/autoloader/config'),
    setConfig: (config) => request('POST', '/api/autoloader/config', config),
    getBundledAutostart: () => request('GET', '/api/autoloader/bundled-autostart'),
    setBundledAutostart: (enabled) => request('POST', '/api/autoloader/bundled-autostart', { enabled }),
  },

  catalog: {
    list: (refresh) => request('GET', `/api/catalog${refresh ? '?refresh=1' : ''}`),
    addSource: (url) => request('POST', '/api/catalog/sources/add', { url }),
    removeSource: (url) => request('POST', '/api/catalog/sources/remove', { url }),
    releases: (url) => request('GET', `/api/catalog/releases?url=${encodeURIComponent(url)}`),
    refreshEntry: (url) => request('GET', `/api/catalog/refresh?url=${encodeURIComponent(url)}`),
    getGithubToken: () => request('GET', '/api/catalog/github-token'),
    setGithubToken: (token) => request('POST', '/api/catalog/github-token', { token }),
    clearGithubToken: () => request('POST', '/api/catalog/github-token/clear'),
  },

  experimental: {
    getShadowMountScanPaths: () => request('GET', '/api/experimental/shadowmount/scan-paths'),
    setShadowMountScanPaths: (scanPaths) => request('POST', '/api/experimental/shadowmount/scan-paths', { scan_paths: scanPaths }),
  },

  processes: {
    list: () => request('GET', '/api/processes'),
    kill: (pid) => request('POST', '/api/processes/kill', { pid }),
  },

  ftp: {
    status: () => request('GET', '/api/ftp/status'),
    storage: () => request('GET', '/api/ftp/storage'),
    ejectUsb: (path) => request('POST', '/api/storage/eject', { path }),
    // `isDir` for a directory destination (e.g. an extraction target) —
    // checks free space on that directory itself rather than its parent.
    preflight: (path, size, isDir) =>
      request('GET', `/api/ftp/preflight?path=${encodeURIComponent(path)}&size=${size}${isDir ? '&is_dir=1' : ''}`),
    list: (path) => request('GET', `/api/ftp/list?path=${encodeURIComponent(path)}`),
    dirSize: (path) => request('GET', `/api/ftp/dirsize?path=${encodeURIComponent(path)}`),
    conflicts: (paths) => request('POST', '/api/ftp/conflicts', { paths }),
    uniqueNames: (paths) => request('POST', '/api/ftp/unique-names', { paths }),
    mkdir: (path) => request('POST', '/api/ftp/mkdir', { path }),
    mkdirs: (paths) => request('POST', '/api/ftp/mkdirs', { paths }),
    sync: () => request('POST', '/api/ftp/sync'),
    delete: (path, type) => request('POST', '/api/ftp/delete', { path, type }),
    rename: (from, to) => request('POST', '/api/ftp/rename', { from, to }),
    chmod: (path, mode, recursive = false) => request('POST', '/api/ftp/chmod', { path, mode, recursive }),
    copy: (source, destination, conflictPolicy = 'cancel') =>
      request('POST', '/api/ftp/copy', { source, destination, conflict_policy: conflictPolicy }),
    move: (source, destination, conflictPolicy = 'cancel') =>
      request('POST', '/api/ftp/move', { source, destination, conflict_policy: conflictPolicy }),
    downloadUrl: (path) => `/api/ftp/download?path=${encodeURIComponent(path)}`,
    downloadDecryptedUrl: (path) => `/api/ftp/download-decrypted?path=${encodeURIComponent(path)}`,
    upload: (path, file, overwrite = false) =>
      uploadRaw(`/api/ftp/upload?path=${encodeURIComponent(path)}${overwrite ? '&overwrite=1' : ''}`, file),
    uploadWithProgress: (path, file, onProgress, onRetry, overwrite = false, signal) =>
      uploadResumable(path, file, onProgress, onRetry, overwrite, signal),
    // Text reads include the version and original line-ending format.
    readText: async (path) => {
      let res
      try {
        res = await fetch(`/api/ftp/text?path=${encodeURIComponent(path)}`)
      } catch (e) {
        throw new Error(isNetworkLayerError(e) ? NETWORK_ERROR_HINT : e.message)
      }
      if (!res.ok) throw new Error((await res.json().catch(() => ({}))).error || 'read failed')
      return res.json()
    },
    writeText: (path, text, metadata) => {
      if (!metadata?.version) {
        return uploadRaw(`/api/ftp/upload?path=${encodeURIComponent(path)}&overwrite=1`, new Blob([text], { type: 'text/plain' }))
      }
      const newline = metadata.newline === 'crlf' ? '\r\n' : metadata.newline === 'cr' ? '\r' : '\n'
      const body = text.replace(/\r\n|\r|\n/g, newline)
      const bytes = metadata.bom ? `\uFEFF${body}` : body
      return uploadRaw(
        `/api/ftp/text/write?path=${encodeURIComponent(path)}&version=${encodeURIComponent(metadata.version)}`,
        new Blob([bytes], { type: 'text/plain;charset=utf-8' }),
      )
    },
    // Every Create-Archive flow goes through zip-selection: it takes a chosen name
    // and treats a one-item `paths` array like any multi-selection.
    zipSelection: (paths, dir, name, compression) => request('POST', '/api/ftp/zip-selection', { paths, dir, name, compression }),
    downloadZip: (paths, name) => request('POST', '/api/ftp/download-zip', { paths, name }),
    archiveInfo: (path, password) =>
      request('GET', `/api/ftp/archive-info?path=${encodeURIComponent(path)}${password ? `&password=${encodeURIComponent(password)}` : ''}`),
    unzip: (path, destination, conflictPolicy, deleteSource, password) =>
      request('POST', '/api/ftp/unzip', { path, destination, conflict_policy: conflictPolicy, delete_source: deleteSource, password }),
    jobs: () => request('GET', '/api/files/jobs'),
    cancelJob: (id) => request('POST', '/api/files/jobs/cancel', { id }),
    clearJobs: (onlyDone = false) => request('POST', '/api/files/jobs/clear', onlyDone ? { only_done: true } : undefined),
    pauseJobs: (paused) => request('POST', '/api/files/jobs/pause', { paused }),
    moveJob: (id, direction) => request('POST', '/api/files/jobs/move', { id, direction }),
  },

  elfldr: {
    status: () => request('GET', '/api/elfldr/status'),
  },

  titles: {
    list: () => request('GET', '/api/titles'),
    running: () => request('GET', '/api/titles/running'),
    size: (titleId) => request('GET', `/api/titles/size?id=${encodeURIComponent(titleId)}`),
    iconUrl: (titleId) => `/api/titles/icon?id=${encodeURIComponent(titleId)}`,
    launch: (titleId) => request('POST', '/api/titles/launch', { title_id: titleId }),
    close: () => request('POST', '/api/titles/close'),
    uninstall: (titleId) => request('POST', '/api/titles/uninstall', { title_id: titleId }),
  },

  media: {
    list: () => request('GET', '/api/media/list'),
    fileUrl: (path, download = false) => `/api/media/file?path=${encodeURIComponent(path)}${download ? '&download=1' : ''}`,
    thumbUrl: (path) => `/api/media/thumb?path=${encodeURIComponent(path)}`,
    remove: (paths) => request('POST', '/api/media/delete', { paths }),
    cleanup: () => request('POST', '/api/media/cleanup'),
  },

  games: {
    identify: (paths) => request('POST', '/api/games/identify', { paths }),
    iconUrl: (game) => (game.own_icon ? `/api/games/folder-icon?path=${encodeURIComponent(game.path)}` : api.titles.iconUrl(game.title_id)),
  },

  avatar: {
    whoami: () => request('GET', '/api/avatar/whoami'),
    upload: (file) => uploadRaw('/api/avatar/upload', file),
    previewUrl: (size) => `/api/avatar/preview?size=${size}&t=${Date.now()}`,
    apply: () => request('POST', '/api/avatar/apply'),
    userIconUrl: (hex) => `/api/avatar/user-icon?hex=${encodeURIComponent(hex)}`,
    rename: (name) => request('POST', '/api/avatar/rename', { name }),
  },

  saves: {
    list: () => request('GET', '/api/saves'),
    iconUrl: (userIdHex, titleId, filename) =>
      `/api/saves/icon?user_id_hex=${encodeURIComponent(userIdHex)}&title_id=${encodeURIComponent(titleId)}&filename=${encodeURIComponent(filename)}`,
    downloadZipUrl: () => '/api/saves/download-zip',
    downloadZip: (userIdHex, titleId) =>
      fetch('/api/saves/download-zip', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ user_id_hex: userIdHex, title_id: titleId }),
      }).catch((e) => {
        throw new Error(isNetworkLayerError(e) ? NETWORK_ERROR_HINT : e.message)
      }),
    getBackupConfig: () => request('GET', '/api/saves/backup-config'),
    setBackupConfig: (enabled, settleSeconds, destinations) =>
      request('POST', '/api/saves/backup-config', { enabled, settle_seconds: settleSeconds, destinations }),
    listBackups: () => request('GET', '/api/saves/backups'),
    backupNow: (titleId) => request('POST', '/api/saves/backup-now', { title_id: titleId }),
    backupAll: () => request('POST', '/api/saves/backup-all'),
    backupAllStatus: () => request('GET', '/api/saves/backup-all/status'),
    backupDownloadUrl: (path) => `/api/saves/backups/download?path=${encodeURIComponent(path)}`,
    importZip: (file, name) => uploadRaw(`/api/saves/import?name=${encodeURIComponent(name || file.name)}`, file),
    deleteSaves: (userIdHex, titleId, backupFirst = false) =>
      request('POST', '/api/saves/delete', { user_id_hex: userIdHex, title_id: titleId, backup: backupFirst ? 'true' : 'false' }),
    restoreBackup: (path, snapshotFirst = true) =>
      request('POST', '/api/saves/backups/restore', { path, snapshot_first: snapshotFirst ? 'true' : 'false' }),
    deleteBackup: (path) => request('POST', '/api/saves/backups/delete', { path }),
  },

  pkg: {
    // Both just start a background job — poll status() for progress.
    install: (opts) => request('POST', '/api/pkg/install', opts),
    // Streams straight into RAM on the console (pkgDirectUpload.js) — nothing is staged
    // on PS5 storage.
    upload: (file, onProgress, title) => directUploadPkg(file, throttleProgress(onProgress), title),
    status: () => request('GET', '/api/pkg/status'),
    info: (path) => request('GET', `/api/pkg/info?path=${encodeURIComponent(path)}`),
    // Hands the cover art the browser read to the console, so its install card can show it.
    // No image clears the previous one.
    cover: (blobUrl, note = '') => (blobUrl ? fetch(blobUrl).then((r) => r.blob()) : Promise.resolve(''))
      .then((blob) => fetch(`/api/pkg/cover?note=${encodeURIComponent(note)}`, { method: 'POST', body: blob }))
      .catch(() => {}),
    iconUrl: (path) => `/api/pkg/icon?path=${encodeURIComponent(path)}`,
    installed: (titleId) => request('GET', `/api/pkg/installed?title_id=${encodeURIComponent(titleId)}`),
    deleteDownload: () => request('POST', '/api/pkg/delete-download'),
    cancel: () => {
      cancelDirectUpload()
      return request('POST', '/api/pkg/cancel')
    },
  },

  update: {
    status: () => request('GET', '/api/update'),
    check: () => request('POST', '/api/update/check'),
    apply: () => request('POST', '/api/update/apply'),
    setAuto: (enabled) => request('POST', '/api/update/auto', { enabled }),
  },

  power: {
    run: (action) => request('POST', '/api/power', { action }),
    schedule: () => request('GET', '/api/power/schedule'),
    setSchedule: (opts) => request('POST', '/api/power/schedule', opts),
    cancelSchedule: () => request('POST', '/api/power/schedule/cancel'),
  },
  logs: {
    list: () => request('GET', '/api/logs'),
    clear: () => request('POST', '/api/logs/clear'),
  },

  klogs: {
    list: () => request('GET', '/api/klogs'),
    clear: () => request('POST', '/api/klogs/clear'),
  },

  hardware: () => request('GET', '/api/hwinfo'),
  thermal: {
    status: () => request('GET', '/api/thermal'),
    history: () => request('GET', '/api/thermal/history'),
    setFan: (tempC) => request('POST', '/api/thermal/fan', { temp_c: tempC }),
    resetFan: () => request('POST', '/api/thermal/fan/reset'),
  },

  accounts: {
    list: () => request('GET', '/api/accounts/offline-activation'),
    apply: (slot, accountIdHex) =>
      request('POST', '/api/accounts/offline-activation/apply', { slot, account_id_hex: accountIdHex }),
    signIn: (accountIdHex) =>
      request('POST', '/api/accounts/offline-activation/sign-in', { account_id_hex: accountIdHex }),
    restore: (slot) => request('POST', '/api/accounts/offline-activation/restore', { slot }),
  },

  shutdown: () => request('POST', '/api/shutdown'),
}
