// Playback controller — framework-agnostic singleton the React tree
// subscribes to via useSyncExternalStore. The engine + reward logic is
// ported VERBATIM from the legacy frontend's app.js:
//
//  * Two engines: the WASM decoder (low click-to-play latency, mp3/flac/ogg,
//    global window.WasmPlayer from public/wasm-player.js) and the native
//    <audio> element as fallback for anything WASM can't decode, with a 5 s
//    no-sound watchdog.
//  * Reward session: the play earns for the artist/seeder/mini, never the
//    listener. The browser reports REAL playback progress (start /
//    5 s heartbeat / complete, sendBeacon on tab close) so a mint only lands
//    on a genuine listen — same lifecycle as the native player.
import { apiPost, streamUrl } from './api'
import { GATEWAY } from './config'

class Player {
  constructor() {
    this.audio = document.createElement('audio')
    this.audio.preload = 'none'
    this.wasm = window.WasmPlayer ? new window.WasmPlayer() : null
    this.engine = 'audio'

    this.queue = []
    this.qIndex = -1
    this.song = null          // currently playing song object
    this.playId = null        // active reward session id (gateway)
    this.hbTimer = null
    this.spinning = false
    this.err = null           // last stream error message (for toasts)

    this.listeners = new Set()
    this.snapshot = this.buildSnapshot()

    // <audio> events only drive the UI when it's the active engine.
    const A = this.audio
    A.addEventListener('playing', () => { if (this.engine === 'audio') { this.spinning = false; this.emit() } })
    A.addEventListener('waiting', () => { if (this.engine === 'audio') { this.spinning = true; this.emit() } })
    A.addEventListener('pause',   () => { if (this.engine === 'audio') this.emit() })
    A.addEventListener('play',    () => { if (this.engine === 'audio') this.emit() })
    A.addEventListener('loadedmetadata', () => { if (this.engine === 'audio') this.emit() })
    A.addEventListener('timeupdate',     () => { if (this.engine === 'audio') this.emit() })
    A.addEventListener('ended',          () => { if (this.engine === 'audio') this.onTrackEnded() })
    A.addEventListener('error', () => {
      if (this.engine !== 'audio') return
      this.spinning = false
      if (this.song) this.err = 'Could not stream this track — no seeders online right now.'
      this.emit()
    })

    // Finalize the reward session if the tab is closed mid-play.
    window.addEventListener('pagehide', () => {
      if (this.playId && navigator.sendBeacon)
        navigator.sendBeacon(GATEWAY + '/api/play/complete',
                             JSON.stringify({ playId: this.playId }))
    })
  }

  // ---- store plumbing ---------------------------------------------------
  subscribe = (fn) => { this.listeners.add(fn); return () => this.listeners.delete(fn) }
  getSnapshot = () => this.snapshot
  emit() {
    this.snapshot = this.buildSnapshot()
    for (const fn of this.listeners) fn()
  }
  buildSnapshot() {
    return {
      song: this.song,
      playingHash: this.song?.contentHash ?? null,
      paused: this.engPaused(),
      curSec: this.engCurSec(),
      durSec: this.engDurSec(),
      spinning: this.spinning,
      hasQueue: this.queue.length > 0,
      err: this.err,
    }
  }
  clearError() { if (this.err) { this.err = null; this.emit() } }

  engPaused = () => (this.engine === 'wasm' && this.wasm ? this.wasm.paused : this.audio.paused)
  engCurSec = () => ((this.engine === 'wasm' && this.wasm ? this.wasm.currentTime : this.audio.currentTime) || 0)
  engDurSec = () => (this.engine === 'wasm' && this.wasm
    ? this.wasm.duration
    : (this.audio.duration || (this.song?.durationMs || 0) / 1000 || 0))

  // ---- public API ---------------------------------------------------------
  playQueue(queue, index) {
    this.queue = queue
    this.playFromQueue(index)
  }

  playFromQueue(i) {
    if (i < 0 || i >= this.queue.length) return
    this.qIndex = i
    this.play(this.queue[i])
  }

  next = () => this.playFromQueue(this.qIndex + 1)
  prev = () => this.playFromQueue(this.qIndex - 1)

