import { useEffect, useRef, useState } from 'react'
import { createPortal } from 'react-dom'
import { useI18n } from '../lib/i18n.jsx'

// dataTransfer.files lists a dropped folder as one empty entry, so walk the entries and
// tag each file with its path inside the folder (e.g. TESTE/TESTE1.txt).
const readAll = (reader) => new Promise((resolve, reject) => {
  const found = []
  const next = () => reader.readEntries((batch) => {
    if (!batch.length) resolve(found)
    else { found.push(...batch); next() }
  }, reject)
  next()
})

async function walkEntry(entry, prefix, out) {
  if (entry.isFile) {
    const file = await new Promise((resolve, reject) => entry.file(resolve, reject))
    if (prefix) Object.defineProperty(file, 'relativePath', { value: prefix + file.name })
    out.push(file)
    return
  }
  for (const child of await readAll(entry.createReader())) await walkEntry(child, `${prefix}${entry.name}/`, out)
}

async function collectDropped(dataTransfer) {
  const items = Array.from(dataTransfer.items || [])
  const entries = items.map((item) => item.webkitGetAsEntry?.()).filter(Boolean)
  if (!entries.length) return Array.from(dataTransfer.files || [])
  const out = []
  for (const entry of entries) await walkEntry(entry, '', out)
  return out
}

// Wraps content with drag-and-drop file handling. Used by FtpView (upload
// to current path) and PayloadsView (import a .elf) — same drop UX both places.
export default function DropZone({ onFiles, children, className = '', global = false, enabled = true, label }) {
  const { t } = useI18n()
  const effectiveLabel = label ?? t('files.dropDefault')
  const [active, setActive] = useState(false)
  const dragDepth = useRef(0)
  const onFilesRef = useRef(onFiles)
  onFilesRef.current = onFiles

  const hasFiles = (event) =>
    Array.from(event.dataTransfer?.types || []).includes('Files') || (event.dataTransfer?.files?.length || 0) > 0

  useEffect(() => {
    if (!global || !enabled) return undefined

    const enter = (event) => {
      if (!hasFiles(event)) return
      event.preventDefault()
      dragDepth.current += 1
      setActive(true)
    }
    const over = (event) => {
      if (!hasFiles(event)) return
      event.preventDefault()
      event.dataTransfer.dropEffect = 'copy'
      setActive(true)
    }
    const leave = (event) => {
      dragDepth.current = Math.max(0, dragDepth.current - 1)
      if (dragDepth.current === 0) setActive(false)
    }
    const drop = (event) => {
      dragDepth.current = 0
      setActive(false)
      if (!hasFiles(event)) return
      event.preventDefault()
      collectDropped(event.dataTransfer).then((files) => {
        if (files.length) onFilesRef.current(files)
      })
    }
    // Close in capture phase; the bubble handler is the only upload target, so a
    // row under the cursor can't change the destination.
    const resetDropUi = () => { dragDepth.current = 0; setActive(false) }

    window.addEventListener('dragenter', enter)
    window.addEventListener('dragover', over)
    window.addEventListener('dragleave', leave)
    window.addEventListener('drop', drop)
    window.addEventListener('drop', resetDropUi, true)
    window.addEventListener('dragend', resetDropUi)
    window.addEventListener('blur', resetDropUi)
    return () => {
      window.removeEventListener('dragenter', enter)
      window.removeEventListener('dragover', over)
      window.removeEventListener('dragleave', leave)
      window.removeEventListener('drop', drop)
      window.removeEventListener('drop', resetDropUi, true)
      window.removeEventListener('dragend', resetDropUi)
      window.removeEventListener('blur', resetDropUi)
    }
  }, [global, enabled])

  const onDrop = (e) => {
    e.preventDefault()
    dragDepth.current = 0
    setActive(false)
    collectDropped(e.dataTransfer).then((files) => {
      if (files.length) onFiles(files)
    })
  }

  return (
    <div
      onDragOver={global ? undefined : (e) => {
        e.preventDefault()
        setActive(true)
      }}
      onDragLeave={global ? undefined : () => setActive(false)}
      onDrop={global ? undefined : onDrop}
      className={`relative ${className}`}
    >
      {children}
      {active && !global && (
        <div className="pointer-events-none absolute bottom-0 left-0 right-0 top-0 z-10 flex items-center justify-center rounded-lg border-2 border-dashed border-accent bg-accent/10">
          <span className="text-sm font-medium text-accent">{effectiveLabel}</span>
        </div>
      )}
      {active && global && createPortal(
        <div className="pmgr-global-drop pointer-events-none fixed top-0 right-0 bottom-0 left-0 z-[100] grid place-items-center p-5">
          <div className="pmgr-global-drop-card flex max-w-lg flex-col items-center text-center">
            <div className="pmgr-global-drop-icon mb-4 grid h-16 w-16 place-items-center rounded-2xl">
              <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round">
                <path d="M12 16V4M7 9l5-5 5 5" />
                <path d="M4 16v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3" />
              </svg>
            </div>
            <div className="text-lg font-black text-text">{effectiveLabel}</div>
            <div className="mt-1 text-sm text-text-dim">{t('files.releaseAnywhere')}</div>
          </div>
        </div>,
        document.body,
      )}
    </div>
  )
}
