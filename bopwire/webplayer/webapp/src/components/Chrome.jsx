import { useEffect, useRef, useState } from 'react'
import { DOWNLOADS } from '../config'

/// Topbar: brand, Home/Browse pills, search, network status, DMCA link.
export function TopBar({ view, onView, query, onQuery, status }) {
  return (
    <header className="flex flex-none flex-wrap items-center gap-3 border-b border-line
        bg-elev/80 px-4 py-2.5 backdrop-blur-md">
      <div className="flex items-center gap-2 font-bold tracking-wide">
        <img src="./logo.png" alt="Bopwire" width="28" height="28" className="rounded" />
        <span className="max-sm:hidden">bopwire</span>
      </div>

      <nav className="inline-flex rounded-full border border-line bg-elev2 p-[3px]" role="tablist">
        {['home', 'browse'].map((v) => (
          <button key={v} role="tab" aria-selected={view === v || (view === 'list' && v === 'home')}
            onClick={() => onView(v)}
            className={`rounded-full px-4 py-1.5 text-sm font-semibold capitalize transition-colors
              ${view === v ? 'bg-mint text-mintink' : 'text-dim hover:text-ink'}`}>
            {v}
          </button>
        ))}
      </nav>

      <div className="relative min-w-40 flex-1 max-w-md max-sm:order-10 max-sm:basis-full max-sm:max-w-none">
        <span className="pointer-events-none absolute left-3 top-1/2 -translate-y-[52%] text-[15px] text-dim">⌕</span>
        <input
          type="search" value={query} onChange={(e) => onQuery(e.target.value)}
          placeholder="Search songs, artists, albums…"
          autoComplete="off" spellCheck="false"
          className="w-full rounded-full border border-line bg-elev2 py-2 pl-8 pr-3 text-sm
            outline-none transition-colors placeholder:text-dim/70 focus:border-sky"
        />
      </div>

      <div className="ml-auto flex items-center gap-1.5 whitespace-nowrap text-xs text-dim" title="Network status">
        <span className={`size-2 rounded-full
          ${status.kind === 'ok' ? 'animate-pulse-dot bg-mint'
            : status.kind === 'err' ? 'bg-red-400' : 'bg-dim'}`} />
        <span className="max-md:hidden">{status.text}</span>
      </div>

      <a href="./dmca.html" title="Copyright takedown request (DMCA)"
        className="rounded-md border border-line px-2 py-1 text-xs font-bold tracking-wider
          text-dim transition-colors hover:border-mint hover:text-mint">DMCA</a>
    </header>
  )
}

/// Earn-crypto CTA footer with the app-download dropdown.
export function CtaFooter() {
  const [open, setOpen] = useState(false)
  const ref = useRef(null)
  useEffect(() => {
    const close = (e) => { if (!ref.current?.contains(e.target)) setOpen(false) }
    document.addEventListener('click', close)
    return () => document.removeEventListener('click', close)
  }, [])
  return (
    <footer className="flex h-11 flex-none items-center justify-center gap-1.5 border-t
        border-line bg-gradient-to-r from-mint/10 to-sky/10 text-[13px] max-sm:px-2
        max-sm:text-xs">
      <span className="text-center">
        Want to earn crypto listening to artists with less than 10,000 plays or sharing your library?
      </span>
      <span className="relative" ref={ref}>
        <button onClick={() => setOpen(!open)} aria-haspopup="true" aria-expanded={open}
          className="font-bold text-mint underline">Click here</button>
        {open && (
          <div className="absolute bottom-[calc(100%+8px)] left-1/2 z-20 min-w-52 -translate-x-1/2
              rounded-xl border border-line bg-elev2 p-1.5 shadow-2xl">
            <img src="./logo.png" alt="Bopwire app" width="64" height="64"
              className="mx-auto mb-0.5 mt-2 rounded-2xl" />
            <p className="px-1.5 pb-2 pt-0.5 text-center text-xs font-bold opacity-85">
              Get the Bopwire app</p>
            {[['windows', '🪟', 'Windows Installer'], ['android', '🤖', 'Android APK'],
              ['linux', '🐧', 'Linux AppImage']].map(([os, ico, label]) => (
              <a key={os} href={DOWNLOADS[os] || '#'} role="menuitem"
                className="flex items-center gap-2.5 rounded-lg px-3 py-2 text-sm hover:bg-elev">
                <span>{ico}</span> {label}
              </a>
            ))}
          </div>
        )}
      </span>
    </footer>
  )
}

export function Toast({ msg }) {
  if (!msg) return null
  return (
    <div className="pointer-events-none fixed inset-x-0 bottom-32 z-50 flex justify-center px-4">
      <div className="animate-rise max-w-[80vw] rounded-lg border border-line bg-elev2 px-4
          py-2.5 text-sm shadow-2xl">{msg}</div>
    </div>
  )
}
