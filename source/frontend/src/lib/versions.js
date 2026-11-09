// Release tags like "v1.15-beta1" or "1.13.1": numbers compare part by part, and a pre-release
// ranks below the same number without a suffix.
function parse(tag) {
  const match = /^v?(\d+(?:\.\d+)*)[-+._]?(.*)$/i.exec(String(tag).trim())
  return match ? { numbers: match[1].split('.').map(Number), pre: match[2] } : null
}

// True when `latest` is a newer release than `installed`. Tags that can't be read as versions
// count as different, so a changed tag still shows up.
export function isNewerVersion(latest, installed) {
  const a = parse(latest)
  const b = parse(installed)
  if (!a || !b) return latest !== installed
  for (let i = 0; i < Math.max(a.numbers.length, b.numbers.length); i++) {
    const x = a.numbers[i] || 0
    const y = b.numbers[i] || 0
    if (x !== y) return x > y
  }
  if (a.pre === b.pre) return false
  if (!a.pre) return true
  if (!b.pre) return false
  return a.pre.localeCompare(b.pre, undefined, { numeric: true }) > 0
}
