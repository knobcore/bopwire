// Deterministic cover art — integer-only algorithm shared verbatim with the
// Flutter client (lib/src/widgets/cover_art.dart) and the legacy frontend.
// Same seed bytes → same hues, same motif geometry ⇒ identical art on every
// client. KEEP THE IMPLEMENTATIONS IN LOCKSTEP.
//
// Song seed:  first 8 bytes of the content hash.
// Name seed:  FNV-1a-32(name) ++ FNV-1a-32(name + '*'), big-endian bytes.

const normKey = (s) => String(s ?? '').trim().toLowerCase().replace(/\s+/g, ' ')

export function seedFromHash(hex) {
  const b = []
  for (let i = 0; i < 8; i++) b.push(parseInt(String(hex).substr(i * 2, 2), 16) || 0)
  return b
}

function fnv32(str) {
  let h = 0x811c9dc5
  for (let i = 0; i < str.length; i++) {
    h ^= str.charCodeAt(i) & 0xff
    h = Math.imul(h, 0x01000193) >>> 0
  }
  return h >>> 0
}

export function seedFromName(name) {
  const n = normKey(name)
  const a = fnv32(n), b = fnv32(n + '*')
  return [(a >>> 24) & 255, (a >>> 16) & 255, (a >>> 8) & 255, a & 255,
          (b >>> 24) & 255, (b >>> 16) & 255, (b >>> 8) & 255, b & 255]
}

export function artParams(seed) {
  const h1    = ((seed[0] << 8) | seed[1]) % 360
  const h2    = (h1 + 40 + (seed[2] % 200)) % 360
  const angle = seed[3] % 360
  const motif = seed[4] % 5
  const n     = 3 + (seed[5] % 4)
  const rot   = (seed[6] % 4) * 90
  return { h1, h2, angle, motif, n, rot, seed }
}

// 96×96 SVG string. All geometry integer math from the seed.
export function coverArtSvg(seed) {
  const p  = artParams(seed)
  const v  = (i) => p.seed[(i + 2) % 8]
  const bg1 = `hsl(${p.h1},45%,15%)`, bg2 = `hsl(${p.h2},60%,28%)`
  const a1  = `hsl(${p.h1},75%,62%)`, a2  = `hsl(${p.h2},85%,60%)`
  const alt = (i) => (i % 2 === 0 ? a1 : a2)
  let m = ''
  if (p.motif === 0) {            // concentric rings
    const step = Math.floor(36 / p.n)
    for (let i = 0; i < p.n; i++)
      m += `<circle cx="48" cy="48" r="${10 + i * step}" fill="none" stroke="${alt(i)}" stroke-width="3" opacity=".85"/>`
  } else if (p.motif === 1) {     // bars
    const w = Math.floor(64 / p.n)
    for (let i = 0; i < p.n; i++) {
      const h = 18 + (v(i) % 52)
      m += `<rect x="${16 + i * w}" y="${48 - (h >> 1)}" width="${w - 4}" height="${h}" rx="3" fill="${alt(i)}" opacity=".85"/>`
    }
  } else if (p.motif === 2) {     // nested diamonds
    const step = Math.floor(26 / p.n)
    for (let i = 0; i < p.n; i++) {
      const half = 34 - i * step
      m += `<rect x="${48 - half}" y="${48 - half}" width="${half * 2}" height="${half * 2}" fill="none" stroke="${alt(i)}" stroke-width="3" opacity=".85" transform="rotate(45,48,48)"/>`
    }
  } else if (p.motif === 3) {     // dot grid
    const sp = Math.floor(64 / (p.n - 1))
    for (let i = 0; i < p.n; i++)
      for (let j = 0; j < p.n; j++)
        m += `<circle cx="${16 + i * sp}" cy="${16 + j * sp}" r="${4 + (v(i + j) % 3)}" fill="${alt(i + j)}" opacity=".85"/>`
  } else {                        // stacked triangles
    const step = Math.floor(24 / p.n)
    for (let i = 0; i < p.n; i++) {
      const s = 38 - i * step
      m += `<polygon points="48,${48 - s} ${48 + s},${48 + s} ${48 - s},${48 + s}" fill="none" stroke="${alt(i)}" stroke-width="3" opacity=".85"/>`
    }
  }
  const gid = 'g' + p.seed.map((x) => x.toString(16).padStart(2, '0')).join('')
  return `<svg viewBox="0 0 96 96" xmlns="http://www.w3.org/2000/svg" preserveAspectRatio="xMidYMid slice">
    <defs><linearGradient id="${gid}" gradientTransform="rotate(${p.angle},.5,.5)">
      <stop offset="0" stop-color="${bg1}"/><stop offset="1" stop-color="${bg2}"/>
    </linearGradient></defs>
    <rect width="96" height="96" fill="url(#${gid})"/>
    <g transform="rotate(${p.rot},48,48)">${m}</g>
  </svg>`
}

export const songSeed = (s) => seedFromHash(s.contentHash)

// Genre tile background: the two hues of the same seed as a CSS gradient.
export function tileGradient(name) {
  const p = artParams(seedFromName(name))
  return `linear-gradient(${p.angle}deg, hsl(${p.h1},60%,30%), hsl(${p.h2},70%,42%))`
}

// Dynamic accent for the now-playing bar, derived from the track's art hues.
export function accentFor(seed) {
  const p = artParams(seed)
  return { a1: `hsl(${p.h1},75%,62%)`, a2: `hsl(${p.h2},85%,60%)` }
}
