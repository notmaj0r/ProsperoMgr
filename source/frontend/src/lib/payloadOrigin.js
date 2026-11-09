// Where a library payload came from, as one of the keys under payloads.origin.
const KNOWN_ORIGINS = ['repository', 'uploaded', 'usb', 'payloadManager']

export function payloadOrigin(payload) {
  if (payload.bundled) return 'bundled'
  return KNOWN_ORIGINS.includes(payload.source) ? payload.source : 'other'
}
