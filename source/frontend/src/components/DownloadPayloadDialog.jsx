import { useEffect, useState } from 'react'
import { Dialog } from './Dialog.jsx'
import Button from './Button.jsx'
import { CheckCircleIcon } from './icons.jsx'
import { api } from '../lib/api.js'
import { useI18n } from '../lib/i18n.jsx'
import { formatFileSize } from '../lib/format.js'
import { SkeletonRows } from './Skeleton.jsx'

// Lets the user pick a specific release instead of always grabbing latest —
// same idea as picking a version before installing a payload elsewhere.
export default function DownloadPayloadDialog({ entry, onClose, onDownloaded }) {
  const { t, locale } = useI18n()
  const [releases, setReleases] = useState(null)
  const [error, setError] = useState('')
  const [selectedTag, setSelectedTag] = useState('')
  const [busy, setBusy] = useState(false)

  useEffect(() => {
    let cancelled = false
    api.catalog.releases(entry.url)
      .then((r) => {
        if (cancelled) return
        const list = r.releases || []
        setReleases(list)
        const firstInstallable = list.find((rel) => !rel.zipOnly) || list[0]
        if (firstInstallable) setSelectedTag(firstInstallable.tag)
      })
      .catch((e) => !cancelled && setError(e.message))
    return () => { cancelled = true }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [entry.url])

  const selected = releases?.find((r) => r.tag === selectedTag)

  const submit = async (close) => {
    if (!selected || selected.zipOnly || busy) return
    setBusy(true)
    setError('')
    try {
      await api.payloads.download(selected.assetUrl, selected.tag, false, entry.url)
      onDownloaded()
      close()
    } catch (e) {
      setError(e.message)
      setBusy(false)
    }
  }

  return (
    <Dialog
      title={t('repositories.downloadTitle', { name: entry.name })}
      description={t('repositories.downloadDesc')}
      onClose={onClose}
      renderFooter={(close) => (
        <>
          <Button tone="neutral" onClick={close} disabled={busy}>{t('common.cancel')}</Button>
          <Button tone="primary" sheen disabled={busy || !selected || selected.zipOnly} onClick={() => submit(close)}>
            {busy ? t('common.working') : t('common.download')}
          </Button>
        </>
      )}
    >
      {() => (
        <div>
          {error && <div className="mb-3 rounded-lg border border-err/25 bg-err/10 px-3 py-2 text-xs text-err">{error}</div>}
          {!releases ? (
            <SkeletonRows count={3} />
          ) : releases.length === 0 ? (
            <div className="grid h-24 place-items-center text-center text-sm text-text-dim">{t('repositories.noReleases')}</div>
          ) : (
            <div className="max-h-72 space-y-1.5 overflow-y-auto" role="radiogroup" aria-label={t('repositories.chooseVersion')}>
              {releases.map((release) => {
                const isSelected = selectedTag === release.tag
                return (
                  <button
                    key={release.tag}
                    type="button"
                    role="radio"
                    aria-checked={isSelected}
                    disabled={release.zipOnly}
                    onClick={() => setSelectedTag(release.tag)}
                    className={`flex w-full items-start gap-2.5 rounded-lg border px-3 py-2.5 text-left transition-colors ${
                      release.zipOnly
                        ? 'cursor-not-allowed border-panel-border opacity-50'
                        : isSelected
                          ? 'border-accent/50 bg-accent/[0.08]'
                          : 'border-panel-border hover:border-accent/25'
                    }`}
                  >
                    <span className={`mt-0.5 shrink-0 ${isSelected && !release.zipOnly ? 'text-accent' : 'text-transparent'}`}>
                      <CheckCircleIcon size={16} />
                    </span>
                    <span className="min-w-0 flex-1">
                      <span className="flex items-center gap-2">
                        <span className={`truncate font-mono text-sm font-semibold ${isSelected && !release.zipOnly ? 'text-accent' : 'text-text'}`} dir="ltr">
                          {release.tag}
                        </span>
                        {release.prerelease && (
                          <span className="shrink-0 rounded-md border border-warn/30 bg-warn/10 px-1.5 py-0.5 text-[10px] font-bold uppercase tracking-wide text-warn">
                            {t('repositories.prerelease')}
                          </span>
                        )}
                        {release.date && <span dir="ltr" className="shrink-0 text-xs text-text-dim">{release.date}</span>}
                      </span>
                      <span className="mt-0.5 block truncate text-xs text-text-dim" dir="ltr">
                        {release.asset} · {formatFileSize(release.assetSize, locale)}
                      </span>
                      {release.zipOnly && <span className="mt-0.5 block text-xs text-warn">{t('repositories.zipOnlyNote')}</span>}
                    </span>
                  </button>
                )
              })}
            </div>
          )}
        </div>
      )}
    </Dialog>
  )
}
