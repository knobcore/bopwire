export const normKey = (s) => String(s ?? '').trim().toLowerCase().replace(/\s+/g, ' ')

export const fmtDur = (ms) => {
  const t = Math.max(0, Math.floor((ms || 0) / 1000))
  const m = Math.floor(t / 60), s = t % 60
  return `${m}:${String(s).padStart(2, '0')}`
}

export const fmtPlays = (n) => {
  n = n || 0
  if (n < 1000) return `${n}`
  if (n < 1e6)  return `${(n / 1e3).toFixed(1).replace(/\.0$/, '')}K`
  return `${(n / 1e6).toFixed(1).replace(/\.0$/, '')}M`
}

// Group items by normalized key; label = most frequent original spelling.
export function bucketByNorm(items, keyFn) {
  const map = new Map()
  for (const it of items) {
    const raw = keyFn(it)
    const key = normKey(raw)
    let b = map.get(key)
    if (!b) { b = { key, items: [], spell: new Map() }; map.set(key, b) }
    b.items.push(it)
    const label = (raw ?? '').toString().trim()
    b.spell.set(label, (b.spell.get(label) || 0) + 1)
  }
  for (const b of map.values()) {
    let best = '', bestN = -1
    for (const [sp, n] of b.spell) if (n > bestN) { best = sp; bestN = n }
    b.label = best
  }
  return map
}

export const albumLabelOf = (s) =>
  (s.album && s.album.trim()) ? s.album.trim() : 'Singles'
