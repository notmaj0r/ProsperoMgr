import { useState } from 'react'
import { api } from '../lib/api.js'

const SIZES = {
  xs: 'h-7 w-7 rounded-lg text-xs',
  sm: 'h-8 w-8 rounded-lg text-sm',
  md: 'h-9 w-9 rounded-lg text-base',
  xl: 'h-14 w-14 rounded-xl text-2xl',
}

// A payload's logo when one was found for its project. Without one (or if the image fails) it shows `fallback`,
// or the payload's initial, in a box of the same size so a list stays aligned.
export default function PayloadLogo({ repo, name = '', size = 'md', dim = false, className = '', fallback = null }) {
  const [failed, setFailed] = useState(false)
  if (repo && !failed) {
    return (
      <img
        src={api.catalog.iconUrl(repo)}
        alt=""
        onError={() => setFailed(true)}
        className={`${SIZES[size]} shrink-0 bg-panel-2 object-cover ${dim ? 'opacity-50' : ''} ${className}`}
      />
    )
  }
  if (fallback) return fallback
  return (
    <span
      aria-hidden="true"
      className={`${SIZES[size]} grid shrink-0 place-items-center bg-panel-2 font-bold uppercase ${dim ? 'text-text-dim' : 'text-accent'} ${className}`}
    >
      {name.charAt(0)}
    </span>
  )
}
