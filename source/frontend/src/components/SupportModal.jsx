import { useState } from 'react'
import EmojiText from './EmojiText.jsx'
import { useToast } from '../lib/ToastContext.jsx'
import { useDismiss } from '../lib/useDismiss.js'
import { copyText } from '../lib/clipboard.js'
import { useI18n } from '../lib/i18n.jsx'
import { useDialogAccessibility } from '../lib/useDialogAccessibility.js'
import {
  ArrowLeftIcon, BitcoinIcon, ChevronRightIcon, CloseIcon, CoffeeIcon,
  HeartIcon, PixIcon,
} from './icons.jsx'

const CRYPTO = [
  { label: 'BTC', addr: 'bc1ppfcad9ytyzmmzq3nfatfcag7q38rqcpsvxfqusq0uzv4gh82pa4syeknlk' },
  { label: 'BCH (Bitcoin Cash)', addr: 'qz2h97c2pyexn3fwhveu33tn5xkd3qlfn5796zhtcq' },
  { label: 'USDC (Solana)', addr: 'BNpezXSQWuUR9HWGjzQUS7AeEBCwiY2ZGxArWNwYXKKV' },
  { label: 'USDT (TON)', addr: 'UQA18f4cVS0778fL_nIssdN1GIHRYR7dQ3UnNgjPwgYvIQi_' },
]

const SUPPORTERS = ['Vinas', 'Someone', 'Bruno Sanglard', 'AirKingBR', 'Marcus', 'Jeffs', 'cavalcanti27', 'Maffioh', 'PCYGOR', 'Lucas Clinio Maraia', 'primo', 'lal3x', 'jawad', 'anonymous', 'X-F1REBALL-X', 'Br0ken4life', 'Nimrod', 'I gave a tip!', 'johnfugate', 'Loxias', 'Shambhala222', 'Zahir Tarafder', 'Wender', 'Heymimd', 'Android_Loads', 'LOCATELLI', 'Tiago G DE LIRA', 'Giih77777', 'mvlira', 'PauloSantos', 'Waleed', 'Doubleq', 'gas17', 'PS5FOREVA', 'thiagodalsoto', 'Thiago_Dark', 'Jeffeerson REC']

