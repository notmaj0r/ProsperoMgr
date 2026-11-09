// A running process whose name contains a payload extension is almost certainly homebrew —
// elfldr keeps the launched filename, unlike a truncated system daemon. A few Sony processes are excluded.
const PAYLOAD_EXT_RE = /\.(elf|bin|self)$/i

const SYSTEM_PROCESS_BLACKLIST = new Set([
  'mini-syscore.elf',
  'scesysavcontrol.elf',
  'scesyscore.elf',
  'orbis_audiod.elf',
  'agccompositor.elf',
  'fs_cleaner.elf',
  'webrtc_daemon.self',
  'prosperomgr.elf',
  'prospero-pkgd.elf',
])

export function isPayloadLikeProcessName(name) {
  return PAYLOAD_EXT_RE.test(name) && !SYSTEM_PROCESS_BLACKLIST.has(name.toLowerCase())
}
