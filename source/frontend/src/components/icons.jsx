// Small stroke-icon set for action buttons — same visual language as the payload-tile bolt
// icon (24x24, currentColor, strokeWidth 1.6). One file so every view shares the vocabulary.

function IconBase({ children, size = 16, className = '' }) {
  return (
    <svg className={`pmgr-action-icon ${className}`} aria-hidden="true" width={size} height={size} viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.7" strokeLinecap="round" strokeLinejoin="round">
      {children}
    </svg>
  )
}

export const UploadIcon = (p) => (
  <IconBase {...p}>
    <path d="M12 16V4M7 9l5-5 5 5" />
    <path d="M4 16v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3" />
  </IconBase>
)

export const DownloadIcon = (p) => (
  <IconBase {...p}>
    <path d="M12 4v12M7 11l5 5 5-5" />
    <path d="M4 16v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3" />
  </IconBase>
)

export const NewFileIcon = (p) => (
  <IconBase {...p}>
    <path d="M13 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8l-6-5Z" />
    <path d="M13 3v5h5" />
    <path d="M12 12v6M9 15h6" />
  </IconBase>
)

export const NewFolderIcon = (p) => (
  <IconBase {...p}>
    <path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7Z" fill="#f8c74f" stroke="#d99a21" />
    <path d="M12 11v5M9.5 13.5h5" stroke="#76500b" />
  </IconBase>
)

export const RefreshIcon = (p) => (
  <IconBase {...p}>
    <path d="M4 4v5h5" />
    <path d="M20 20v-5h-5" />
    <path d="M5.5 9a7 7 0 0 1 12-3.5L20 8" />
    <path d="M18.5 15a7 7 0 0 1-12 3.5L4 16" />
  </IconBase>
)

export const ZipIcon = (p) => (
  <IconBase {...p}>
    <path d="M4 7h16v13a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7Z" />
    <path d="M4 7 6 3h12l2 4" />
    <path d="M10 11h4v3h-4z" />
  </IconBase>
)

export const UnzipIcon = (p) => (
  <IconBase {...p}>
    <path d="M4 7h16v13a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7Z" />
    <path d="M4 7 6 3h12l2 4" />
    <path d="M12 10v5M9.5 12.5 12 10l2.5 2.5" />
  </IconBase>
)

export const EditIcon = (p) => (
  <IconBase {...p}>
    <path d="M4 20h4L18.5 9.5a2.1 2.1 0 0 0-3-3L5 17v3Z" />
    <path d="M13 6.5l3 3" />
  </IconBase>
)

export const RenameIcon = (p) => (
  <IconBase {...p}>
    <rect x="3" y="6" width="18" height="12" rx="2" />
    <path d="M8.5 9.5v-1L7 9l1.5.5V13M8 13h1" />
    <path d="M14 9.5h4M14 12h4M14 14.5h2.5" />
  </IconBase>
)

export const CopyIcon = (p) => (
  <IconBase {...p}>
    <rect x="8" y="8" width="11" height="11" rx="2" />
    <path d="M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2" />
  </IconBase>
)

export const MoveIcon = (p) => (
  <IconBase {...p}>
    <path d="M5 12h14M15 8l4 4-4 4" />
    <path d="M9 5H5a2 2 0 0 0-2 2v10a2 2 0 0 0 2 2h4" />
  </IconBase>
)

export const GripIcon = (p) => (
  <IconBase {...p}>
    <circle cx="8" cy="6" r="1" fill="currentColor" stroke="none" />
    <circle cx="16" cy="6" r="1" fill="currentColor" stroke="none" />
    <circle cx="8" cy="12" r="1" fill="currentColor" stroke="none" />
    <circle cx="16" cy="12" r="1" fill="currentColor" stroke="none" />
    <circle cx="8" cy="18" r="1" fill="currentColor" stroke="none" />
    <circle cx="16" cy="18" r="1" fill="currentColor" stroke="none" />
  </IconBase>
)

export const PowerIcon = (p) => (
  <IconBase {...p}>
    <path d="M12 3v9" />
    <path d="M7.2 5.8a8 8 0 1 0 9.6 0" />
  </IconBase>
)

export const DeleteIcon = (p) => (
  <IconBase {...p}>
    <path d="M4 7h16" />
    <path d="M9 7V4h6v3" />
    <path d="M6 7l1 13a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-13" />
    <path d="M10 11v6M14 11v6" />
  </IconBase>
)

export const KillIcon = (p) => (
  <IconBase {...p}>
    <circle cx="12" cy="12" r="9" />
    <path d="M9 9l6 6M15 9l-6 6" />
  </IconBase>
)

