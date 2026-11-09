import { useState } from 'react'
import { Dialog } from './Dialog.jsx'
import Button from './Button.jsx'
import { AlertTriangleIcon } from './icons.jsx'

// Replace / skip / keep-both choice shared by file transfers and payload imports.
export default function TransferConflictDialog({ conflict, onClose, onResolve, t }) {
  const [policy, setPolicy] = useState('keep_both')
  const policies = [
    { id: 'keep_both', label: t('archive.conflict.keep_both') },
    { id: 'skip', label: t('archive.conflict.skip') },
    { id: 'replace', label: t('files.replaceMerge') },
  ]

  return (
    <Dialog
      title={t('files.transferConflictTitle')}
      description={t('files.transferConflictDesc', { count: conflict.conflicts.length })}
      onClose={onClose}
      tone="warn"
      icon={<AlertTriangleIcon size={19} />}
      renderFooter={(close) => (
        <>
          <Button tone="neutral" onClick={close}>{t('common.cancel')}</Button>
          <Button tone={policy === 'replace' ? 'warn' : 'primary'} onClick={() => { close(); onResolve(policy) }}>
            {t('common.confirm')}
          </Button>
        </>
      )}
    >
      <div className="space-y-3">
        <div className="pmgr-mono max-h-28 overflow-y-auto whitespace-pre-wrap rounded-lg border border-panel-border bg-bg/45 px-3 py-2 text-xs text-text-dim">
          {conflict.conflicts.map((entry) => entry.name).join('\n')}
        </div>
        <div role="radiogroup" className="grid gap-2">
          {policies.map((option) => {
            const selected = policy === option.id
            return (
              <label
                key={option.id}
                className={`flex min-h-11 w-full cursor-pointer items-center gap-3 rounded-lg border px-3 py-2.5 text-left text-sm font-semibold transition-[border-color,background-color,color] ${
                  selected ? 'border-accent/55 bg-accent/[0.08] text-text' : 'border-panel-border bg-bg/25 text-text-dim hover:border-accent/30 hover:text-text'
                }`}
              >
                <input
                  type="radio"
                  name="transfer-conflict-policy"
                  value={option.id}
                  checked={selected}
                  onChange={() => setPolicy(option.id)}
                  className="peer sr-only"
                />
                <span className={`grid h-4 w-4 shrink-0 place-items-center rounded-full border peer-focus-visible:ring-2 peer-focus-visible:ring-accent/50 peer-focus-visible:ring-offset-2 peer-focus-visible:ring-offset-bg ${selected ? 'border-accent' : 'border-text-dim/60'}`}>
                  {selected && <span className="h-2 w-2 rounded-full bg-accent" />}
                </span>
                <span>{option.label}</span>
              </label>
            )
          })}
        </div>
      </div>
    </Dialog>
  )
}
