import { useEffect, useMemo, useState } from 'react'
import { coverArtSvg } from '../art'
import { artUrl } from '../api'

const MAX = 6          // covers to probe per artist (bounded fan-out)
const PERIOD = 6000    // ms between crossfades

/// Artist thumbnail: probes the artist's album covers and crossfades through the
/// ones that exist, so an artist tile shows real album art instead of the
/// generated motif. Generated art (the SVG base) shows through until/unless any
/// cover loads, so a node with no covers looks exactly as before.
export default function ArtistArt({ seed, artist, albums = [], className = '' }) {
  const svg = useMemo(() => coverArtSvg(seed), [seed.join(',')])
  const [covers, setCovers] = useState([])
  const [idx, setIdx] = useState(0)

  const key = albums.join('|')
  useEffect(() => {
    setCovers([])
    setIdx(0)
    if (!artist) return
    const seen = new Set()
    const uniq = []
    for (const a of albums) {
      const t = (a || '').trim()
      if (!t || seen.has(t.toLowerCase())) continue
      seen.add(t.toLowerCase())
      uniq.push(t)
      if (uniq.length >= MAX) break
    }
    let alive = true
    const loaded = []
    for (const album of uniq) {
      const url = artUrl(artist, album)
      const img = new Image()
      img.onload = () => { if (alive) { loaded.push(url); setCovers([...loaded]) } }
      img.src = url   // 404 (miss) → never loads → excluded
    }
    return () => { alive = false }
  }, [artist, key])

  useEffect(() => {
    if (covers.length < 2) return
    const t = setInterval(() => setIdx((i) => (i + 1) % covers.length), PERIOD)
    return () => clearInterval(t)
  }, [covers.length])

  const cur = covers.length ? idx % covers.length : 0
  return (
    <div className={`relative ${className}`}>
      <div
        className="absolute inset-0 [&>svg]:block [&>svg]:size-full"
        dangerouslySetInnerHTML={{ __html: svg }}
      />
      {covers.map((url, i) => (
        <img
          key={url}
          src={url}
          alt=""
          className="absolute inset-0 size-full object-cover transition-opacity duration-700"
          style={{ opacity: i === cur ? 1 : 0 }}
        />
      ))}
    </div>
  )
}
