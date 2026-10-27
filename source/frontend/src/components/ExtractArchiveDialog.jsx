import { useEffect, useState } from 'react'
import { Dialog } from './Dialog.jsx'
import Button from './Button.jsx'
import Checkbox from './Checkbox.jsx'
import PasswordInput from './PasswordInput.jsx'
import PathPickerModal from './PathPickerModal.jsx'
import { LockIcon } from './icons.jsx'
import { api } from '../lib/api.js'
import { useI18n } from '../lib/i18n.jsx'
import { formatFileSize } from '../lib/format.js'

function joinPath(dir, name) {
  return dir.endsWith('/') ? dir + name : `${dir}/${name}`
}

// "archive.zip" -> "archive" — same stripping the backend uses as its own
// default extraction folder, case-insensitive.
function stripZipExtension(name) {
  return name.slice(0, name.length - 4)
}

const CONFLICT_POLICIES = ['skip', 'replace', 'keep_both']

// Handles both a single archive (full stats, from /api/ftp/archive-info)
// and a batch of several — same dialog either way.
export default function ExtractArchiveDialog({ entries, sourceDir, volumes, onClose, onExtracted }) {
  const { t, locale } = useI18n()
  const single = entries.length === 1
  const [destMode, setDestMode] = useState('sibling') // sibling|current|choose
  const [chosenFolder, setChosenFolder] = useState('')
  const [pickerOpen, setPickerOpen] = useState(false)
  const [conflictPolicy, setConflictPolicy] = useState('skip')
  const [deleteSource, setDeleteSource] = useState(false)
  const [info, setInfo] = useState(null) // single-archive stats only
  const [infoError, setInfoError] = useState('')
  const [space, setSpace] = useState(null) // { ok, available }
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState('')
  const [passwordNeeded, setPasswordNeeded] = useState(false)
  const [password, setPassword] = useState('')
  const [unlocking, setUnlocking] = useState(false)
  const [passwordError, setPasswordError] = useState('')

  const archivePathOf = (entry) => joinPath(sourceDir, entry.name)
  const siblingFolderOf = (entry) => joinPath(sourceDir, stripZipExtension(entry.name))

  const destinationFor = (entry) => {
    if (destMode === 'current') return sourceDir
    if (destMode === 'choose') return chosenFolder || sourceDir
    return siblingFolderOf(entry)
  }

  useEffect(() => {
    if (!single) return
    let cancelled = false
    api.ftp.archiveInfo(archivePathOf(entries[0]))
      .then((r) => !cancelled && setInfo(r))
      .catch((e) => {
        if (cancelled) return
        if (e.code === 'password_required') setPasswordNeeded(true)
        else setInfoError(e.message)
      })
    return () => { cancelled = true }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const unlock = async () => {
    if (!password || unlocking) return
    setUnlocking(true)
    setPasswordError('')
    try {
      const r = await api.ftp.archiveInfo(archivePathOf(entries[0]), password)
      setInfo(r)
      setPasswordNeeded(false)
    } catch (e) {
      setPasswordError(e.code === 'password_required' ? t('archive.wrongPassword') : e.message)
    } finally {
      setUnlocking(false)
    }
  }

  useEffect(() => {
    if (!single || !info) return
    let cancelled = false
    const dest = destinationFor(entries[0])
    if (!dest) return undefined
    api.ftp.preflight(dest, info.uncompressed_size, true)
      .then((r) => !cancelled && setSpace({ ok: true, available: r.available }))
      .catch((e) => !cancelled && setSpace({ ok: false, available: e.available, message: e.message }))
    return () => { cancelled = true }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [single, info, destMode, chosenFolder])

  const submit = async (close) => {
    if (busy) return
    if (destMode === 'choose' && !chosenFolder) return
    if (single && passwordNeeded) return
    setBusy(true)
    setError('')
    try {
      const jobIds = []
      for (const entry of entries) {
        // A batch can't be probed one by one, so its optional password goes to every job;
        // archives that aren't encrypted just ignore it.
        const result = await api.ftp.unzip(archivePathOf(entry), destinationFor(entry), conflictPolicy, deleteSource, password || undefined)
        jobIds.push(result.job_id)
      }
      onExtracted(jobIds)
      close()
    } catch (e) {
      setError(e.message)
      setBusy(false)
    }
  }

  const destOptions = [
    { id: 'sibling', label: single ? `${stripZipExtension(entries[0].name)}/` : t('archive.extractOwnFolders') },
    { id: 'current', label: t('files.currentFolder') },
    { id: 'choose', label: chosenFolder || t('archive.chooseFolder') },
  ]

  const spaceBlocked = space && space.ok === false

  return (
    <>
      <Dialog
        title={single ? t('archive.extractTitle', { name: entries[0].name }) : t('archive.extractManyTitle', { count: entries.length })}
        onClose={onClose}
        renderFooter={(close) => (
          <>
            <Button tone="neutral" onClick={close} disabled={busy}>{t('common.cancel')}</Button>
            <Button
              tone="primary"
              sheen
              disabled={busy || (destMode === 'choose' && !chosenFolder) || spaceBlocked || (single && passwordNeeded)}
              onClick={() => submit(close)}
            >
              {busy ? t('common.working') : t('archive.extract')}
            </Button>
          </>
        )}
      >
        {() => (
          <div>
            {single && passwordNeeded && (
              <div className="mb-4 space-y-2 rounded-lg border border-warn/25 bg-warn/5 px-3 py-3">
                <div className="flex items-center gap-2 text-xs font-semibold text-warn">
                  <LockIcon size={14} /> {t('archive.passwordRequired')}
                </div>
                <div className="flex gap-1.5">
                  <PasswordInput
                    className="min-w-0 flex-1"
                    value={password}
                    onChange={(e) => setPassword(e.target.value)}
                    onKeyDown={(e) => e.key === 'Enter' && (e.preventDefault(), unlock())}
                    placeholder={t('archive.passwordPlaceholder')}
                    autoFocus
                  />
                  <Button tone="neutral" disabled={!password || unlocking} onClick={unlock}>
                    {unlocking ? t('common.working') : t('archive.unlock')}
                  </Button>
                </div>
                {passwordError && <div className="text-xs text-err">{passwordError}</div>}
              </div>
            )}
            {single && !passwordNeeded && (
              <div className="mb-4 space-y-1 rounded-lg border border-panel-border bg-bg/45 px-3 py-2.5 text-xs">
                {infoError ? (
                  <div className="text-err">{infoError}</div>
                ) : !info ? (
                  <div className="text-text-dim">{t('common.loading')}</div>
                ) : (
                  <>
                    <div className="flex items-center justify-between text-text-dim">
                      <span>{t('archive.compressedSize')}</span>
                      <span dir="ltr" className="pmgr-mono text-text">{formatFileSize(info.compressed_size, locale)}</span>
                    </div>
                    <div className="flex items-center justify-between text-text-dim">
                      <span>{t('archive.extractedSize')}</span>
                      <span dir="ltr" className="pmgr-mono text-text">{formatFileSize(info.uncompressed_size, locale)}</span>
                    </div>
                    <div className="flex items-center justify-between text-text-dim">
                      <span>{t('archive.items')}</span>
                      <span dir="ltr" className="pmgr-mono text-text">{info.items}</span>
                    </div>
                  </>
                )}
              </div>
            )}

            <div className="mb-1.5 text-xs font-bold uppercase tracking-wide text-text-dim">{t('archive.extractTo')}</div>
            <div className="mb-4 space-y-1.5" role="radiogroup" aria-label={t('archive.extractTo')}>
              {destOptions.map((opt) => (
                <button
                  key={opt.id}
                  type="button"
                  role="radio"
                  aria-checked={destMode === opt.id}
                  onClick={() => (opt.id === 'choose' ? setPickerOpen(true) : setDestMode(opt.id))}
                  className={`flex w-full items-center justify-between gap-2 rounded-lg border px-3 py-2 text-left text-sm transition-colors ${
                    destMode === opt.id ? 'border-accent/50 bg-accent/[0.08] text-accent' : 'border-panel-border text-text hover:border-accent/25'
                  }`}
                >
                  <span className="min-w-0 truncate" dir={opt.id === 'choose' && chosenFolder ? 'ltr' : undefined}>{opt.label}</span>
                </button>
              ))}
            </div>

            <div className="mb-1.5 text-xs font-bold uppercase tracking-wide text-text-dim">{t('archive.conflictPolicy')}</div>
            <div className="mb-4 grid grid-cols-3 gap-1.5">
              {CONFLICT_POLICIES.map((policy) => (
                <button
                  key={policy}
                  type="button"
                  role="radio"
                  aria-checked={conflictPolicy === policy}
                  onClick={() => setConflictPolicy(policy)}
                  className={`rounded-lg border px-2 py-2 text-center text-xs font-semibold transition-colors ${
                    conflictPolicy === policy ? 'border-accent/50 bg-accent/[0.08] text-accent' : 'border-panel-border text-text-dim hover:text-text'
                  }`}
                >
                  {t(`archive.conflict.${policy}`)}
                </button>
              ))}
            </div>

            <div className="mb-3 flex items-center gap-1.5">
              <Checkbox checked={deleteSource} onChange={setDeleteSource} label={t('archive.deleteAfterExtract')} />
              <span className="cursor-pointer text-sm text-text" onClick={() => setDeleteSource((v) => !v)}>
                {t('archive.deleteAfterExtract')}
              </span>
            </div>

            {!single && (
              <div className="mb-3 space-y-1.5">
                <div className="flex items-center gap-2 text-xs font-bold uppercase tracking-wide text-text-dim">
                  <LockIcon size={12} /> {t('archive.passwordOptional')}
                </div>
                <PasswordInput
                  value={password}
                  onChange={(e) => setPassword(e.target.value)}
                  placeholder={t('archive.passwordPlaceholder')}
                />
              </div>
            )}

            {single && space && (
              <div className={`flex items-center justify-between border-t border-panel-border pt-3 text-xs ${spaceBlocked ? 'text-err' : 'text-text-dim'}`}>
                <span>{spaceBlocked ? t('archive.notEnoughSpace') : t('archive.available', { size: formatFileSize(space.available, locale) })}</span>
              </div>
            )}
            {error && <div className="mt-2 text-xs text-err">{error}</div>}
          </div>
        )}
      </Dialog>

      {pickerOpen && (
        <PathPickerModal
          title={t('archive.chooseFolder')}
          initialPath={sourceDir}
          volumes={volumes}
          mode="folder"
          actionLabel={t('common.confirm')}
          onClose={() => setPickerOpen(false)}
          onSelect={(folder) => { setChosenFolder(folder); setDestMode('choose'); setPickerOpen(false) }}
        />
      )}
    </>
  )
}
