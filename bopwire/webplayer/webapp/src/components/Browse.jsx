import { useCallback, useRef, useState } from 'react'
import CoverArt from './CoverArt'
import ArtistArt from './ArtistArt'
import TrackList from './TrackList'
import { avail, fetchSongs } from '../api'
import { seedFromHash, seedFromName, tileGradient } from '../art'
import { bucketByNorm, normKey, albumLabelOf } from '../util'

/// The facet drill (Artist/Genre → Album → tracks) in the same visual
/// language as Home: cover-art cards for artists/albums, gradient tiles for
/// genres. Fed by /api/facets + per-facet /api/songs pages — the catalog
/// never ships whole.
///
/// Responsive: on ≥sm the grid and the album track pane share a resizable
/// vertical split; on phones the open album takes the whole view with a
/// back button (a tiny split pane is useless at 375px).
export default function Browse({ facets, playingHash, onPlayTracks, onBlocked, onError }) {
  const [mode, setMode] = useState('artist')       // 'artist' | 'genre'
  const [path, setPath] = useState([])             // [{key,label}]
  const [album, setAlbum] = useState(null)         // {key,label}
  const [songs, setSongs] = useState([])           // fetched facet page
  const [loading, setLoading] = useState(false)
  const [topPct, setTopPct] = useState(48)         // draggable split (desktop)
  const panesRef = useRef(null)

  const reset = (m) => { setMode(m); setPath([]); setAlbum(null); setSongs([]) }

  const drillFetch = useCallback(async (param, value) => {
    setLoading(true)
    try {
      setSongs(await fetchSongs({ [param]: value, limit: 500, sort: 'album' }))
    } catch (e) {
      setSongs([])
      onError?.('Could not load songs: ' + e.message)
    }
    setLoading(false)
  }, [onError])

  const levelKind = mode === 'artist'
    ? (path.length === 0 ? 'artist' : 'album')
    : ['genre', 'artist', 'album'][Math.min(path.length, 2)]

  const songsForPath = () => {
    let arr = songs
    if (mode === 'genre' && path[1]) arr = arr.filter((s) => normKey(s.artist) === path[1].key)
    return arr
  }

  // ---- bucket data for the current level --------------------------------
  let buckets
  if (levelKind === 'artist' && mode === 'artist') {
    buckets = (facets?.artists || []).map((a) => ({ key: normKey(a.name), label: a.name, count: a.count, albums: a.albums || [] }))
  } else if (levelKind === 'genre') {
    buckets = (facets?.genres || []).map((g) => ({ key: normKey(g.name), label: g.name, count: g.count }))
  } else {
    const arr = songsForPath()
    const keyFn = levelKind === 'artist' ? (s) => s.artist : (s) => albumLabelOf(s)
    buckets = [...bucketByNorm(arr, keyFn).values()]
      .map((b) => ({ key: b.key, label: b.label, count: b.items.length, items: b.items }))
    if (levelKind === 'album') {
      for (const b of buckets)
        b.year = b.items.reduce((y, s) => (s.year && (!y || s.year < y)) ? s.year : y, 0)
      buckets.sort((a, b) => (a.year || 9999) - (b.year || 9999) || a.label.localeCompare(b.label))
    } else {
      buckets.sort((a, b) => (!!b.key - !!a.key) || a.label.localeCompare(b.label))
    }
  }

  const cardTap = (b) => {
    if (levelKind === 'album') {
      setAlbum(album?.key === b.key ? null : { key: b.key, label: b.label })
      return
    }
    const next = [...path, { key: b.key, label: b.label }]
    setPath(next); setAlbum(null)
    if (next.length === 1) drillFetch(mode === 'artist' ? 'artist' : 'genre', b.label)
  }

  const crumbs = [
    { label: mode === 'artist' ? 'Artists' : 'Genres',
      go: () => { setPath([]); setAlbum(null); setSongs([]) } },
    ...path.map((p, i) => ({ label: p.label, go: () => { setPath(path.slice(0, i + 1)); setAlbum(null) } })),
    ...(album ? [{ label: album.label, current: true }] : []),
  ]

  const tracks = album
    ? songsForPath()
        .filter((s) => normKey(albumLabelOf(s)) === album.key)
        .sort((a, b) => (a.trackNumber || 9999) - (b.trackNumber || 9999)
                     || a.title.localeCompare(b.title))
    : []

  const onDividerMove = (e) => {
    if (e.buttons !== 1 || !panesRef.current) return
    const r = panesRef.current.getBoundingClientRect()
    setTopPct(Math.min(80, Math.max(20, ((e.clientY - r.top) / r.height) * 100)))
  }

  return (
    <div className="flex h-full flex-col">
      {/* mode toggle + breadcrumb */}
      <div className="flex flex-none flex-wrap items-center gap-x-4 gap-y-1.5 border-b border-line
          px-4 py-2.5">
        <div className="inline-flex rounded-full border border-line bg-elev2 p-[3px]">
          {['artist', 'genre'].map((m) => (
            <button key={m} onClick={() => reset(m)}
              className={`rounded-full px-3.5 py-1.5 text-sm transition-colors
                ${mode === m ? 'bg-mint font-semibold text-mintink' : 'text-dim hover:text-ink'}`}>
              {m === 'artist' ? '👤 Artist' : '🏷 Genre'}
            </button>
          ))}
        </div>
        <nav className="flex min-h-5 min-w-0 flex-wrap items-center gap-1 text-[13px] text-dim">
          {crumbs.map((c, i) => (
            <span key={i} className="flex min-w-0 items-center gap-1">
              {i > 0 && <span className="opacity-50">›</span>}
              {c.current || i === crumbs.length - 1
                ? <span className="truncate font-semibold text-ink">{c.label}</span>
                : <button className="truncate text-sky hover:underline" onClick={c.go}>{c.label}</button>}
            </span>
          ))}
        </nav>
      </div>

      {/* panes */}
      <div ref={panesRef} className="flex min-h-0 flex-1 flex-col">
        {/* facet grid — hidden on phones while an album is open */}
        <div
          className={`overflow-auto px-4 py-4 ${album ? 'max-sm:hidden' : ''}`}
          style={{ flex: album ? `0 0 ${topPct}%` : '1 1 auto' }}
        >
          <FacetGrid
            kind={levelKind} buckets={buckets} loading={loading}
            hasAny={!!facets?.total} selectedKey={album?.key}
            onTap={cardTap}
            onPlayGroup={(b) => {
              const playable = (b.items || []).filter(avail)
              playable.length ? onPlayTracks(playable, 0) : onBlocked?.()
            }}
          />
        </div>

        {album && (
          <>
            <div
              onPointerDown={(e) => e.currentTarget.setPointerCapture(e.pointerId)}
              onPointerMove={onDividerMove}
              title="Drag to resize"
              className="relative flex-none cursor-row-resize border-y border-line bg-bg py-1
                max-sm:hidden
                after:absolute after:left-1/2 after:top-1/2 after:h-[3px] after:w-10
                after:-translate-x-1/2 after:-translate-y-1/2 after:rounded after:bg-line"
            />
            <AlbumPane
              album={album} tracks={tracks} playingHash={playingHash}
              artistHint={path.length ? path[path.length - 1].label : ''}
              onClose={() => setAlbum(null)}
              onPlayTracks={onPlayTracks} onBlocked={onBlocked}
            />
          </>
        )}
      </div>
    </div>
  )
}