export const LaunchIcon = (p) => (
  <IconBase {...p}>
    <path d="M6 4l14 8-14 8V4Z" />
  </IconBase>
)

export const InstallIcon = (p) => (
  <IconBase {...p}>
    <path d="M12 3v11M8 10l4 4 4-4" />
    <path d="M4 16v3a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-3" />
  </IconBase>
)

export const GoUpIcon = (p) => (
  <IconBase {...p}>
    <path d="M12 19V5M5 12l7-7 7 7" />
  </IconBase>
)

export const SaveIcon = (p) => (
  <IconBase {...p}>
    <path d="M5 4h11l3 3v13a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1Z" />
    <path d="M8 4v5h7V4M8 13h8v7H8z" />
  </IconBase>
)

export const CloseIcon = (p) => (
  <IconBase {...p}>
    <path d="M6 6l12 12M18 6 6 18" />
  </IconBase>
)

export const PlusIcon = (p) => (
  <IconBase {...p}>
    <path d="M12 5v14M5 12h14" />
  </IconBase>
)

export const UserIcon = (p) => (
  <IconBase {...p}>
    <circle cx="12" cy="8" r="4" />
    <path d="M4 20c0-4 3.5-6 8-6s8 2 8 6" />
  </IconBase>
)

export const ExternalIcon = (p) => (
  <IconBase {...p}>
    <path d="M14 4h6v6" />
    <path d="M20 4 10 14" />
    <path d="M8 6H5a1 1 0 0 0-1 1v12a1 1 0 0 0 1 1h12a1 1 0 0 0 1-1v-3" />
  </IconBase>
)

export const SlidersIcon = (p) => (
  <IconBase {...p}>
    <path d="M4 6h10M17 6h3M4 12h3M9 12h11M4 18h13M20 18h0" />
    <circle cx="16" cy="6" r="2" />
    <circle cx="6" cy="12" r="2" />
    <circle cx="17" cy="18" r="2" />
  </IconBase>
)

export const PaletteIcon = (p) => (
  <IconBase {...p}>
    <path d="M12 3a9 8.5 0 1 0 0 17c1.1 0 2-.9 2-2 0-.5-.2-1-.5-1.4-.3-.4-.5-.9-.5-1.4 0-1.1.9-2 2-2h2.3c1.8 0 3.2-1.5 3.2-3.3C20.5 6 16.8 3 12 3Z" />
    <circle cx="7.5" cy="10.5" r="1" fill="currentColor" stroke="none" />
    <circle cx="11" cy="7" r="1" fill="currentColor" stroke="none" />
    <circle cx="15.5" cy="8" r="1" fill="currentColor" stroke="none" />
  </IconBase>
)

export const FlaskIcon = (p) => (
  <IconBase {...p}>
    <path d="M9 3h6M10 3v5.5L5.5 17a2 2 0 0 0 1.8 3h9.4a2 2 0 0 0 1.8-3L14 8.5V3" />
    <path d="M7.5 14h9" />
  </IconBase>
)

export const InfoIcon = (p) => (
  <IconBase {...p}>
    <circle cx="12" cy="12" r="9" />
    <path d="M12 11v6M12 8v.01" />
  </IconBase>
)

export const ThermometerIcon = (p) => (
  <IconBase {...p}>
    <path d="M12 14.5V5a2 2 0 0 0-4 0v9.5a4 4 0 1 0 4 0Z" />
    <path d="M12 9h2" />
  </IconBase>
)

export const FanIcon = (p) => (
  <IconBase {...p}>
    <circle cx="12" cy="12" r="1.6" fill="currentColor" stroke="none" />
    <path d="M12 12c0-3 1.5-7 4-7 2 0 3 1.5 3 3 0 2.5-3.5 4-7 4Z" />
    <path d="M12 12c-3 0-7-1.5-7-4 0-2 1.5-3 3-3 2.5 0 4 3.5 4 7Z" />
    <path d="M12 12c3 0 7 1.5 7 4 0 2-1.5 3-3 3-2.5 0-4-3.5-4-7Z" />
  </IconBase>
)

export const ClockIcon = (p) => (
  <IconBase {...p}>
    <circle cx="12" cy="12" r="9" />
    <path d="M12 7v5l3.5 2" strokeLinecap="round" />
  </IconBase>
)

export const ShieldIcon = (p) => (
  <IconBase {...p}>
    <path d="M12 3.5 19 6v6c0 4.2-2.9 7-7 8.5-4.1-1.5-7-4.3-7-8.5V6l7-2.5Z" />
  </IconBase>
)