  toggle = () => {
    if (this.engine === 'wasm' && this.wasm) {
      this.wasm.paused ? this.wasm.resume() : this.wasm.pause()
      setTimeout(() => this.emit(), 0)
    } else {
      this.audio.paused ? this.audio.play() : this.audio.pause()
    }
  }

  // For <audio>, seeking live during a drag is cheap. For WASM every seek is
  // a fresh fetch + decoder, so the UI calls seekCommit only on release.
  seekLive(frac) {
    if (this.engine === 'audio' && this.audio.duration)
      this.audio.currentTime = frac * this.audio.duration
  }
  seekCommit(frac) {
    if (this.engine === 'wasm' && this.wasm && this.wasm.duration)
      this.wasm.seek(frac * this.wasm.duration)
  }

  // ---- engine ---------------------------------------------------------
  play(song) {
    if (!song) return
    // Silence whatever is playing RIGHT NOW, synchronously, before the async
    // startEngine for the new song runs — otherwise a quick second click
    // leaves the first track playing underneath the new one.
    this.audio.pause()
    try { this.audio.removeAttribute('src'); this.audio.load() } catch { /* ok */ }
    if (this.wasm) { this.wasm.unlock(); this.wasm.stop() }
    this.completePlay()               // finalize the previous song's session
    this.song = song
    this.spinning = true
    this.startEngine(song)
    this.startPlay(song)              // open reward session
    this.emit()
  }

  async startEngine(song) {
    const url = streamUrl(song.contentHash)
    const durSec = (song.durationMs || 0) / 1000
    this.audio.pause()
    try { this.audio.removeAttribute('src'); this.audio.load() } catch { /* ok */ }
    const useAudio = () => {
      if (this.song?.contentHash !== song.contentHash) return
      this.engine = 'audio'
      this.audio.src = url
      this.audio.play().catch(() => {/* autoplay policy: user can hit play */})
      this.emit()
    }
    if (this.wasm) {
      let started = false
      this.wasm.onplaying = () => {
        started = true
        if (this.song?.contentHash === song.contentHash) { this.spinning = false; this.emit() }
      }
      this.wasm.ontimeupdate = () => { if (this.song?.contentHash === song.contentHash) this.emit() }
      this.wasm.onended      = () => { if (this.song?.contentHash === song.contentHash) this.onTrackEnded() }
      try {
        await this.wasm.load(url, durSec)
        if (this.song?.contentHash !== song.contentHash) return  // superseded
        this.engine = 'wasm'
        this.emit()
        setTimeout(() => {            // watchdog: no sound? drop to <audio>
          if (!started && this.engine === 'wasm' && this.song?.contentHash === song.contentHash) {
            this.wasm.stop(); useAudio()
          }
        }, 5000)
        return
      } catch {                       // unsupported codec or decode error
        if (this.song?.contentHash !== song.contentHash) return
        try { await this.wasm.stop() } catch { /* ok */ }
      }
    }
    useAudio()
  }

  onTrackEnded() {
    if (this.qIndex >= 0 && this.qIndex + 1 < this.queue.length) {
      this.playFromQueue(this.qIndex + 1)   // play() finalizes the session
    } else {
      this.completePlay()
      this.song = null
      this.emit()
    }
  }

  // ---- reward session ---------------------------------------------------
  stopHeartbeat() { if (this.hbTimer) { clearInterval(this.hbTimer); this.hbTimer = null } }

  async startPlay(song) {
    this.stopHeartbeat(); this.playId = null
    try {
      const r = await apiPost('/api/play/start', { contentHash: song.contentHash })
      if (this.song?.contentHash === song.contentHash) {
        this.playId = r.playId
        this.hbTimer = setInterval(() => {
          if (this.playId && !this.engPaused())
            apiPost('/api/play/heartbeat',
              { playId: this.playId, positionMs: Math.floor(this.engCurSec() * 1000) })
              .catch(() => {})
        }, 5000)
      }
    } catch { /* reward best-effort; playback continues regardless */ }
  }

  completePlay() {
    this.stopHeartbeat()
    const id = this.playId; this.playId = null
    if (id) apiPost('/api/play/complete', { playId: id }).catch(() => {})
  }
}

export const player = new Player()
