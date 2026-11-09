import { FileIcon, ImageIcon, InstallIcon, LaunchIcon, ZipIcon } from './icons.jsx'

const TYPES = [
  { exts: ['pkg'], Icon: InstallIcon, color: 'text-info' },
  { exts: ['elf', 'self', 'bin', 'prx', 'sprx'], Icon: LaunchIcon, color: 'text-accent' },
  { exts: ['zip', 'rar'], Icon: ZipIcon, color: 'text-warn' },
  { exts: ['png', 'jpg', 'jpeg', 'gif', 'bmp', 'webp'], Icon: ImageIcon, color: 'text-ok' },
  { exts: ['txt', 'log', 'ini', 'json', 'cfg', 'xml', 'md', 'sh', 'lua', 'js', 'yml', 'yaml'], Icon: FileIcon, color: 'text-text' },
]

export default function FileTypeIcon({ name, size = 16 }) {
  const ext = name.includes('.') ? name.split('.').pop().toLowerCase() : ''
  const type = TYPES.find((entry) => entry.exts.includes(ext))
  const Icon = type ? type.Icon : FileIcon
  return <span className={`inline-flex shrink-0 ${type ? type.color : 'text-text-dim'}`}><Icon size={size} /></span>
}