// ---- facet grid ----------------------------------------------------------

function FacetGrid({ kind, buckets, loading, hasAny, selectedKey, onTap, onPlayGroup }) {
  if (loading) return <GridSkeleton tiles={kind === 'genre'} />
  if (!buckets.length) {
    return (
      <p className="py-8 text-center text-dim">
        {hasAny ? 'Nothing here right now.' : 'No streamable songs on the network yet.'}
      </p>
    )
  }

  if (kind === 'genre') {
    return (
      <div className="grid grid-cols-[repeat(auto-fill,minmax(8.25rem,1fr))] gap-3">
        {buckets.map((b) => (
          <button key={b.key} onClick={() => onTap(b)}
            className="animate-rise relative flex h-[4.5rem] items-end overflow-hidden rounded-xl
              border border-white/10 p-2.5 text-left font-extrabold text-white
              [text-shadow:0_1px_6px_rgba(0,0,0,.6)] transition
              hover:-translate-y-0.5 hover:border-white/25"
            style={{ background: tileGradient(b.label) }}>
            <span className="absolute right-2 top-2 rounded-full bg-black/35 px-2 py-0.5
                text-[10px] font-bold">{b.count}</span>
            <span className="text-[13px] leading-tight">{b.label}</span>
          </button>
        ))}
      </div>
    )
  }

  // artist / album cover-art cards
  return (
    <div className="grid grid-cols-[repeat(auto-fill,minmax(7.75rem,1fr))] gap-3
        sm:grid-cols-[repeat(auto-fill,minmax(8.75rem,1fr))]">
      {buckets.map((b) => {
        const seed = kind === 'album' && b.items?.length
          ? seedFromHash(b.items[0].contentHash)
          : seedFromName(b.label)
        // Album cards get their real cover; artist cards cycle through the
        // artist's album covers (see ArtistArt below).
        const artArtist = kind === 'album' ? (b.items?.[0]?.artist || '') : ''
        const artistAlbums = kind === 'artist'
          ? (b.albums?.length
              ? b.albums
              : [...new Set((b.items || [])
                  .map((s) => (s.album || '').trim()).filter(Boolean))])
          : []
        const selected = selectedKey && selectedKey === b.key
        const sub = kind === 'album'
          ? `${b.year ? b.year + ' · ' : ''}${b.count} track${b.count === 1 ? '' : 's'}`
          : `${b.count} track${b.count === 1 ? '' : 's'}`
        return (
          <div key={b.key || b.label}
            role="button" tabIndex={0}
            onClick={() => onTap(b)}
            onKeyDown={(e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); onTap(b) } }}
            className={`animate-rise group cursor-pointer rounded-xl border p-2.5 text-left
              transition hover:-translate-y-1 hover:bg-elev2
              ${selected ? 'border-mint bg-elev' : 'border-transparent bg-elev hover:border-line'}`}>
            <div className="relative mb-2 aspect-square w-full overflow-hidden rounded-lg
                shadow-[0_8px_22px_rgba(0,0,0,.45)]">
              {kind === 'artist' && artistAlbums.length
                ? <ArtistArt seed={seed} artist={b.label} albums={artistAlbums} className="size-full" />
                : <CoverArt seed={seed} artist={artArtist} album={b.label} className="size-full" />}
              {b.items?.length > 0 && (
                <button
                  aria-label={`Play ${b.label}`}
                  onClick={(e) => { e.stopPropagation(); onPlayGroup(b) }}
                  className={`absolute bottom-2 right-2 grid size-9 place-items-center rounded-full
                    bg-mint text-[15px] text-mintink shadow-[0_6px_16px_rgba(0,0,0,.5)]
                    transition-all
                    ${selected ? 'opacity-100'
                      : 'translate-y-1.5 opacity-0 group-hover:translate-y-0 group-hover:opacity-100'}`}
                >▶</button>
              )}
            </div>
            <p className="truncate text-[13.5px] font-bold">{b.label}</p>
            <p className="truncate text-xs text-dim">{sub}</p>
          </div>
        )
      })}
    </div>
  )
}

