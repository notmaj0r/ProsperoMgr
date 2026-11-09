import Pill from './Pill.jsx'
import NotificationsBell from './NotificationsBell.jsx'
import LanguagePicker from './LanguagePicker.jsx'
import PowerMenu from './PowerMenu.jsx'
import UpdateBadge from './UpdateBadge.jsx'
import { useI18n } from '../lib/i18n.jsx'

const LINKS = {
  discord: 'https://discord.gg/E4g6fEqp46',
  x: 'https://x.com/callmemaj0r',
  bluesky: 'https://bsky.app/profile/notmaj0r.bsky.social',
}

// Discord/X/Bluesky/Support stay in the header at every width, by design — narrow layouts go
// icon-only, but the label stays in the DOM (visually hidden) so the accessible name doesn't depend on breakpoint.
function SocialButton({ href, onClick, className, title, icon, label }) {
  const Tag = href ? 'a' : 'button'
  return (
    <Tag
      href={href}
      onClick={onClick}
      target={href ? '_blank' : undefined}
      rel={href ? 'noopener noreferrer' : undefined}
      type={href ? undefined : 'button'}
      title={title}
      aria-label={title}
      className={`pmgr-social-btn px-2.5 2xl:px-[13px] ${className}`}
    >
      {icon}
      <span className="hidden 2xl:inline">{label}</span>
    </Tag>
  )
}

function SocialRow({ onOpenSupport, t }) {
  return (
    <>
      <SocialButton
        href={LINKS.discord}
        title={t('header.discord')}
        className="pmgr-social-discord"
        label={t('header.discordLabel')}
        icon={
          <svg width="15" height="15" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
            <path d="M20.317 4.37a19.79 19.79 0 0 0-4.885-1.515.074.074 0 0 0-.079.037c-.21.375-.444.864-.608 1.25a18.27 18.27 0 0 0-5.487 0 12.64 12.64 0 0 0-.617-1.25.077.077 0 0 0-.079-.037A19.74 19.74 0 0 0 3.677 4.37a.07.07 0 0 0-.032.027C.533 9.046-.32 13.58.099 18.057a.078.078 0 0 0 .03.056 19.9 19.9 0 0 0 5.993 3.03.078.078 0 0 0 .084-.028 14.09 14.09 0 0 0 1.226-1.994.076.076 0 0 0-.041-.106 13.107 13.107 0 0 1-1.872-.892.077.077 0 0 1-.008-.128c.126-.094.252-.192.372-.292a.074.074 0 0 1 .077-.01c3.928 1.793 8.18 1.793 12.062 0a.074.074 0 0 1 .078.01c.12.098.246.198.373.292a.077.077 0 0 1-.006.127 12.3 12.3 0 0 1-1.873.892.077.077 0 0 0-.041.107c.36.698.772 1.362 1.225 1.993a.076.076 0 0 0 .084.028 19.84 19.84 0 0 0 6.002-3.03.077.077 0 0 0 .032-.054c.5-5.177-.838-9.674-3.549-13.66a.061.061 0 0 0-.031-.03ZM8.7 14c-.8 0-1.4-.7-1.4-1.6 0-.9.6-1.6 1.4-1.6.8 0 1.4.7 1.4 1.6 0 .9-.6 1.6-1.4 1.6Zm6.6 0c-.8 0-1.4-.7-1.4-1.6 0-.9.6-1.6 1.4-1.6.8 0 1.4.7 1.4 1.6 0 .9-.6 1.6-1.4 1.6Z" />
          </svg>
        }
      />
      <SocialButton
        href={LINKS.x}
        title={t('header.x')}
        className="pmgr-social-x"
        label={t('header.xLabel')}
        icon={
          <svg width="13" height="13" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
            <path d="M18.244 2H21.5l-7.11 8.13L22.75 22h-6.55l-5.13-6.7L5.2 22H1.94l7.6-8.69L1.5 2h6.72l4.64 6.14L18.244 2Zm-1.14 17.91h1.8L7.24 3.99H5.31l11.793 15.92Z" />
          </svg>
        }
      />
      <SocialButton
        href={LINKS.bluesky}
        title={t('header.bluesky')}
        className="pmgr-social-bluesky"
        label={t('header.blueskyLabel')}
        icon={
          <svg width="15" height="15" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
            <path d="M12 10.8c-1.087-2.114-4.046-6.053-6.798-7.995C2.566.944 1.561 1.266.902 1.565.139 1.908 0 3.08 0 3.768c0 .69.378 5.65.624 6.479.815 2.736 3.713 3.66 6.383 3.364.136-.02.275-.039.415-.06-.138.022-.276.04-.415.06-3.912.58-7.387 2.005-2.83 7.078 5.013 5.19 6.87-1.113 7.823-4.308.953 3.195 2.05 9.271 7.733 4.308 4.267-4.308 1.172-6.498-2.74-7.078a8.741 8.741 0 0 1-.415-.06c.14.02.279.04.415.06 2.67.297 5.568-.628 6.383-3.364.246-.828.624-5.79.624-6.478 0-.69-.139-1.861-.902-2.203-.659-.299-1.664-.62-4.3 1.24C16.046 4.748 13.087 8.687 12 10.8Z" />
          </svg>
        }
      />
      <SocialButton
        onClick={onOpenSupport}
        title={t('header.support')}
        className="pmgr-social-support pmgr-btn-sheen"
        label={t('header.supportLabel')}
        icon={
          <svg width="13" height="13" viewBox="0 0 24 24" fill="currentColor" aria-hidden="true">
            <path d="M12 21.35 10.55 20C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35Z" />
          </svg>
        }
      />
    </>
  )
}

