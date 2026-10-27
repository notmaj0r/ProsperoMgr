import { useState } from 'react'
import { EyeIcon, EyeOffIcon } from './icons.jsx'
import { useI18n } from '../lib/i18n.jsx'

// A password field with a show/hide toggle. Everything but `className` (which sizes the
// wrapper, so it can sit in a flex row) goes straight to the <input>.
export default function PasswordInput({ className = '', ...inputProps }) {
  const { t } = useI18n()
  const [visible, setVisible] = useState(false)
  return (
    <div className={`relative ${className}`}>
      <input
        {...inputProps}
        type={visible ? 'text' : 'password'}
        dir="ltr"
        className="w-full rounded-lg border border-panel-border bg-bg py-2 pl-3 pr-10 text-sm text-text outline-none focus:border-accent"
      />
      <button
        type="button"
        onClick={() => setVisible((v) => !v)}
        aria-label={visible ? t('archive.hidePassword') : t('archive.showPassword')}
        aria-pressed={visible}
        title={visible ? t('archive.hidePassword') : t('archive.showPassword')}
        className="absolute inset-y-0 right-0 grid w-9 place-items-center rounded-r-lg text-text-dim transition-colors hover:text-text focus-visible:text-accent"
      >
        {visible ? <EyeOffIcon size={16} /> : <EyeIcon size={16} />}
      </button>
    </div>
  )
}
