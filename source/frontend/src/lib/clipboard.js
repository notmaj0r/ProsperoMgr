// Clipboard.writeText is restricted to secure contexts in several browsers, including the
// PS5 browser. Keep the modern path, then fall back to the selection API for plain HTTP origins.
import { translate } from './i18n.jsx'

export async function copyText(text) {
  if (navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text)
      return
    } catch {
      // Permission denied / insecure origin — use the compatibility path.
    }
  }

  const field = document.createElement('textarea')
  field.value = text
  field.setAttribute('readonly', '')
  field.style.position = 'fixed'
  field.style.left = '-9999px'
  field.style.opacity = '0'
  document.body.appendChild(field)
  field.focus()
  field.select()
  field.setSelectionRange(0, field.value.length)

  let copied = false
  try {
    copied = document.execCommand('copy')
  } finally {
    document.body.removeChild(field)
  }
  if (!copied) throw new Error(translate('errors.clipboardRejected'))
}