export const LockIcon = (p) => (
  <IconBase {...p}>
    <rect x="5" y="11" width="14" height="9" rx="2" />
    <path d="M8 11V7a4 4 0 0 1 8 0v4" strokeLinecap="round" />
  </IconBase>
)

export const CheckCircleIcon = (p) => (
  <IconBase {...p}>
    <circle cx="12" cy="12" r="9" />
    <path d="m8 12.5 2.5 2.5L16 9.5" />
  </IconBase>
)

export const AlertTriangleIcon = (p) => (
  <IconBase {...p}>
    <path d="M12 3.5 21.5 20h-19L12 3.5Z" />
    <path d="M12 10v4M12 17v.01" />
  </IconBase>
)

export const XCircleIcon = (p) => (
  <IconBase {...p}>
    <circle cx="12" cy="12" r="9" />
    <path d="m9 9 6 6M15 9l-6 6" />
  </IconBase>
)

export const CoffeeIcon = (p) => (
  <IconBase {...p}>
    <path d="M5 9h12v6a4 4 0 0 1-4 4H9a4 4 0 0 1-4-4V9Z" />
    <path d="M17 10h1.5a2.5 2.5 0 0 1 0 5H17" />
    <path d="M8 6c0-1 1-1 1-2M12 6c0-1 1-1 1-2" strokeLinecap="round" />
  </IconBase>
)

export const BitcoinIcon = (p) => (
  <IconBase {...p}>
    <circle cx="12" cy="12" r="9" />
    <path d="M9.5 8h4a2 2 0 0 1 0 4h-4M9.5 12h4.5a2 2 0 0 1 0 4h-4.5M10.5 8V6M13 8V6M10.5 18v-2M13 18v-2" strokeLinecap="round" />
  </IconBase>
)

export const PixIcon = ({ size = 16 }) => (
  <svg className="pmgr-action-icon" aria-hidden="true" width={size} height={size} viewBox="0 0 24 24" fill="currentColor">
    <path d="M5.283 18.36a3.505 3.505 0 0 0 2.493-1.032l3.6-3.6a.684.684 0 0 1 .946 0l3.613 3.613a3.504 3.504 0 0 0 2.493 1.032h.71l-4.56 4.56a3.647 3.647 0 0 1-5.156 0L4.85 18.36ZM18.428 5.627a3.505 3.505 0 0 0-2.493 1.032l-3.613 3.614a.67.67 0 0 1-.946 0l-3.6-3.6A3.505 3.505 0 0 0 5.283 5.64h-.434l4.573-4.572a3.646 3.646 0 0 1 5.156 0l4.559 4.559ZM1.068 9.422 3.79 6.699h1.492a2.483 2.483 0 0 1 1.744.722l3.6 3.6a1.73 1.73 0 0 0 2.443 0l3.614-3.613a2.482 2.482 0 0 1 1.744-.723h1.767l2.737 2.737a3.646 3.646 0 0 1 0 5.156l-2.736 2.736h-1.768a2.482 2.482 0 0 1-1.744-.722l-3.613-3.613a1.77 1.77 0 0 0-2.444 0l-3.6 3.6a2.483 2.483 0 0 1-1.744.722H3.791l-2.723-2.723a3.646 3.646 0 0 1 0-5.156" />
  </svg>
)

// The actual octocat mark, not a stroke redraw — same convention the
// Discord/X/Bluesky brand buttons already use (real logo, not IconBase).
export const GithubIcon = ({ size = 16, className = '' }) => (
  <svg className={className} aria-hidden="true" width={size} height={size} viewBox="0 0 24 24" fill="currentColor">
    <path d="M12 .5C5.73.5.5 5.73.5 12c0 5.08 3.29 9.39 7.86 10.91.58.11.79-.25.79-.56 0-.27-.01-1.17-.02-2.12-3.2.7-3.88-1.36-3.88-1.36-.53-1.34-1.29-1.7-1.29-1.7-1.05-.72.08-.71.08-.71 1.17.08 1.78 1.2 1.78 1.2 1.03 1.77 2.71 1.26 3.37.96.1-.75.4-1.26.73-1.55-2.55-.29-5.24-1.28-5.24-5.7 0-1.26.45-2.29 1.19-3.09-.12-.29-.52-1.47.11-3.06 0 0 .97-.31 3.18 1.18a11.1 11.1 0 0 1 2.9-.39c.98 0 1.97.13 2.9.39 2.2-1.49 3.17-1.18 3.17-1.18.63 1.59.23 2.77.11 3.06.74.8 1.19 1.83 1.19 3.09 0 4.43-2.69 5.41-5.25 5.7.41.36.78 1.06.78 2.14 0 1.55-.01 2.79-.01 3.17 0 .31.21.68.8.56A11.5 11.5 0 0 0 23.5 12c0-6.27-5.23-11.5-11.5-11.5Z" />
  </svg>
)