export default function Header({ title, status, connection, onRetry, onOpenHelp, onOpenSupport, onOpenCommand }) {
  const { t } = useI18n()
  const CONNECTION_LABELS = {
    connecting: t('header.connecting'),
    reconnecting: t('header.reconnecting'),
    offline: t('header.offline'),
  }

  const utilityCluster = (
    <>
      {connection !== 'online' && (
        <button
          type="button"
          onClick={connection === 'offline' ? onRetry : undefined}
          className={`pmgr-connection-pill ${connection === 'offline' ? 'is-offline' : 'is-connecting'}`}
          title={connection === 'offline' ? t('header.offlineRetry') : t('header.connectingToServer')}
        >
          <span className="pmgr-connection-dot" />
          <span className="hidden sm:inline">{CONNECTION_LABELS[connection] || t('header.connecting')}</span>
        </button>
      )}
      {status && <span className="hidden sm:inline-flex"><Pill tone="neutral">v{status.version}</Pill></span>}
      <UpdateBadge />
      <button
        onClick={onOpenCommand}
        title={t('header.command')}
        aria-label={t('header.command')}
        className="pmgr-icon-btn hidden h-8 place-items-center gap-1.5 rounded-full border border-panel-border px-2.5 text-text-dim hover:border-accent/50 hover:text-accent sm:grid sm:grid-flow-col"
      >
        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.8" aria-hidden="true">
          <circle cx="11" cy="11" r="7" /><path d="m16.5 16.5 4 4" strokeLinecap="round" />
        </svg>
        <kbd className="hidden text-[10px] font-bold opacity-70 lg:inline">{t('header.commandShortcut')}</kbd>
      </button>
      <LanguagePicker />
      <NotificationsBell />
      <button
        onClick={onOpenHelp}
        title={t('header.help')}
        aria-label={t('header.helpLabel')}
        className="pmgr-icon-btn grid h-8 w-8 place-items-center rounded-full border border-panel-border text-text-dim hover:border-accent/50 hover:text-accent"
      >
        ?
      </button>
      <PowerMenu />
    </>
  )

  return (
    <header className="pmgr-header flex flex-col gap-2 border-b border-panel-border px-4 py-2.5 sm:min-h-[68px] sm:flex-row sm:items-center sm:justify-between sm:gap-3 sm:py-3 sm:px-5">
      <div className="flex min-w-0 items-center justify-between gap-3 sm:justify-start">
        <div className="flex min-w-0 items-center gap-3">
          <div className="flex items-center gap-2 md:hidden">
            <img src="/icon-64.png" alt="" className="h-7 w-7 rounded-md border border-accent/40" />
          </div>
          <h1 className="truncate text-lg font-bold tracking-tight text-text">{title}</h1>
        </div>
        {/* On phones the utility cluster sits on the title row, above the social strip. */}
        <div className="flex shrink-0 items-center gap-1.5 sm:hidden">{utilityCluster}</div>
      </div>

      {/* The pills' glow paints outside their box, and overflow-x clips it. The vertical padding
          gives it room; lg drops the scrollport since the strip never scrolls there. */}
      <div className="-my-2.5 flex min-w-0 items-center gap-1.5 overflow-x-auto overflow-y-visible py-2.5 sm:gap-2 lg:overflow-visible">
        <div className="flex shrink-0 items-center gap-1.5 sm:gap-2">
          <SocialRow onOpenSupport={onOpenSupport} t={t} />
        </div>
        <div className="hidden shrink-0 items-center gap-1.5 sm:flex sm:gap-2">{utilityCluster}</div>
      </div>
    </header>
  )
}
