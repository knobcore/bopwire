import { useRef } from 'react'
import CoverArt from './CoverArt'
import { songSeed, seedFromName, tileGradient } from '../art'
import { avail } from '../api'
import { fmtPlays } from '../util'

const KIND_ORDER = ['rising', 'top', 'new', 'year', 'genre']

export default function Home({ collections, playingHash, onPlayCollection, onOpenCollection, onBlocked }) {
  const cols = (collections?.collections || []).filter((c) => (c.songs || []).length > 0)

  if (!cols.length) return <HomeSkeleton loaded={!!collections} />

  const rows = [...cols].sort((a, b) => KIND_ORDER.indexOf(a.kind) - KIND_ORDER.indexOf(b.kind))
  const genres = rows.filter((c) => c.kind === 'genre')
  const rising = rows.find((c) => c.kind === 'rising') || rows[0]

  return (
    <div className="h-full overflow-y-auto px-4 pb-10 pt-4 sm:px-6">
      <Hero collection={rising} onPlay={onPlayCollection} onMore={onOpenCollection} />

      {genres.length > 0 && (
        <section className="mt-6">
          <h2 className="mb-2.5 px-0.5 text-[15px] font-extrabold tracking-wide">Genres</h2>
          <div className="rail-mask grid snap-x snap-proximity auto-cols-[8.5rem] grid-flow-col
              gap-3 overflow-x-auto pb-2">
            {genres.map((c) => (
              <button
                key={c.id}
                onClick={() => onOpenCollection(c)}
                className="relative flex h-[4.5rem] snap-start items-end overflow-hidden rounded-xl
                  border border-white/10 p-2.5 text-left font-extrabold text-white
                  [text-shadow:0_1px_6px_rgba(0,0,0,.6)] transition
                  hover:-translate-y-0.5 hover:border-white/25"
                style={{ background: tileGradient(c.facet) }}
              >
                <span className="absolute right-2 top-2 rounded-full bg-black/35 px-2 py-0.5
                    text-[10px] font-bold">{c.songs.length}</span>
                <span className="text-[13px] leading-tight">{c.title.replace(/^Best of /, '')}</span>
              </button>
            ))}
          </div>
        </section>
      )}

      {rows.map((c, i) => (
        <Carousel
          key={c.id}
          collection={c}
          playingHash={playingHash}
          onPlay={onPlayCollection}
          onMore={onOpenCollection}
          onBlocked={onBlocked}
          style={{ animationDelay: `${Math.min(i, 8) * 45}ms` }}
        />
      ))}

      {collections?.contentDigest && (
        <p className="mt-8 text-center text-[11px] text-dim/80">
          curated deterministically by the network · epoch {collections.epoch} · digest{' '}
          <code className="text-[10.5px] text-sky">{String(collections.contentDigest).slice(0, 16)}…</code>
        </p>
      )}
    </div>
  )
}

function Hero({ collection, onPlay, onMore }) {
  const songs = collection.songs || []
  const idx = Math.max(0, songs.findIndex((s) => avail(s)))
  const s = songs[idx]
  if (!s) return null
  return (
    <section className="animate-rise relative flex flex-col items-start gap-5 overflow-hidden
        rounded-2xl border border-line bg-elev p-5 sm:flex-row sm:items-center sm:p-7
        bg-[radial-gradient(900px_280px_at_8%_-40%,color-mix(in_oklab,var(--color-mint)_16%,transparent),transparent_60%),radial-gradient(760px_300px_at_92%_140%,color-mix(in_oklab,var(--color-sky)_14%,transparent),transparent_60%)]">
      <CoverArt seed={songSeed(s)} className="size-28 shrink-0 overflow-hidden rounded-xl
          shadow-[0_14px_40px_rgba(0,0,0,.55)] sm:size-36" />
      <div className="min-w-0">
        <p className="mb-1.5 text-[11px] font-extrabold uppercase tracking-[.14em] text-mint">
          ✦ Rising — every listen pays the artist in full
        </p>
        <h1 className="truncate text-xl font-extrabold leading-tight sm:text-[26px]">
          {s.title || '(untitled)'}
        </h1>
        <p className="mb-3.5 text-sm text-dim">
          {s.artist}{s.playCount ? ` · ${fmtPlays(s.playCount)} plays` : ''}
        </p>
        <div className="flex items-center gap-2.5">
          <button
            onClick={() => onPlay(collection, idx)}
            className="rounded-full bg-mint px-5 py-2 text-sm font-extrabold text-mintink
              transition-transform hover:scale-105"
          >▶ Play</button>
          <button
            onClick={() => onMore(collection)}
            className="rounded-full border border-line px-4 py-2 text-sm font-semibold
              transition-colors hover:border-sky hover:text-sky"
          >Explore Rising</button>
        </div>
      </div>
      <span className="absolute right-4 top-3.5 rounded-full border border-line bg-bg/55
          px-2.5 py-0.5 text-[11px] text-dim">under 10k plays</span>
    </section>
  )
}

