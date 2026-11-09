import { useEffect, useRef, useState } from 'react'
import Button from './Button.jsx'
import Pill from './Pill.jsx'
import Toggle from './Toggle.jsx'
import CheckOption from './CheckOption.jsx'
import { api } from '../lib/api.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'

const INPUT_CLASS =
  'min-h-9 w-full rounded-md border border-panel-border bg-bg px-2 py-1.5 text-sm outline-none focus:border-accent pmgr-mono'

// Google Drive as an extra backup destination. Sign-in is Google's "device" flow: the console shows
// a code, the user enters it on google.com/device from any phone or PC, and the console polls
// until Google answers. The OAuth client id/secret are the user's own (Google Cloud console), so
// nothing is baked into the ELF. The secret is write-only here: the backend never returns it.
export default function GdriveDestination({ onChanged }) {
  const { t } = useI18n()
  const showToast = useToast()
  const [status, setStatus] = useState(null)
  const [clientId, setClientId] = useState('')
  const [clientSecret, setClientSecret] = useState('')
  const [busy, setBusy] = useState(false)
  const pollRef = useRef(null)
  const previousPhaseRef = useRef(null) // null until the first status arrives

  const apply = (next) => {
    setStatus(next)
    setClientId((current) => current || next.client_id || '')
    // Report only real transitions, so the parent reloads the Drive list once per change. The first
    // load just records where things stand: a sign-in finished earlier is not news.
    if (previousPhaseRef.current !== next.link_phase) {
      if (next.link_phase === 'linked' && previousPhaseRef.current !== null) {
        showToast(t('saves.gdriveLinkedToast'))
        onChanged?.()
      }
      previousPhaseRef.current = next.link_phase
    }
  }

  const refresh = () => api.gdrive.status().then(apply).catch((e) => showToast(e.message, 'err'))

  useEffect(() => {
    refresh()
    return () => clearInterval(pollRef.current)
  }, [])

  // Only while a code is on screen: the console is the one waiting on Google, the page just mirrors it.
  const waiting = status?.link_phase === 'waiting'
  useEffect(() => {
    clearInterval(pollRef.current)
    if (!waiting) return undefined
    pollRef.current = setInterval(() => {
      api.gdrive.status().then(apply).catch(() => {})
    }, 3000)
    return () => clearInterval(pollRef.current)
  }, [waiting])

  const run = (promise, doneKey) => {
    setBusy(true)
    return promise
      .then((next) => {
        apply(next)
        if (doneKey) showToast(t(doneKey))
      })
      .catch((e) => showToast(e.message, 'err'))
      .finally(() => setBusy(false))
  }

  if (!status) return null

  const saveCredentials = () =>
    run(api.gdrive.setConfig(clientId.trim(), clientSecret.trim(), status.enabled, status.drive_only), 'saves.gdriveSavedToast').then(
      () => setClientSecret(''),
    )
  const toggle = (checked) =>
    run(api.gdrive.setConfig(status.client_id, '', checked, status.drive_only)).then(() => onChanged?.())
  const toggleDriveOnly = (checked) =>
    run(api.gdrive.setConfig(status.client_id, '', status.enabled, checked)).then(() => onChanged?.())
  const unlink = () => run(api.gdrive.unlink(), 'saves.gdriveUnlinkedToast').then(() => onChanged?.())

  const pill = status.linked
    ? { tone: status.enabled ? 'ok' : 'neutral', text: t('saves.gdriveStateLinked') }
    : waiting
      ? { tone: 'neutral', text: t('saves.gdriveStateWaiting') }
      : { tone: 'neutral', text: t('saves.gdriveStateNotLinked') }

  return (
    <div className="mt-3 rounded-lg border border-panel-border px-3 py-3">
      <div className="mb-2 flex items-center justify-between">
        <div className="text-sm font-medium text-text">{t('saves.gdriveTitle')}</div>
        <Pill tone={pill.tone}>{pill.text}</Pill>
      </div>

      {status.linked ? (
        <div>
          <div className="mb-2 flex items-center">
            <Toggle checked={status.enabled} onChange={toggle} label={t('saves.gdriveEnabledLabel')} />
            <p className="ml-3 min-w-0 text-sm text-text-dim">{t('saves.gdriveDesc', { folder: status.folder_name })}</p>
          </div>
          {status.enabled && (
            <div className="mb-3">
              <CheckOption
                checked={status.drive_only}
                onChange={toggleDriveOnly}
                label={t('saves.gdriveOnlyLabel')}
                hint={t('saves.gdriveOnlyHint')}
              />
            </div>
          )}
          <p className="mb-2 text-xs text-text-dim">{t('saves.gdriveScopeNote')}</p>
          <Button size="sm" tone="neutral" disabled={busy} onClick={unlink}>
            {t('saves.gdriveUnlink')}
          </Button>
        </div>
      ) : waiting ? (
        <div aria-live="polite">
          <p className="mb-2 text-sm text-text-dim">{t('saves.gdriveLinkInstructions')}</p>
          <div className="mb-1 break-all text-sm text-text pmgr-mono" dir="ltr">{status.verification_url}</div>
          <div className="mb-3 text-2xl font-semibold tracking-widest text-text pmgr-mono" dir="ltr">{status.user_code}</div>
          <Button size="sm" tone="neutral" disabled={busy} onClick={() => run(api.gdrive.linkCancel())}>
            {t('saves.gdriveCancelLink')}
          </Button>
        </div>
      ) : (
        <div>
          <p className="mb-3 text-xs text-text-dim">{t('saves.gdriveSetupHelp')}</p>
          {status.link_phase === 'error' && status.link_error && (
            <div className="mb-3 rounded-lg border border-err/20 bg-err/10 px-3 py-2 text-sm text-err">{status.link_error}</div>
          )}
          <label className="mb-2 block text-sm text-text">
            <span className="mb-1 block">{t('saves.gdriveClientId')}</span>
            <input
              className={INPUT_CLASS}
              dir="ltr"
              autoComplete="off"
              spellCheck={false}
              value={clientId}
              onChange={(e) => setClientId(e.target.value)}
            />
          </label>
          <label className="mb-3 block text-sm text-text">
            <span className="mb-1 block">{t('saves.gdriveClientSecret')}</span>
            <input
              className={INPUT_CLASS}
              dir="ltr"
              type="password"
              autoComplete="off"
              spellCheck={false}
              placeholder={status.configured ? t('saves.gdriveSecretSaved') : ''}
              value={clientSecret}
              onChange={(e) => setClientSecret(e.target.value)}
            />
          </label>
          <div className="flex flex-wrap items-center gap-2">
            <Button size="sm" tone="neutral" disabled={busy || !clientId.trim()} onClick={saveCredentials}>
              {t('saves.gdriveSaveCredentials')}
            </Button>
            <Button size="sm" tone="primary" disabled={busy || !status.configured} onClick={() => run(api.gdrive.linkStart())}>
              {t('saves.gdriveLink')}
            </Button>
          </div>
        </div>
      )}
    </div>
  )
}