export const YoutubeIcon = (p) => (
  <IconBase {...p}>
    <path d="M21 12c0-2.5-.3-4.2-.7-5.1a2.6 2.6 0 0 0-1.6-1.5C17.5 5 15.2 5 12 5s-5.5 0-6.7.4a2.6 2.6 0 0 0-1.6 1.5C3.3 7.8 3 9.5 3 12s.3 4.2.7 5.1a2.6 2.6 0 0 0 1.6 1.5c1.2.4 3.5.4 6.7.4s5.5 0 6.7-.4a2.6 2.6 0 0 0 1.6-1.5c.4-.9.7-2.6.7-5.1Z" />
    <path d="m10 9 5 3-5 3V9Z" fill="currentColor" stroke="none" />
  </IconBase>
)

export const HeartIcon = (p) => (
  <IconBase {...p}>
    <path d="M20.8 5.7a5.2 5.2 0 0 0-7.4 0L12 7.1l-1.4-1.4a5.2 5.2 0 0 0-7.4 7.4L12 21l8.8-7.9a5.2 5.2 0 0 0 0-7.4Z" />
  </IconBase>
)

export const StarIcon = ({ filled = false, ...p }) => (
  <IconBase {...p}>
    <path d="m12 2.8 2.8 5.7 6.3.9-4.6 4.4 1.1 6.3-5.6-3-5.6 3 1.1-6.3-4.6-4.4 6.3-.9L12 2.8Z" fill={filled ? 'currentColor' : 'none'} />
  </IconBase>
)

export const ChevronRightIcon = (p) => (
  <IconBase {...p}>
    <path d="m9 5 7 7-7 7" />
  </IconBase>
)

export const ArrowLeftIcon = (p) => (
  <IconBase {...p}>
    <path d="M19 12H5M10 7l-5 5 5 5" />
  </IconBase>
)

export const FolderIcon = (p) => (
  <IconBase {...p}>
    <path d="M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V7Z" fill="#f8c74f" stroke="#d99a21" />
  </IconBase>
)

export const FileIcon = (p) => (
  <IconBase {...p}>
    <path d="M6 3h8l4 4v14H6a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2Z" />
    <path d="M14 3v5h5M8 13h7M8 17h5" />
  </IconBase>
)

export const SunIcon = (p) => (
  <IconBase {...p}>
    <circle cx="12" cy="12" r="4" />
    <path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4" />
  </IconBase>
)

export const MoonIcon = (p) => (
  <IconBase {...p}>
    <path d="M20 15.4A8.5 8.5 0 0 1 8.6 4a8.5 8.5 0 1 0 11.4 11.4Z" />
  </IconBase>
)

export const ClipboardIcon = (p) => (
  <IconBase {...p}>
    <rect x="6" y="4" width="12" height="17" rx="2" />
    <path d="M9 4V3a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v1" />
    <path d="M9 11h6M9 15h6" strokeLinecap="round" />
  </IconBase>
)

export const ImageIcon = (p) => (
  <IconBase {...p}>
    <rect x="3" y="4" width="18" height="16" rx="2" />
    <circle cx="9" cy="10" r="1.7" />
    <path d="M21 16l-5.5-5.5a2 2 0 0 0-2.8 0L4 19" />
  </IconBase>
)

export const LogsIcon = (p) => (
  <IconBase {...p}>
    <rect x="3" y="4" width="18" height="16" rx="2" />
    <path d="m7 10 3 2-3 2M13 14h4" />
  </IconBase>
)

export const StopIcon = (p) => (
  <IconBase {...p}>
    <rect x="6" y="6" width="12" height="12" rx="2" />
  </IconBase>
)

export const SearchIcon = (p) => (
  <IconBase {...p}>
    <circle cx="11" cy="11" r="7" /><path d="m16.5 16.5 4 4" />
  </IconBase>
)

export const StarFilledIcon = ({ size = 16, className = '' }) => (
  <svg className={`pmgr-action-icon ${className}`} aria-hidden="true" width={size} height={size} viewBox="0 0 24 24" fill="currentColor">
    <path d="m12 2 3.09 6.26L22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01L12 2Z" />
  </svg>
)

export const EyeIcon = (p) => (
  <IconBase {...p}>
    <path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12Z" /><circle cx="12" cy="12" r="3" />
  </IconBase>
)

export const EyeOffIcon = (p) => (
  <IconBase {...p}>
    <path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12Z" /><circle cx="12" cy="12" r="3" /><path d="M4 4l16 16" />
  </IconBase>
)
