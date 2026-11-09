import { useState } from 'react'
import UpdateDialog from './UpdateDialog.jsx'
import { DownloadIcon } from './icons.jsx'
import { useUpdate } from '../lib/useUpdate.js'
import { useI18n } from '../lib/i18n.jsx'

// Only shows up when there is something to act on, so the header stays quiet otherwise.
export default function UpdateBadge() {
  const { t } = useI18n()
  const { status, refresh } = useUpdate()
  const [open, setOpen] = useState(false)
  const updating = status && (status.state === 'downloading' || status.state === 'launching')
  if (!status || (!status.available && !updating)) return null

  return (
    <>
      <button
        type="button"
        onClick={() => setOpen(true)}
        title={t('update.title', { version: status.latest })}
        className="pmgr-icon-btn pmgr-btn-sheen inline-flex h-8 shrink-0 items-center gap-1.5 rounded-full border border-accent/50 bg-accent/10 px-2.5 text-xs font-bold text-accent hover:bg-accent/20"
      >
        {updating ? <span className="pmgr-live-dot text-accent" /> : <DownloadIcon size={14} />}
        <span className="hidden sm:inline">{updating ? t('update.updating') : t('update.badge', { version: status.latest })}</span>
      </button>
      {open && <UpdateDialog status={status} refresh={refresh} onClose={() => setOpen(false)} />}
    </>
  )
}
