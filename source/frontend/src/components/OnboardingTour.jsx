import { useState } from 'react'
import { ICONS } from './NavRail.jsx'
import { CheckIcon, CloseIcon } from './icons.jsx'
import { useDismiss } from '../lib/useDismiss.js'
import { useI18n } from '../lib/i18n.jsx'
import { useDialogAccessibility } from '../lib/useDialogAccessibility.js'

// Four steps, not a page-by-page tour — enough to get someone started.
// Everything else is explained where it's actually used, not up front.
const STEPS = [
  { id: 'home', titleKey: 'onboarding.step1Title', descKey: 'onboarding.step1Desc', tipKeys: ['onboarding.step1Tip'], nav: false },
  { id: 'ftp', titleKey: 'onboarding.step2Title', descKey: 'onboarding.step2Desc', tipKeys: ['onboarding.step2Tip'] },
  { id: 'payloads', titleKey: 'onboarding.step3Title', descKey: 'onboarding.step3Desc', tipKeys: ['onboarding.step3Tip'] },
  { id: 'pkg', titleKey: 'onboarding.stepPkgTitle', descKey: 'onboarding.stepPkgDesc', tipKeys: ['onboarding.stepPkgTip'] },
  { id: 'settings', titleKey: 'onboarding.step4Title', descKey: 'onboarding.step4Desc', tipKeys: [], navLabelKey: 'onboarding.step4Nav' },
]

export default function OnboardingTour({ onClose, onNavigate }) {
  const { t } = useI18n()
  const [step, setStep] = useState(0)
  const s = STEPS[step]
  const last = step === STEPS.length - 1
  const { closing, close } = useDismiss(onClose)
  const dialogRef = useDialogAccessibility(close)

  const openPage = () => {
    onNavigate(s.id)
    close()
  }

  return (
    <div className={`pmgr-modal-overlay ${closing ? 'is-closing' : ''}`} onClick={close}>
      <div
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-labelledby="pmgr-onboarding-title"
        tabIndex={-1}
        className={`pmgr-panel pmgr-modal-pop flex w-full max-w-3xl overflow-hidden rounded-xl border border-panel-border ${closing ? 'is-closing' : ''}`}
        onClick={(e) => e.stopPropagation()}
      >
        <div className="flex w-16 flex-col items-center gap-1 border-r border-panel-border bg-panel/60 py-6 sm:w-20">
          {STEPS.map((st, i) => (
            <div key={st.id} className="flex flex-col items-center">
              <button
                onClick={() => setStep(i)}
                aria-label={t(st.titleKey)}
                aria-current={i === step ? 'step' : undefined}
                className={`grid h-9 w-9 place-items-center rounded-full text-xs font-bold transition-[color,background-color,box-shadow] ${
                  i === step
                    ? 'bg-accent text-white shadow-[0_0_16px_-2px_var(--color-accent)]'
                    : i < step
                      ? 'bg-accent/20 text-accent'
                      : 'bg-panel-border text-text-dim'
                }`}
              >
                {i < step ? <CheckIcon size={14} /> : i + 1}
              </button>
              {i < STEPS.length - 1 && <span className={`h-6 w-px ${i < step ? 'bg-accent/50' : 'bg-panel-border'}`} />}
            </div>
          ))}
        </div>

        <div className="flex flex-1 flex-col p-6">
          <button
            onClick={close}
            aria-label={t('common.close')}
            className="ml-auto mb-2 grid h-7 w-7 place-items-center rounded-full bg-panel-border text-xs text-text-dim hover:text-text"
          >
            <CloseIcon size={12} />
          </button>

          <div className="mb-4 flex items-center gap-3">
            <div className="grid h-11 w-11 shrink-0 place-items-center rounded-lg border border-accent/30 bg-accent/10 text-accent">
              {ICONS[s.id]}
            </div>
            <h2 id="pmgr-onboarding-title" className="text-xl font-bold text-text">{t(s.titleKey)}</h2>
          </div>

          <p className="mb-4 text-sm text-text-dim">{t(s.descKey)}</p>

          {s.tipKeys.length > 0 && (
            <ul className="mb-6 space-y-1.5 text-sm text-text">
              {s.tipKeys.map((tipKey) => (
                <li key={tipKey} className="flex gap-2">
                  <span className="text-accent">→</span>
                  <span>{t(tipKey)}</span>
                </li>
              ))}
            </ul>
          )}

          {s.nav !== false && (
            <button
              onClick={openPage}
              className="mb-6 self-start rounded-md bg-accent px-4 py-1.5 text-xs font-medium text-white hover:bg-accent-hover"
            >
              {t(s.navLabelKey || 'onboarding.takeMeThere')}
            </button>
          )}

          <div className="mt-auto flex items-center justify-between border-t border-panel-border pt-4">
            <button onClick={close} className="text-xs text-text-dim hover:text-text">
              {t('onboarding.skip')}
            </button>
            <div className="flex gap-2">
              <button
                onClick={() => setStep(step - 1)}
                disabled={step === 0}
                className="rounded-md bg-panel-border px-3 py-1.5 text-xs font-medium text-text-dim disabled:opacity-40"
              >
                {t('onboarding.back')}
              </button>
              <button
                onClick={() => (last ? close() : setStep(step + 1))}
                className="rounded-md bg-accent px-4 py-1.5 text-xs font-medium text-white hover:bg-accent-hover"
              >
                {last ? t('onboarding.done') : t('onboarding.next')}
              </button>
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
