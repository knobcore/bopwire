import CoverArt from './CoverArt'
import { songSeed } from '../art'
import { avail } from '../api'
import { fmtDur, fmtPlays } from '../util'

/// Shared track rows (Browse album pane, List view, search results).
/// Tapping a row queues the whole list's playable subset from that track.
export default function TrackList({ tracks, numbered, playingHash, onPlay, onBlocked }) {
  return (
    <div className="flex flex-col">
      {tracks.map((s, i) => {
        const off = !avail(s)
        const playing = playingHash === s.contentHash
        return (
          <button
            key={s.contentHash + i}
            onClick={() => (off ? onBlocked?.() : onPlay(s, i))}
            className={`group grid w-full cursor-pointer grid-cols-[28px_36px_1fr_auto_auto_32px]
              items-center gap-3 rounded-lg px-2 py-1.5 text-left transition-colors hover:bg-elev
              max-sm:grid-cols-[22px_36px_1fr_auto_26px] max-sm:gap-2.5
              ${playing ? 'bg-mint/10' : ''} ${off ? 'opacity-45' : ''}`}
          >
            <span className="text-right text-xs tabular-nums text-dim">
              {numbered ? (s.trackNumber || i + 1) : i + 1}
            </span>
            <CoverArt seed={songSeed(s)} artist={s.artist} album={s.album}
              className="size-9 overflow-hidden rounded-md" />
            <span className="min-w-0">
              <span className={`block truncate text-sm ${playing ? 'font-semibold text-mint' : ''}`}>
                {s.title || '(untitled)'}
              </span>
              <span className="block truncate text-xs text-dim">{s.artist}</span>
            </span>
            <span className="whitespace-nowrap text-xs text-dim max-sm:hidden">
              {fmtPlays(s.playCount)} plays
            </span>
            <span className="text-xs tabular-nums text-dim">{fmtDur(s.durationMs)}</span>
            <span className={`text-center text-mint transition-opacity
              ${playing ? 'opacity-100' : 'opacity-0 group-hover:opacity-100'}`}>
              {off ? '' : playing ? '♫' : '▶'}
            </span>
          </button>
        )
      })}
    </div>
  )
}