export default function SupportModal({ onClose }) {
  const { t } = useI18n()
  const [view, setView] = useState('main')
  const showToast = useToast()
  const { closing, close } = useDismiss(onClose)
  const dialogRef = useDialogAccessibility(close)

  const copy = (addr, label) => {
    copyText(addr).then(
      () => showToast(t('support.addressCopied', { label })),
      () => showToast(t('support.copyFailed'), 'err'),
    )
  }

  return (
    <div className={`pmgr-modal-overlay ${closing ? 'is-closing' : ''}`} onClick={close}>
      <div
        ref={dialogRef}
        role="dialog"
        aria-modal="true"
        aria-label={t(view === 'main' ? 'support.title' : 'support.cryptoTitle')}
        tabIndex={-1}
        className={`pmgr-support-modal pmgr-panel pmgr-modal-pop pmgr-max-h-dvh-85 w-full max-w-lg overflow-y-auto rounded-2xl border border-panel-border ${closing ? 'is-closing' : ''}`}
        onClick={(e) => e.stopPropagation()}
      >
        {view === 'main' ? (
          <>
            <div className="pmgr-support-hero relative overflow-hidden px-6 pb-5 pt-6 text-center">
              <button type="button" onClick={close} aria-label={t('common.close')} className="pmgr-support-close pmgr-icon-btn !absolute right-4 top-4 grid h-9 w-9 place-items-center rounded-full text-text-dim hover:text-text">
                <CloseIcon size={16} />
              </button>
              <div className="pmgr-support-heart mx-auto mb-3 grid h-14 w-14 place-items-center rounded-2xl text-white">
                <HeartIcon size={26} />
              </div>
              <h2 className="text-xl font-black tracking-tight text-text">{t('support.title')}</h2>
              <p className="mx-auto mt-2 max-w-sm text-sm leading-relaxed text-text-dim">
                <EmojiText text={t('support.desc')} />
              </p>
              <p className="mx-auto mt-2 max-w-sm text-xs leading-relaxed text-text-dim">
                {t('support.goal')}
              </p>
            </div>
            <div className="grid gap-2.5 px-5 pb-5 sm:grid-cols-2">
              <a
                href="https://ko-fi.com/maj0r"
                target="_blank"
                rel="noopener noreferrer"
                className="pmgr-support-option pmgr-support-kofi"
              >
                <span className="pmgr-support-option-icon"><CoffeeIcon size={20} /></span>
                <span><strong>{t('support.kofi')}</strong></span>
                <ChevronRightIcon size={16} />
              </a>
              <a
                href="https://buymeacoffee.com/maj0r"
                target="_blank"
                rel="noopener noreferrer"
                className="pmgr-support-option pmgr-support-coffee"
              >
                <span className="pmgr-support-option-icon"><CoffeeIcon size={20} /></span>
                <span><strong>{t('support.coffee')}</strong></span>
                <ChevronRightIcon size={16} />
              </a>
              <button
                type="button"
                onClick={() => setView('crypto')}
                className="pmgr-support-option pmgr-support-crypto"
              >
                <span className="pmgr-support-option-icon"><BitcoinIcon size={20} /></span>
                <span><strong>{t('support.crypto')}</strong><small>{t('support.cryptoDesc')}</small></span>
                <ChevronRightIcon size={16} />
              </button>
              <a
                href="https://livepix.gg/notmaj0r"
                target="_blank"
                rel="noopener noreferrer"
                className="pmgr-support-option pmgr-support-pix"
              >
                <span className="pmgr-support-option-icon"><PixIcon size={20} /></span>
                <span><strong>{t('support.pix')}</strong><small>{t('support.pixDesc')}</small></span>
                <ChevronRightIcon size={16} />
              </a>
            </div>
            <div className="mx-5 border-t border-panel-border px-1 pb-5 pt-4 text-center">
              <div className="mb-2.5 text-xs font-bold uppercase tracking-[0.14em] text-text-dim">
                {t('settings.supporters')}
              </div>
              <div className="flex flex-wrap items-center justify-center gap-x-5 gap-y-2">
                {SUPPORTERS.map((name, index) => (
                  <span key={name} className="pmgr-supporter-name font-mono text-sm" style={{ animationDelay: `${-(index * 3) / SUPPORTERS.length}s` }}>{name}</span>
                ))}
              </div>
            </div>
          </>
        ) : (
          <>
            <div className="flex items-center gap-3 border-b border-panel-border px-5 py-4">
              <button type="button" onClick={() => setView('main')} className="pmgr-icon-btn grid h-9 w-9 place-items-center rounded-full border border-panel-border bg-panel-2 text-text-dim hover:text-text">
                <ArrowLeftIcon size={17} />
              </button>
              <div>
                <h2 className="flex items-center gap-2 text-lg font-bold text-text"><BitcoinIcon size={19} /> {t('support.cryptoTitle')}</h2>
                <p className="text-xs text-text-dim">{t('support.cryptoHint')}</p>
              </div>
              <button type="button" onClick={close} aria-label={t('common.close')} className="pmgr-icon-btn ml-auto grid h-9 w-9 place-items-center rounded-full text-text-dim hover:text-text"><CloseIcon size={16} /></button>
            </div>
            <div className="space-y-2 p-5">
              {CRYPTO.map((c) => (
                <button
                  type="button"
                  key={c.label}
                  onClick={() => copy(c.addr, c.label)}
                  className="pmgr-crypto-address w-full rounded-xl border border-panel-border bg-bg/45 px-3.5 py-3 text-left"
                >
                  <div className="mb-1 flex items-center justify-between text-xs font-bold text-accent"><span>{c.label}</span><span className="text-xs uppercase tracking-wide text-text-dim">{t('support.copyAddress')}</span></div>
                  <div className="break-all font-mono text-xs leading-relaxed text-text-dim" dir="ltr">{c.addr}</div>
                </button>
              ))}
            </div>
          </>
        )}
      </div>
    </div>
  )
}
