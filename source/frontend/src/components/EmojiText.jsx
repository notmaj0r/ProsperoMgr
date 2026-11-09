// The PS5 browser has no emoji font, so the few emoji in our texts are drawn as small inline images.
const EMOJI = /(❤️?|⭐|🫶|🇧🇷)/

const heart = (
  <path d="M12 21s-8-5.2-8-11a4.6 4.6 0 0 1 8-3 4.6 4.6 0 0 1 8 3c0 5.8-8 11-8 11z" fill="#e5484d" />
)

const ART = {
  '❤': heart,
  '❤️': heart,
  '🫶': heart,
  '⭐': (
    <path d="M12 2.5l2.9 6 6.6.9-4.8 4.6 1.2 6.5-5.9-3.1-5.9 3.1 1.2-6.5L2.5 9.4l6.6-.9z" fill="#f5c211" />
  ),
  '🇧🇷': (
    <>
      <rect x="1" y="4" width="22" height="16" rx="2" fill="#2f9e44" />
      <path d="M12 6.5L20 12l-8 5.5L4 12z" fill="#f5c211" />
      <circle cx="12" cy="12" r="3.2" fill="#1c5fc4" />
    </>
  ),
}

export default function EmojiText({ text }) {
  return text.split(EMOJI).map((part, i) => (ART[part]
    ? (
      <svg key={i} aria-hidden="true" viewBox="0 0 24 24" width="1.15em" height="1.15em" style={{ display: 'inline-block', verticalAlign: '-0.2em' }}>
        {ART[part]}
      </svg>
    )
    : part))
}
