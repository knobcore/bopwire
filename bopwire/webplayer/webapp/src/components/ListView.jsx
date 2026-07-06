import CoverArt from './CoverArt'
import TrackList from './TrackList'
import { avail } from '../api'

/// "See all" for a collection / server-side search results.
export default function ListView({ list, playingHash, onBack, onPlayTracks, onBlocked }) {
  if (!list) return null
  const playable = list.songs.filter((s) => avail(s))
  return (
    <div className="h-full overflow-y-auto px-4 pb-10 pt-4 sm:px-6">
      <div className="animate-rise mb-4 flex items-end gap-5">
        <CoverArt seed={list.seed} className="size-24 shrink-0 overflow-hidden rounded-xl
            shadow-[0_12px_32px_rgba(0,0,0,.5)] sm:size-32" />
        <div className="min-w-0 flex-1">
          <p className="mb-1 text-[11px] font-extrabold uppercase tracking-[.14em] text-mint">
            {list.kicker}
          </p>
          <h1 className="truncate text-xl font-extrabold leading-tight sm:text-2xl">{list.title}</h1>
          <p className="mb-3 mt-1 text-[13px] text-dim">{list.sub}</p>
          <div className="flex items-center gap-2.5">
            <button
              disabled={!playable.length}
              onClick={() => onPlayTracks(playable, 0)}
              className="rounded-full bg-mint px-5 py-2 text-sm font-extrabold text-mintink
                transition-transform hover:scale-105 disabled:opacity-40"
            >▶ Play all</button>
            <button
              disabled={!playable.length}
              onClick={() => onPlayTracks([...playable].sort(() => Math.random() - 0.5), 0)}
              className="rounded-full border border-line px-4 py-2 text-sm font-semibold
                transition-colors hover:border-sky hover:text-sky disabled:opacity-40"
            >🔀 Shuffle</button>
          </div>
        </div>
        <button onClick={onBack}
          className="self-start rounded-full border border-line px-4 py-2 text-sm font-semibold
            transition-colors hover:border-sky hover:text-sky">← Back</button>
      </div>
      {list.songs.length === 0
        ? <p className="py-8 text-center text-dim">Nothing here.</p>
        : <TrackList tracks={list.songs} numbered={false} playingHash={playingHash}
            onPlay={(s, i) => {
              const qi = playable.findIndex((p) => p.contentHash === s.contentHash)
              onPlayTracks(playable, Math.max(0, qi))
            }}
            onBlocked={onBlocked} />}
    </div>
  )
}
