// Locale-aware date/number formatting — small wrappers around Intl so callers don't reach
// for the browser's default locale, which may not match what the user picked in Prospero.

export function formatDate(value, locale, options = { dateStyle: 'medium', timeStyle: 'short' }) {
  const date = value instanceof Date ? value : new Date(value)
  if (isNaN(date.getTime())) return String(value)
  return new Intl.DateTimeFormat(locale, options).format(date)
}

export function formatNumber(value, locale, options) {
  if (!Number.isFinite(value)) return String(value)
  return new Intl.NumberFormat(locale, options).format(value)
}

const SIZE_UNITS = ['B', 'KB', 'MB', 'GB', 'TB']

export function formatFileSize(bytes, locale) {
  if (!Number.isFinite(bytes) || bytes < 0) return '—'
  if (bytes < 1024) return `${bytes} ${SIZE_UNITS[0]}`
  let value = bytes
  let unit = 0
  while (value >= 1024 && unit < SIZE_UNITS.length - 1) {
    value /= 1024
    unit++
  }
  return `${formatNumber(Number(value.toFixed(1)), locale)} ${SIZE_UNITS[unit]}`
}

// Storage labels arrive from the backend as English fallbacks; the kind is
// what identifies them, so translate on that and fall back if a key is missing.
export function volumeLabel(volume, t) {
  if (!volume?.kind) return volume?.label || ''
  const key = `storage.${volume.kind}`
  const translated = t(key, { index: volume.index })
  return translated === key ? volume.label : translated
}

// Storage bar colour: calm until the disk is nearly full.
export function volumeBarClass(pct) {
  if (pct >= 93) return 'bg-err'
  if (pct >= 80) return 'bg-warn'
  return 'bg-accent'
}
