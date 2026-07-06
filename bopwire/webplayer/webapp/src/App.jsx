import { useCallback, useEffect, useRef, useState, useSyncExternalStore } from 'react'
import { TopBar, CtaFooter, Toast } from './components/Chrome'
import Home from './components/Home'
import Browse from './components/Browse'
import ListView from './components/ListView'
import NowPlaying from './components/NowPlaying'
import { fetchCollections, fetchFacets, fetchSongs, avail } from './api'
import { seedFromName } from './art'
import { player } from './player'
import { REFRESH_MS, COLLECTIONS_MS } from './config'

export default function App() {
  const [view, setViewRaw] = useState('home')       // 'home' | 'browse' | 'list'
  const [lastView, setLastView] = useState('home')
  const [list, setList] = useState(null)            // {kicker,title,sub,songs,seed}
  const [facets, setFacets] = useState(null)
  const [collections, setCollections] = useState(null)
  const [status, setStatus] = useState({ kind: 'warn', text: 'connecting…' })
  const [query, setQuery] = useState('')
  const [toast, setToast] = useState(null)
  const toastTimer = useRef(null)

  const playerState = useSyncExternalStore(player.subscribe, player.getSnapshot)
  const playingHash = playerState.playingHash

  const showToast = useCallback((msg) => {
    setToast(msg)
    clearTimeout(toastTimer.current)
    toastTimer.current = setTimeout(() => setToast(null), 3200)
  }, [])

  // stream errors from the engine surface as toasts
  useEffect(() => {
    if (playerState.err) { showToast(playerState.err); player.clearError() }
  }, [playerState.err, showToast])

  // Animated view switches via the View Transitions API (graceful no-op
  // where unsupported or when the user prefers reduced motion).
  const transition = (fn) => {
    if (document.startViewTransition &&
        !matchMedia('(prefers-reduced-motion: reduce)').matches) {
      document.startViewTransition(fn)
    } else fn()
  }

  const setView = (v) => transition(() => {
    setViewRaw(v)
    if (v !== 'list') setLastView(v)
  })

  const openList = (payload) => transition(() => {
    setList(payload)
    setViewRaw('list')
  })

  // ---- data ----------------------------------------------------------
  useEffect(() => {
    let alive = true
    const loadFacets = async () => {
      try {
        const f = await fetchFacets()
        if (!alive) return
        setFacets(f)
        setStatus({ kind: 'ok', text: `${f.total || 0} songs online` })
      } catch {
        if (alive) setStatus({ kind: 'err', text: 'gateway unreachable' })
      }
    }
    const loadCollections = async () => {
      try { const c = await fetchCollections(); if (alive) setCollections(c) }
      catch { /* home shows skeleton/empty; facets drive the status pill */ }
    }
    loadFacets(); loadCollections()
    const t1 = setInterval(loadFacets, REFRESH_MS)
    const t2 = setInterval(loadCollections, Math.max(REFRESH_MS, COLLECTIONS_MS))
    return () => { alive = false; clearInterval(t1); clearInterval(t2) }
  }, [])

  // ---- search (server-side, debounced) ---------------------------------
  useEffect(() => {
    const q = query.trim()
    if (!q) {
      if (view === 'list' && list?.kicker === 'Search') setView(lastView)
      return
    }
    const t = setTimeout(async () => {
      try {
        const songs = await fetchSongs({ q, limit: 200, sort: 'plays' })
        openList({
          kicker: 'Search',
          title: `“${q}”`,
          sub: `${songs.length} result${songs.length === 1 ? '' : 's'}`,
          songs,
          seed: seedFromName(q),
        })
      } catch (e) { showToast('Search failed: ' + e.message) }
    }, 250)
    return () => clearTimeout(t)
  }, [query]) // eslint-disable-line react-hooks/exhaustive-deps

  // ---- playback wiring ----------------------------------------------------
  const blocked = () => showToast('No seeders online for this track right now.')

  // Queue = the collection's ordered songs minus unseeded members
  // (kept visible, dimmed, but unplayable).
  const playCollection = (c, idx) => {
    const playable = (c.songs || []).filter(avail)
    if (!playable.length) { showToast('Nothing in this row is streamable right now.'); return }
    const want = c.songs[idx]
    let qi = playable.findIndex((s) => s.contentHash === want.contentHash)
    if (qi < 0) qi = 0
    player.playQueue(playable, qi)
  }

  const playTracks = (tracks, idx) => {
    const playable = tracks.filter(avail)
    if (!playable.length) { showToast('Nothing here is streamable right now.'); return }
    const want = tracks[idx]
    let qi = playable.findIndex((s) => s.contentHash === want?.contentHash)
    if (qi < 0) qi = 0
    player.playQueue(playable, qi)
  }

  const openCollection = (c) => openList({
    kicker: c.kind === 'genre' ? 'Genre' : c.kind === 'year' ? 'Year' : 'Collection',
    title: c.title,
    sub: `${c.subtitle ? c.subtitle + ' · ' : ''}${(c.songs || []).length} songs`,
    songs: c.songs || [],
    seed: seedFromName(c.facet || c.id),
  })

  return (
    <div className="flex h-full flex-col">
      <TopBar
        view={view} status={status} query={query}
        onQuery={setQuery}
        onView={(v) => { setQuery(''); setView(v) }}
      />

      <main className="min-h-0 flex-1">
        {view === 'home' && (
          <Home collections={collections} playingHash={playingHash}
            onPlayCollection={playCollection}
            onOpenCollection={openCollection}
            onBlocked={blocked} />
        )}
        {view === 'browse' && (
          <Browse facets={facets} playingHash={playingHash}
            onPlayTracks={playTracks} onBlocked={blocked} onError={showToast} />
        )}
        {view === 'list' && (
          <ListView list={list} playingHash={playingHash}
            onBack={() => setView(lastView)}
            onPlayTracks={playTracks} onBlocked={blocked} />
        )}
      </main>

      <NowPlaying />
      <CtaFooter />
      <Toast msg={toast} />
    </div>
  )
}
