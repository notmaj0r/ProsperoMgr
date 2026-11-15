import { useState } from 'react'
import { FolderIcon } from './icons.jsx'
import FadeImage from './FadeImage.jsx'
import { api } from '../lib/api.js'

// Game folders show the game's cover; a missing or broken image falls back to the plain folder icon.
// `small` is for dense lists such as the folder picker.
export default function GameCover({ game, small = false }) {
  const [failed, setFailed] = useState(false)
  const size = small ? 'h-9 w-9' : 'pmgr-game-cover h-11 w-11'
  if (!game.has_icon || failed) return <span className={`${size} grid shrink-0 place-items-center rounded-lg bg-panel-2`}><FolderIcon size={18} /></span>
  return <FadeImage src={api.games.iconUrl(game)} alt="" loading="lazy" onError={() => setFailed(true)} className={`${size} shrink-0 rounded-lg object-cover`} />
}
