import { useEffect, useState } from 'react'

// Fades the image in once it has loaded, so thumbnails and covers do not pop in one by one.
export default function FadeImage({ src, className = '', onLoad, ...props }) {
  const [loaded, setLoaded] = useState(false)
  useEffect(() => setLoaded(false), [src])
  return (
    <img
      src={src}
      className={`pmgr-img-fade ${loaded ? 'is-loaded' : ''} ${className}`}
      onLoad={(e) => { setLoaded(true); if (onLoad) onLoad(e) }}
      {...props}
    />
  )
}
