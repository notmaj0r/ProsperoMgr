import { translate } from './i18n.jsx'

// Messages the backend writes in English, matched by their shape so they show in the user's language.
const RULES = [
  [/^Could not reach (\S+)\. Check the PS5's network and DNS settings\.$/, 'errors.netUnreachable', ['host']],
  [/^Connection to (\S+) timed out\.$/, 'errors.netTimeout', ['host']],
  [/^(\S+) returned 404 \(not found\)\.$/, 'errors.netNotFound', ['host']],
  [/^(\S+) temporarily rejected the request \(HTTP (\d+)\) — possibly rate limited\.$/, 'errors.netRateLimited', ['host', 'status']],
  [/^(\S+) returned HTTP (\d+)\.$/, 'errors.netHttp', ['host', 'status']],
  [/^Request to (\S+) failed\.$/, 'errors.netFailed', ['host']],
  [/^could not reach GitHub$/, 'errors.githubUnreachable', []],
  [/^unexpected release data from GitHub$/, 'errors.githubUnexpected', []],
  [/^the release has no ProsperoMgr\.elf file$/, 'errors.releaseNoElf', []],
  [/^The repository was reached successfully, but it has no stable releases\.$/, 'errors.releasesNone', []],
  [/^The repository was reached successfully, but it has no installable releases or pre-releases\.$/, 'errors.releasesNoneInstallable', []],
  [/^The server's response could not be understood as a release list\.$/, 'errors.releaseListInvalid', []],
  [/^This repository is already in your directory\.$/, 'errors.repoDuplicate', []],
  [/^not enough (?:free )?space( at the destination| to allocate the destination file| \(32 MB safety reserve required\))?$/i, 'errors.noSpace', []],
]

// The installer's own error code comes first and the explanation follows after a dash.
const HINTS = [
  [' — not enough free space on the console. Free some space or move games to the extended storage', 'errors.pkgNoSpaceHint'],
  [/ — the console refuses this package\..*Package Installer$/s, 'errors.pkgRefusedHint'],
]

export function localizeError(message) {
  if (typeof message !== 'string' || !message) return message
  for (const [pattern, key, names] of RULES) {
    const match = message.match(pattern)
    if (!match) continue
    const vars = {}
    names.forEach((name, index) => { vars[name] = match[index + 1] })
    return translate(key, vars)
  }
  for (const [hint, key] of HINTS) {
    const found = typeof hint === 'string' ? message.includes(hint) : hint.test(message)
    if (found) return message.replace(hint, ` — ${translate(key)}`)
  }
  return message
}
