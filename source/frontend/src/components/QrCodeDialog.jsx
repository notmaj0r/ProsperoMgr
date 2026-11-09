import { useMemo } from 'react'
import qrcode from 'qrcode-generator'
import { Dialog } from './Dialog.jsx'
import Button from './Button.jsx'
import { CopyIcon, QrIcon } from './icons.jsx'
import { copyText } from '../lib/clipboard.js'
import { useToast } from '../lib/ToastContext.jsx'
import { useI18n } from '../lib/i18n.jsx'

const QUIET_ZONE = 3

function QrSvg({ text }) {
  const cells = useMemo(() => {
    const qr = qrcode(0, 'M')
    qr.addData(text)
    qr.make()
    const count = qr.getModuleCount()
    const dark = []
    for (let row = 0; row < count; row++) {
      for (let col = 0; col < count; col++) if (qr.isDark(row, col)) dark.push(`M${col + QUIET_ZONE} ${row + QUIET_ZONE}h1v1h-1z`)
    }
    return { size: count + QUIET_ZONE * 2, path: dark.join('') }
  }, [text])

  // Always dark-on-white, whatever the theme: scanners need the contrast.
  return (
    <svg viewBox={`0 0 ${cells.size} ${cells.size}`} className="h-full w-full" shapeRendering="crispEdges" role="img" aria-label={text}>
      <rect width={cells.size} height={cells.size} fill="#fff" />
      <path d={cells.path} fill="#111" />
    </svg>
  )
}

export default function QrCodeDialog({ url, onClose }) {
  const { t } = useI18n()
  const showToast = useToast()

  return (
    <Dialog
      onClose={onClose}
      title={t('home.qrTitle')}
      description={t('home.qrDesc')}
      icon={<QrIcon size={19} />}
      maxWidth="max-w-sm"
      renderFooter={(close) => (
        <>
          <Button tone="neutral" icon={<CopyIcon size={15} />} onClick={() => copyText(url).then(() => showToast(t('home.qrCopied')))}>
            {t('home.qrCopy')}
          </Button>
          <Button tone="primary" onClick={close}>{t('common.close')}</Button>
        </>
      )}
    >
      <div className="pmgr-badge-pop mx-auto aspect-square w-full max-w-[15rem] overflow-hidden rounded-xl border border-panel-border bg-white p-1 shadow-lg">
        <QrSvg text={url} />
      </div>
      <div className="pmgr-mono mt-3 break-all text-center text-xs text-text-dim" dir="ltr">{url}</div>
    </Dialog>
  )
}