function GridSkeleton({ tiles }) {
  return tiles ? (
    <div className="grid grid-cols-[repeat(auto-fill,minmax(8.25rem,1fr))] gap-3" aria-busy="true">
      {Array.from({ length: 8 }, (_, i) => <div key={i} className="skel h-[4.5rem] rounded-xl" />)}
    </div>
  ) : (
    <div className="grid grid-cols-[repeat(auto-fill,minmax(8.75rem,1fr))] gap-3" aria-busy="true">
      {Array.from({ length: 10 }, (_, i) => (
        <div key={i}>
          <div className="skel aspect-square w-full rounded-lg" />
          <div className="skel mt-2 h-3.5 w-4/5" />
          <div className="skel mt-1.5 h-3 w-3/5" />
        </div>
      ))}
    </div>
  )
}

// ---- album track pane ------------------------------------------------------

function AlbumPane({ album, tracks, playingHash, artistHint, onClose, onPlayTracks, onBlocked }) {
  const artist = (tracks.find((s) => s.artist?.trim())?.artist) || artistHint || ''
  const year = tracks.reduce((y, s) => s.year || y, 0)
  const playable = tracks.filter(avail)
  const seed = tracks.length ? seedFromHash(tracks[0].contentHash) : seedFromName(album.label)

  return (
    <div className="flex min-h-0 flex-1 flex-col">
      <div className="flex flex-none items-center gap-3.5 border-b border-line bg-elev/60 px-4
          py-3 backdrop-blur-sm">
        <button onClick={onClose} aria-label="Back"
          className="grid size-8 flex-none place-items-center rounded-full border border-line
            text-dim transition-colors hover:border-dim hover:text-ink sm:hidden">←</button>
        <CoverArt seed={seed} artist={artist} album={album.label}
            className="size-14 flex-none overflow-hidden rounded-lg
            shadow-[0_8px_22px_rgba(0,0,0,.45)] sm:size-16" />
        <div className="min-w-0 flex-1">
          <p className="truncate text-[15px] font-extrabold sm:text-base">{album.label}</p>
          <p className="truncate text-xs text-dim">
            {[artist, year > 0 ? String(year) : '',
              `${tracks.length} track${tracks.length === 1 ? '' : 's'}`]
              .filter(Boolean).join(' · ')}
          </p>
        </div>
        <button
          disabled={!playable.length}
          onClick={() => onPlayTracks(playable, 0)}
          className="rounded-full bg-mint px-4 py-1.5 text-sm font-extrabold text-mintink
            transition-transform hover:scale-105 disabled:opacity-40 max-sm:px-3"
        >▶ <span className="max-sm:hidden">Play all</span></button>
        <button
          disabled={!playable.length}
          aria-label="Shuffle"
          onClick={() => onPlayTracks([...playable].sort(() => Math.random() - 0.5), 0)}
          className="grid size-8 flex-none place-items-center rounded-full border border-line
            text-sm text-dim transition-colors hover:border-sky hover:text-sky disabled:opacity-40"
        >🔀</button>
        <button onClick={onClose} aria-label="Close"
          className="hidden size-8 flex-none place-items-center rounded-full border border-line
            text-dim transition-colors hover:border-dim hover:text-ink sm:grid">✕</button>
      </div>
      <div className="min-h-0 flex-1 overflow-auto px-3 pb-3 pt-1.5 sm:px-4">
        <TrackList tracks={tracks} numbered playingHash={playingHash}
          onPlay={(s, i) => onPlayTracks(tracks, i)} onBlocked={onBlocked} />
      </div>
    </div>
  )
}
