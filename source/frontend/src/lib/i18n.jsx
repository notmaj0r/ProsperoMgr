import { createContext, useCallback, useContext, useEffect, useMemo, useState } from 'react'
import { resolveKey, resolvePlurals, interpolate } from './i18nFormat.js'

// Lightweight internal i18n — a JSON dictionary per locale plus Intl for plural/date/number
// formatting. Only en-US is a static import (guaranteed fallback); every other locale loads on demand.
import enUS from '../locales/en-US.json'

// Regional-indicator flag emoji need glyphs the PS5's system font doesn't have, so they
// render as tofu/letter boxes instead — SVGs sidestep that entirely.
import flagEnUS from '../assets/flags/en-US.svg'
import flagArSA from '../assets/flags/ar-SA.svg'
import flagZhCN from '../assets/flags/zh-CN.svg'
import flagFrFR from '../assets/flags/fr-FR.svg'
import flagDeDE from '../assets/flags/de-DE.svg'
import flagItIT from '../assets/flags/it-IT.svg'
import flagJaJP from '../assets/flags/ja-JP.svg'
import flagKoKR from '../assets/flags/ko-KR.svg'
import flagFaIR from '../assets/flags/fa-IR.svg'
import flagHuHU from '../assets/flags/hu-HU.svg'
import flagPlPL from '../assets/flags/pl-PL.svg'
import flagPtBR from '../assets/flags/pt-BR.svg'
import flagRuRU from '../assets/flags/ru-RU.svg'
import flagEsES from '../assets/flags/es-ES.svg'
import flagThTH from '../assets/flags/th-TH.svg'
import flagTrTR from '../assets/flags/tr-TR.svg'
import flagUkUA from '../assets/flags/uk-UA.svg'

export const LOCALES = [
  { id: 'en-US', flag: flagEnUS, name: 'English' },
  { id: 'ar-SA', flag: flagArSA, name: 'العربية' },
  { id: 'zh-CN', flag: flagZhCN, name: '简体中文' },
  { id: 'fr-FR', flag: flagFrFR, name: 'Français' },
  { id: 'de-DE', flag: flagDeDE, name: 'Deutsch' },
  { id: 'it-IT', flag: flagItIT, name: 'Italiano' },
  { id: 'ja-JP', flag: flagJaJP, name: '日本語' },
  { id: 'ko-KR', flag: flagKoKR, name: '한국어' },
  { id: 'fa-IR', flag: flagFaIR, name: 'فارسی' },
  { id: 'hu-HU', flag: flagHuHU, name: 'Magyar' },
  { id: 'pl-PL', flag: flagPlPL, name: 'Polski' },
  { id: 'pt-BR', flag: flagPtBR, name: 'Português (Brasil)' },
  { id: 'ru-RU', flag: flagRuRU, name: 'Русский' },
  { id: 'es-ES', flag: flagEsES, name: 'Español' },
  { id: 'th-TH', flag: flagThTH, name: 'ไทย' },
  { id: 'tr-TR', flag: flagTrTR, name: 'Türkçe' },
  { id: 'uk-UA', flag: flagUkUA, name: 'Українська' },
]
const KNOWN_LOCALES = new Set(LOCALES.map((l) => l.id))

// One dynamic import() per locale, written out explicitly (not a templated path) so Vite
// can see every possible target at build time and give each its own small chunk.
const LOCALE_LOADERS = {
  'ar-SA': () => import('../locales/ar-SA.json'),
  'zh-CN': () => import('../locales/zh-CN.json'),
  'fr-FR': () => import('../locales/fr-FR.json'),
  'de-DE': () => import('../locales/de-DE.json'),
  'it-IT': () => import('../locales/it-IT.json'),
  'ja-JP': () => import('../locales/ja-JP.json'),
  'ko-KR': () => import('../locales/ko-KR.json'),
  'fa-IR': () => import('../locales/fa-IR.json'),
  'hu-HU': () => import('../locales/hu-HU.json'),
  'pl-PL': () => import('../locales/pl-PL.json'),
  'pt-BR': () => import('../locales/pt-BR.json'),
  'ru-RU': () => import('../locales/ru-RU.json'),
  'es-ES': () => import('../locales/es-ES.json'),
  'th-TH': () => import('../locales/th-TH.json'),
  'tr-TR': () => import('../locales/tr-TR.json'),
  'uk-UA': () => import('../locales/uk-UA.json'),
}

