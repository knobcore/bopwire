import { useState, useSyncExternalStore } from 'react'
import CoverArt from './CoverArt'
import { player } from '../player'
import { songSeed, accentFor } from '../art'
import { fmtDur } from '../util'

/// Persistent player bar. The accent color is derived from the current
/// track's generated cover art, so the bar re-tints per song.
export default function NowPlaying() {
  const st = useSyncExternalStore(player.subscribe, player.getSnapshot)
  const [dragFrac, setDragFrac] = useState(null)

  if (!st.song) return null
  const s = st.song
  const { a1 } = accentFor(songSeed(s))

  const frac = dragFrac ?? (st.durSec ? st.curSec / st.durSec : 0)
  const curLabel = dragFrac != null && st.durSec
    ? fmtDur(dragFrac * st.durSec * 1000)
    : fmtDur(st.curSec * 1000)

  return (
    <div
      className="flex h-[4.5rem] flex-none items-center gap-3.5 border-t border-line
        bg-elev/85 px-3 backdrop-blur-md sm:px-4"
      style={{ '--seek-fill': a1, boxShadow: `0 -1px 24px color-mix(in oklab, ${a1} 12%, transparent)` }}
    >
      <CoverArt seed={songSeed(s)} artist={s.artist} album={s.album}
        className="size-11 shrink-0 overflow-hidden rounded-lg" />
      <div className="w-32 min-w-0 sm:w-52">
        <p className="truncate text-sm font-semibold">{s.title || '(untitled)'}</p>
        <p className="truncate text-xs text-dim">{s.artist}</p>
      </div>

      <div className="flex min-w-0 flex-1 items-center gap-3">
        <button onClick={player.prev} aria-label="Previous"
          className="hidden size-8 rounded-full border border-line text-sm text-dim
            transition-colors hover:border-dim hover:text-ink sm:block">⏮</button>
        <button onClick={player.toggle} aria-label="Play/Pause"
          className="grid size-10 flex-none place-items-center rounded-full text-[15px]
            font-bold text-mintink transition-transform hover:scale-105"
          style={{ background: a1 }}>
          {st.paused ? '▶' : '⏸'}
        </button>
        <button onClick={player.next} aria-label="Next"
          className="hidden size-8 rounded-full border border-line text-sm text-dim
            transition-colors hover:border-dim hover:text-ink sm:block">⏭</button>

        <div className="flex min-w-0 flex-1 items-center gap-2.5 text-xs text-dim">
          <span className="tabular-nums">{curLabel}</span>
          <input
            type="range" min="0" max="1000" step="1"
            className="seek min-w-0 flex-1"
            style={{ '--seek-pct': `${Math.round(frac * 1000) / 10}%` }}
            value={Math.round(frac * 1000)}
            onChange={(e) => {
              const f = +e.target.value / 1000
              setDragFrac(f)
              player.seekLive(f)   // cheap for <audio>; WASM commits on release
            }}
            onPointerUp={(e) => {
              player.seekCommit(+e.currentTarget.value / 1000)
              setDragFrac(null)
            }}
            onKeyUp={(e) => {
              player.seekCommit(+e.currentTarget.value / 1000)
              setDragFrac(null)
            }}
          />
          <span className="tabular-nums">{fmtDur(st.durSec * 1000)}</span>
        </div>
      </div>

      {st.spinning && (
        <span className="size-4 flex-none animate-spin rounded-full border-2 border-line"
          style={{ borderTopColor: a1 }} />
      )}
    </div>
  )
}
