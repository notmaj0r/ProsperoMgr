// Reads a package's own header in the browser so the review card can name it before anything is
// uploaded. Mirrors the backend's ReadPkgMetadata; any failure just leaves the details empty.

const HEAD_BYTES = 2.5 * 1024 * 1024
const MAX_COVER = 10 * 1024 * 1024
const SEND_COVER = 900 * 1024 // what the console accepts in one request
const CONTENT_ID = /[A-Z]{2}\d{4}-[A-Z]{4}\d{5}_\d{2}-[A-Z0-9_]{16}/

// FileReader, not Blob.arrayBuffer(): the PS5's WebKit build has it but it is slow there.
function readSlice(file, start, length) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader()
    reader.onload = () => resolve(reader.result)
    reader.onerror = () => reject(reader.error)
    reader.readAsArrayBuffer(file.slice(start, start + length))
  })
}

function text(bytes, start, length) {
  let out = ''
  for (let i = start; i < start + length && i < bytes.length && bytes[i] !== 0; i++) out += String.fromCharCode(bytes[i])
  return out.trim()
}

function fieldOf(json, key) {
  const match = new RegExp(`"${key}"\\s*:\\s*"([^"]*)"`).exec(json)
  return match ? match[1] : ''
}

// param.sfo: a key table plus a data table, little-endian.
function readSfo(view, bytes, base, size, out) {
  if (size < 20 || text(bytes, base + 1, 3) !== 'PSF') return
  const keysAt = view.getUint32(base + 8, true)
  const dataAt = view.getUint32(base + 12, true)
  const count = Math.min(view.getUint32(base + 16, true), 1024)
  for (let i = 0; i < count; i++) {
    const entry = base + 20 + i * 16
    if (entry + 16 > base + size) break
    const key = text(bytes, base + keysAt + view.getUint16(entry, true), 32)
    const value = text(bytes, base + dataAt + view.getUint32(entry + 12, true), view.getUint32(entry + 4, true))
    if (!value) continue
    if (key === 'TITLE' && !out.titleName) out.titleName = value
    else if (key === 'TITLE_ID' && !out.titleId) out.titleId = value
    else if (key === 'CATEGORY' && !out.category) out.category = value
    else if ((key === 'APP_VER' || key === 'VERSION') && !out.version) out.version = value
  }
}

// Large covers (some are several MB) are scaled down: the console only needs a small picture.
async function fitCover(blob) {
  if (blob.size <= SEND_COVER) return blob
  try {
    const source = URL.createObjectURL(blob)
    const image = await new Promise((resolve, reject) => {
      const img = new Image()
      img.onload = () => resolve(img)
      img.onerror = reject
      img.src = source
    })
    URL.revokeObjectURL(source)
    const side = 512
    const canvas = document.createElement('canvas')
    canvas.width = side
    canvas.height = side
    canvas.getContext('2d').drawImage(image, 0, 0, side, side)
    const small = await new Promise((resolve) => canvas.toBlob(resolve, 'image/png'))
    return small && small.size > 0 && small.size <= SEND_COVER ? small : blob
  } catch {
    return blob
  }
}

// Base, update or DLC, decided like the console's own package manager does: the category alone is
// not enough, since PS5 updates and DLC are also told apart by header flags and patch entries.
function packageKind(category, facts) {
  const isLih = magicAt(facts.container, 0, 'LIH')
  const delta = (facts.typeMagic & 0xff) === 0x1e || (facts.typeMagic & 0xff000000) === 0x41000000
  if (isLih || (facts.flags & 0x40100000) || facts.hasPlaygo || facts.hasDelta || delta || category.startsWith('gp')) return 'update'
  if (category.startsWith('ac') || category.startsWith('al') || category === 'addcont') return 'dlc'
  if (facts.sfoCategory && /^(gd|bd|gc|wt)/.test(category)) return 'base'
  if ((facts.typeMagic & 0xff) === 1 && !facts.hasBaseMeta) return 'dlc'
  return 'base'
}

const le64 = (view, at) => view.getUint32(at, true) + view.getUint32(at + 4, true) * 4294967296
const magicAt = (bytes, at, name) => name.split('').every((c, k) => bytes[at + 1 + k] === c.charCodeAt(0)) && bytes[at] === 0x7f

// PS5 packages wrap the CNT header in an FIH or LIH container; this finds where it starts.
async function findCnt(file, head, view) {
  const isCnt = async (at) => at >= 0 && at + 0x80 <= file.size && magicAt(new Uint8Array(await readSlice(file, at, 4)), 0, 'CNT')
  if (magicAt(head, 0, 'CNT')) return 0
  if (magicAt(head, 0, 'LIH')) {
    const at = le64(view, 0x30)
    return view.getUint16(6, true) === 1 && at >= 0x80 && await isCnt(at) ? at : -1
  }
  if (!magicAt(head, 0, 'FIH')) return -1
  const direct = le64(view, 0x58)
  if (direct > 0 && await isCnt(direct)) return direct
  for (let off = 0x10; off + 8 <= head.length; off += 8) {
    const at = le64(view, off)
    if (at >= 0x10000 && at % 0x1000 === 0 && await isCnt(at)) return at
  }
  return -1
}

