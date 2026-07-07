import { useMemo } from 'react'
import { coverArtSvg } from '../art'
import { artUrl } from '../api'

/// Deterministic generated cover with an optional real-art overlay. `seed` = 8
/// bytes (see art.js). When `artist` is given, the node-scraped cover is layered
/// on top via <img>; on a 404/error it removes itself and the generated art
/// shows through — zero-flash, no state, identical to before on a miss.
export default function CoverArt({ seed, artist, album, className = '' }) {
  const svg = useMemo(() => coverArtSvg(seed), [seed.join(',')])
  return (
    <div className={`relative ${className}`}>
      <div
        className="absolute inset-0 [&>svg]:block [&>svg]:size-full"
        dangerouslySetInnerHTML={{ __html: svg }}
      />
      {artist ? (
        <img
          src={artUrl(artist, album)}
          alt=""
          loading="lazy"
          className="absolute inset-0 size-full object-cover"
          onError={(e) => e.currentTarget.remove()}
        />
      ) : null}
    </div>
  )
}
