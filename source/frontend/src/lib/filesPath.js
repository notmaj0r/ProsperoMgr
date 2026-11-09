// The file manager reopens where it was last left; other pages use this to point it at a folder first.
export const FILES_PATH_KEY = 'pmgr-files-path'

export function setFilesPath(path) {
  try {
    window.sessionStorage.setItem(FILES_PATH_KEY, path)
  } catch { /* the file manager just opens at its last folder */ }
}

// The open folder is shown in the address (#/ftp?path=/data), so a link opens it directly.
export function filesHash(path) {
  return `/ftp?path=${encodeURIComponent(path)}`
}

export function filesPathFromHash() {
  const query = window.location.hash.split('?')[1] || ''
  const path = new URLSearchParams(query).get('path')
  return path && path.startsWith('/') ? path : null
}