export async function readPkgInfo(file) {
  const info = { size: file.size, contentId: '', titleId: '', titleName: '', version: '', category: '', icon: '', debug: '' }
  try {
    const headBuffer = await readSlice(file, 0, 0x1000)
    const head = new Uint8Array(headBuffer)
    const match = CONTENT_ID.exec(String.fromCharCode.apply(null, head.subarray(0, 0x1000)))
    if (match) info.contentId = match[0]

    const cnt = await findCnt(file, head, new DataView(headBuffer))
    info.debug += `head=${Array.from(head.subarray(0, 8), (b) => b.toString(16).padStart(2, '0')).join('')} cnt=${cnt}`
    if (cnt >= 0) {
      // Entry offsets count from the CNT header, so the buffer starts there.
      const buffer = await readSlice(file, cnt, HEAD_BYTES)
      const bytes = new Uint8Array(buffer)
      const view = new DataView(buffer)
      if (!info.contentId) info.contentId = text(bytes, 0x40, 48)
      const count = view.getUint32(0x10)
      const tableAt = view.getUint32(0x18)
      const seen = []
      for (let i = 0; i < Math.min(count, 64) && tableAt + (i + 1) * 32 <= bytes.length; i++) seen.push(view.getUint32(tableAt + i * 32).toString(16))
      info.debug += ` entries=${count} table=${tableAt} types=${seen.join(',')}`
      if (count > 0 && count <= 2048 && tableAt <= 0x200000) {
        // Entries are told apart by type, or by name when the package carries a name table (type 0x0200).
        let names = ''
        for (let i = 0; i < count && tableAt + (i + 1) * 32 <= bytes.length; i++) {
          const entry = tableAt + i * 32
          const size = view.getUint32(entry + 20)
          const at = view.getUint32(entry + 16)
          if (view.getUint32(entry) === 0x0200 && size > 0 && size < 65536 && at + size <= bytes.length) {
            names = String.fromCharCode.apply(null, bytes.subarray(at, at + size))
            break
          }
        }
        let iconAt = 0
        let iconSize = 0
        let hasPlaygo = false
        let hasDelta = false
        let hasBaseMeta = false
        let sfoCategory = false
        for (let i = 0; i < count; i++) {
          const entry = tableAt + i * 32
          if (entry + 32 > bytes.length) break
          const type = view.getUint32(entry)
          const nameAt = view.getUint32(entry + 4)
          const name = nameAt < names.length ? names.substring(nameAt, names.indexOf('\0', nameAt)) : ''
          const dataAt = view.getUint32(entry + 16)
          const size = view.getUint32(entry + 20)
          if (type === 0x1008 || name === 'app/playgo-chunk.dat') hasPlaygo = true
          if (type === 0x0407 || type === 0x0408 || name === 'target-deltainfo.dat' || name === 'origin-deltainfo.dat') hasDelta = true
          // The cover can sit anywhere in the file, so it is read on its own below.
          if ((type === 0x1200 || name === 'icon0.png') && size > 8 && size <= MAX_COVER) {
            iconAt = dataAt
            iconSize = size
            continue
          }
          if (size === 0 || size > 256 * 1024 || dataAt + size > bytes.length) continue
          if (type === 0x1000 || name === 'param.sfo') {
            const before = info.category
            readSfo(view, bytes, dataAt, size, info)
            if (!before && info.category) sfoCategory = true
          } else if (type === 0x2000 || name === 'param.json') {
            const json = text(bytes, dataAt, size)
            if (/"(applicationDrmType|applicationCategoryType|contentBadgeType)"/.test(json)) hasBaseMeta = true
            info.titleName = info.titleName || fieldOf(json, 'titleName')
            info.titleId = info.titleId || fieldOf(json, 'titleId')
            info.version = info.version || fieldOf(json, 'contentVersion')
            info.category = info.category || fieldOf(json, 'category')
          }
        }
        info.category = packageKind(info.category, { typeMagic: view.getUint32(4), flags: view.getUint32(0x78), container: head, hasPlaygo, hasDelta, hasBaseMeta, sfoCategory })
        if (iconSize) {
          const png = new Uint8Array(await readSlice(file, cnt + iconAt, iconSize))
          info.debug += ` icon=${iconSize}@${iconAt}`
          if (text(png, 1, 3) === 'PNG') info.icon = URL.createObjectURL(await fitCover(new Blob([png], { type: 'image/png' })))
        }
      }
    }
    if (!info.titleId && info.contentId.length >= 16) info.titleId = info.contentId.substr(7, 9)
  } catch (e) {
    // Unreadable header: the card falls back to the file name.
    info.debug += ` error=${e && e.message}`
  }
  return info
}