// Module-level, not component state — a dictionary fetched once stays fetched for the tab's
// life, so switching back to an earlier language is instant, no repeat network/parse cost.
const loadedDictionaries = { 'en-US': enUS }
const pendingLoads = {}

function loadDictionary(id) {
  if (loadedDictionaries[id]) return Promise.resolve(loadedDictionaries[id])
  if (pendingLoads[id]) return pendingLoads[id]
  const loader = LOCALE_LOADERS[id]
  if (!loader) return Promise.resolve(enUS)
  const promise = loader()
    .then((mod) => {
      loadedDictionaries[id] = mod.default
      delete pendingLoads[id]
      return mod.default
    })
    .catch((e) => {
      delete pendingLoads[id]
      if (import.meta.env?.DEV) console.warn(`[i18n] failed to load locale "${id}":`, e)
      return enUS
    })
  pendingLoads[id] = promise
  return promise
}

const RTL_LOCALES = new Set(['ar-SA', 'fa-IR'])
export const DEFAULT_LOCALE = 'en-US'
export const LOCALE_KEY = 'pmgr_locale'

export function directionFor(locale) {
  return RTL_LOCALES.has(locale) ? 'rtl' : 'ltr'
}

export function loadSavedLocale() {
  const saved = window.localStorage.getItem(LOCALE_KEY)
  return KNOWN_LOCALES.has(saved) ? saved : DEFAULT_LOCALE
}

// Applied as early as possible (main.jsx, before first paint) and again on every language
// change, so there's never a flash of the wrong language or the wrong text direction.
export function applyLocaleToDocument(locale) {
  const root = document.documentElement
  root.lang = locale
  root.dir = directionFor(locale)
}

// Awaited in main.jsx before the app renders, for the saved locale — the alternative (mount
// en-US, swap once the chunk arrives) is exactly the flash this prevents. English visitors pay nothing extra.
export async function preloadSavedLocale() {
  await loadDictionary(loadSavedLocale())
}

export const I18nContext = createContext(null)

export function I18nProvider({ children }) {
  const [locale, setLocaleState] = useState(loadSavedLocale)
  // The active dictionary has to live in state too: a module-level cache hit
  // wouldn't re-render when its promise resolves.
  const [dict, setDict] = useState(() => loadedDictionaries[locale] || enUS)

  useEffect(() => {
    applyLocaleToDocument(locale)
    window.localStorage.setItem(LOCALE_KEY, locale)
  }, [locale])

  const setLocale = useCallback((next) => {
    if (!KNOWN_LOCALES.has(next)) return
    if (loadedDictionaries[next]) {
      // Already fetched this session (or it's en-US) — switch immediately,
      // no loading state, no flash.
      setDict(loadedDictionaries[next])
      setLocaleState(next)
      return
    }
    // Fetch before committing the switch, so t() never resolves keys against a
    // dictionary that hasn't loaded.
    loadDictionary(next).then((loaded) => {
      setDict(loaded)
      setLocaleState(next)
    })
  }, [])

  const t = useCallback((key, vars) => {
    let template = resolveKey(dict, key)
    if (template === undefined) {
      template = resolveKey(enUS, key)
      if (template === undefined) {
        if (import.meta.env?.DEV) console.warn(`[i18n] missing translation key: ${key}`)
        return key
      }
    }
    template = resolvePlurals(template, vars, locale)
    return interpolate(template, vars)
  }, [dict, locale])

  const value = useMemo(() => ({ locale, setLocale, t, direction: directionFor(locale) }), [locale, setLocale, t])

  return <I18nContext.Provider value={value}>{children}</I18nContext.Provider>
}

export function useI18n() {
  const ctx = useContext(I18nContext)
  if (!ctx) throw new Error('useI18n() must be used inside I18nProvider')
  return ctx
}