function Carousel({ collection: c, playingHash, onPlay, onMore, onBlocked, style }) {
  const rail = useRef(null)
  const nudge = (dir) =>
    rail.current?.scrollBy({ left: dir * rail.current.clientWidth * 0.8, behavior: 'smooth' })

  return (
    <section className="animate-rise group/row mt-6" style={style}>
      <div className="mb-2 flex items-baseline gap-3 px-0.5">
        <h2 className="bg-gradient-to-r from-mint to-sky bg-clip-text text-[17px]
            font-extrabold tracking-wide text-transparent">{c.title}</h2>
        <p className="min-w-0 flex-1 truncate text-xs text-dim">{c.subtitle}</p>
        <button onClick={() => onMore(c)}
          className="whitespace-nowrap text-[12.5px] font-bold text-sky hover:underline">
          See all ›
        </button>
      </div>
      <div className="relative">
        <div ref={rail} className="rail-mask grid snap-x snap-proximity auto-cols-[9.5rem]
            grid-flow-col gap-3.5 overflow-x-auto px-0.5 pb-3 pt-1 max-sm:auto-cols-[8.25rem]">
          {(c.songs || []).slice(0, 20).map((s, i) => (
            <SongCard key={s.contentHash} song={s} playing={playingHash === s.contentHash}
              onClick={() => (avail(s) ? onPlay(c, i) : onBlocked?.())} />
          ))}
        </div>
        <RailArrow dir={-1} onClick={() => nudge(-1)} />
        <RailArrow dir={1} onClick={() => nudge(1)} />
      </div>
    </section>
  )
}

function RailArrow({ dir, onClick }) {
  return (
    <button
      onClick={onClick}
      aria-label={dir < 0 ? 'Scroll left' : 'Scroll right'}
      className={`absolute top-[38%] z-10 hidden size-9 -translate-y-1/2 place-items-center
        rounded-full border border-line bg-elev2/90 text-sm text-ink opacity-0 shadow-lg
        backdrop-blur transition-opacity hover:border-sky
        group-hover/row:opacity-100 pointer-fine:grid
        ${dir < 0 ? 'left-1' : 'right-1'}`}
    >{dir < 0 ? '‹' : '›'}</button>
  )
}

function SongCard({ song: s, playing, onClick }) {
  const off = !avail(s)
  const rising = (s.playCount || 0) > 0 && (s.playCount || 0) < 10000
  return (
    <button
      onClick={onClick}
      className={`group snap-start rounded-xl border p-2.5 text-left transition
        hover:-translate-y-1 hover:bg-elev2
        ${playing ? 'border-mint bg-elev' : 'border-transparent bg-elev hover:border-line'}
        ${off ? 'opacity-45' : ''}`}
    >
      <div className="relative mb-2 aspect-square w-full overflow-hidden rounded-lg
          shadow-[0_8px_22px_rgba(0,0,0,.45)]">
        <CoverArt seed={songSeed(s)} className="size-full" />
        {off ? (
          <span className="absolute left-1.5 top-1.5 rounded-full border border-line bg-bg/75
              px-2 py-0.5 text-[10px] font-bold">offline</span>
        ) : (
          <span className={`absolute bottom-2 right-2 grid size-9 place-items-center rounded-full
              bg-mint text-[15px] text-mintink shadow-[0_6px_16px_rgba(0,0,0,.5)] transition-all
              ${playing ? 'opacity-100' : 'translate-y-1.5 opacity-0 group-hover:translate-y-0 group-hover:opacity-100'}`}>
            {playing ? '♫' : '▶'}
          </span>
        )}
      </div>
      <p className="truncate text-[13.5px] font-bold">{s.title || '(untitled)'}</p>
      <p className="truncate text-xs text-dim">{s.artist}</p>
      <p className="mt-1 flex items-center gap-1.5 text-[11px] text-dim">
        {rising && <span className="size-1.5 rounded-full bg-mint" />}
        {fmtPlays(s.playCount)} plays
      </p>
    </button>
  )
}

function HomeSkeleton({ loaded }) {
  if (loaded) {
    return (
      <div className="grid h-full place-items-center px-8 text-center text-dim">
        The network hasn’t curated anything yet — check back soon.
      </div>
    )
  }
  return (
    <div className="h-full overflow-y-auto px-4 pb-10 pt-4 sm:px-6" aria-busy="true">
      <div className="skel h-44 rounded-2xl sm:h-52" />
      {[0, 1].map((r) => (
        <div key={r} className="mt-7">
          <div className="skel mb-3 h-5 w-40" />
          <div className="grid auto-cols-[9.5rem] grid-flow-col gap-3.5 overflow-hidden">
            {Array.from({ length: 8 }, (_, i) => (
              <div key={i}>
                <div className="skel aspect-square w-full rounded-lg" />
                <div className="skel mt-2 h-3.5 w-4/5" />
                <div className="skel mt-1.5 h-3 w-3/5" />
              </div>
            ))}
          </div>
        </div>
      ))}
    </div>
  )
}
