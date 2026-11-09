// Short relative time for notification rows — "3 minutes ago", "2 hours ago",
// localized via Intl.RelativeTimeFormat instead of a hardcoded English suffix.
export function timeAgo(ts, locale) {
  const s = Math.floor((Date.now() - ts) / 1000)
  // The console's older browser has no RelativeTimeFormat, so it shows the clock time instead.
  if (typeof Intl.RelativeTimeFormat !== 'function') {
    return new Date(ts).toLocaleTimeString(locale, { hour: '2-digit', minute: '2-digit' })
  }
  const rtf = new Intl.RelativeTimeFormat(locale, { numeric: 'auto' })
  if (s < 60) return rtf.format(0, 'second')
  const m = Math.floor(s / 60)
  if (m < 60) return rtf.format(-m, 'minute')
  const h = Math.floor(m / 60)
  if (h < 24) return rtf.format(-h, 'hour')
  return rtf.format(-Math.floor(h / 24), 'day')
}
