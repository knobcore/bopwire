import { useMemo } from 'react'
import { coverArtSvg } from '../art'

/// Deterministic generated cover. `seed` = 8 bytes (see art.js).
export default function CoverArt({ seed, className = '' }) {
  const svg = useMemo(() => coverArtSvg(seed), [seed.join(',')])
  return (
    <div
      className={`[&>svg]:block [&>svg]:size-full ${className}`}
      dangerouslySetInnerHTML={{ __html: svg }}
    />
  )
}
