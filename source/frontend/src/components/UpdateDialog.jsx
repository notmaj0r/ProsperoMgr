import { Dialog } from './Dialog.jsx'
import Button from './Button.jsx'
import TransferProgress, { formatSize } from './TransferProgress.jsx'
import { DownloadIcon } from './icons.jsx'
import { api } from '../lib/api.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'

export default function UpdateDialog({ status, refresh, onClose }) {
  const { t, locale } = useI18n()
  const showToast = useToast()
  const downloading = status.state === 'downloading'
  const restarting = status.state === 'launching'
  const busy = downloading || restarting
  const pct = status.total > 0 ? Math.min(100, (status.loaded / status.total) * 100) : 0

  const apply = () => api.update.apply().then(refresh).catch((e) => showToast(e.message, 'err'))

  return (
    <Dialog
      onClose={onClose}
      title={t('update.title', { version: status.latest })}
      description={t('update.desc', { current: status.current })}
      icon={<DownloadIcon size={19} />}
      maxWidth="max-w-lg"
      renderFooter={(close) => (
        <>
          <Button tone="neutral" onClick={close} disabled={busy}>{t('update.later')}</Button>
          <Button tone="primary" sheen icon={<DownloadIcon size={15} />} onClick={apply} disabled={busy}>
            {busy ? t('update.updating') : t('update.now')}
          </Button>
        </>
      )}
    >
      <div className="space-y-3">
        {busy && (
          <TransferProgress
            label={restarting ? t('update.restarting') : t('update.downloading')}
            pct={pct}
            indeterminate={restarting || status.total <= 0}
            sub={downloading && status.total > 0 ? `${formatSize(status.loaded, locale)} / ${formatSize(status.total, locale)}` : undefined}
          />
        )}
        {status.error && <div className="rounded-lg border border-err/25 bg-err/10 px-3 py-2 text-xs text-err">{status.error}</div>}
        {status.notes && (
          <div>
            <div className="mb-1.5 text-xs font-bold uppercase tracking-wide text-text-dim">{t('update.notes')}</div>
            <div className="max-h-56 overflow-y-auto whitespace-pre-wrap rounded-lg border border-panel-border bg-bg/45 px-3 py-2 text-xs leading-relaxed text-text-dim">
              {status.notes}
            </div>
          </div>
        )}
      </div>
    </Dialog>
  )
}
