import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './index.css'
import App from './App.jsx'
import { ToastProvider } from './lib/ToastContext.jsx'
import { applyEffects, isPS5, loadReducedEffects, markFlexGap } from './lib/effects.js'
import { I18nProvider, applyLocaleToDocument, loadSavedLocale, preloadSavedLocale } from './lib/i18n.jsx'

// Apply the saved theme before first paint — otherwise it only kicks in
// once Settings mounts, flashing default Dark Blue first.
const savedTheme = window.localStorage.getItem('pmgr_theme')
if (savedTheme === 'light') {
  document.documentElement.dataset.pmgrTheme = savedTheme
} else if (savedTheme && savedTheme !== 'dark') {
  window.localStorage.setItem('pmgr_theme', 'dark')
}

// Same idea for Visual Effects (Full/Reduced) — apply before first paint so
// a Reduced preference never flashes full motion first.
document.documentElement.classList.toggle('pmgr-ps5', isPS5())
applyEffects(loadReducedEffects())
markFlexGap()

// Paths, names and links are not prose, so the on-screen keyboard should not capitalize or correct them.
document.addEventListener('focusin', (event) => {
  const field = event.target
  if (field.tagName !== 'INPUT' && field.tagName !== 'TEXTAREA') return
  if (!field.hasAttribute('autocapitalize')) field.setAttribute('autocapitalize', 'off')
  if (!field.hasAttribute('autocorrect')) field.setAttribute('autocorrect', 'off')
  if (!field.hasAttribute('spellcheck')) field.setAttribute('spellcheck', 'false')
})

// And for language/direction — set lang/dir before React ever renders, so
// there's no English -> saved-language flash and no LTR -> RTL layout jump.
applyLocaleToDocument(loadSavedLocale())

// Non-English locales are code-split — fetch the saved one before mounting.
// IIFE, not top-level await: the PS5's WebKit build can't parse that.
;(async () => {
  await preloadSavedLocale()

  createRoot(document.getElementById('root')).render(
    <StrictMode>
      <I18nProvider>
        <ToastProvider>
          <App />
        </ToastProvider>
      </I18nProvider>
    </StrictMode>,
  )
})()
