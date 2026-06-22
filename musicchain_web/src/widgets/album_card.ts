// Reusable album tile widget for the musicchain web player.
//
// Rendered into the `.card-grid` containers on the Discover, Library,
// and Artist screens. Mirrors the Flutter AlbumTile in
// musicchain_player/lib/src/widgets/album_tile.dart: square art on top,
// title + artist/year subtitle beneath.
//
// No framework — pure DOM, atomic CSS classes shared with artist_card.ts
// through `.grid-card` in public/style.css.

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/** Minimal album shape the widget reads. Callers can pass through richer
 *  AlbumRow types from verbs.ts — extra fields are ignored. */
export interface AlbumCardAlbum {
  title: string;
  artist: string;
  /** Optional release year — appended to the subtitle when present. */
  year?: number;
  trackCount: number;
  /** Optional cover-art URL. When absent the tile renders a deterministic
   *  gradient placeholder derived from the album title. */
  coverUrl?: string;
}

export interface AlbumCardOptions {
  album: AlbumCardAlbum;
  /** Click handler — fired on the whole card and on Enter/Space when
   *  focused. */
  onTap: () => void;
}

// ---------------------------------------------------------------------------
// Placeholder gradient generator
// ---------------------------------------------------------------------------

/** Deterministic 32-bit hash of a string (FNV-1a). Cheap, no deps, stable
 *  across runs so a given title always renders the same gradient. */
function hashString(s: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    // 32-bit FNV prime multiply, force into uint32 range.
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return h >>> 0;
}

/** Pick two HSL stops from `hash` and assemble a 135deg linear-gradient
 *  CSS value. The first hue comes from the low 9 bits, the second is
 *  rotated by a value derived from the next 8 bits so similar titles
 *  still diverge visually. */
export function gradientFor(title: string): string {
  const h = hashString(title || 'untitled');
  const hue1 = h % 360;
  const hue2 = (hue1 + 40 + ((h >>> 9) % 120)) % 360;
  // Keep saturation moderate and lightness in the dark band so text on
  // the (currently unused) overlay stays legible if we ever add one.
  return `linear-gradient(135deg, hsl(${hue1} 55% 32%) 0%, hsl(${hue2} 60% 22%) 100%)`;
}

// ---------------------------------------------------------------------------
// DOM helpers
// ---------------------------------------------------------------------------

function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  cls?: string,
  text?: string,
): HTMLElementTagNameMap[K] {
  const n = document.createElement(tag);
  if (cls) n.className = cls;
  if (text !== undefined) n.textContent = text;
  return n;
}

function subtitleFor(album: AlbumCardAlbum): string {
  const artist = album.artist.trim() || 'Unknown artist';
  if (typeof album.year === 'number' && Number.isFinite(album.year) && album.year > 0) {
    return `${artist} · ${album.year}`;
  }
  return artist;
}

// ---------------------------------------------------------------------------
// Public render
// ---------------------------------------------------------------------------

/** Append an album card to `container`. Returns the card element so the
 *  caller can stash it for later remove/replace if needed. */
export function renderAlbumCard(
  container: HTMLElement,
  opts: AlbumCardOptions,
): HTMLElement {
  const { album, onTap } = opts;
  const title = album.title.trim() || '(Untitled album)';

  const card = el('button', 'grid-card grid-card--album');
  card.type = 'button';
  card.setAttribute('aria-label', `${title} by ${album.artist.trim() || 'Unknown artist'}`);

  // Square art region. Either an <img> with the cover, or a div with a
  // gradient background derived from the title hash.
  const art = el('div', 'grid-card__art grid-card__art--square');
  if (album.coverUrl && album.coverUrl.length > 0) {
    const img = el('img', 'grid-card__art-img');
    img.alt = '';
    img.loading = 'lazy';
    img.decoding = 'async';
    img.src = album.coverUrl;
    art.appendChild(img);
  } else {
    art.style.background = gradientFor(title);
    // First letter watermark — keeps the placeholder from looking blank.
    const letter = el('span', 'grid-card__art-letter', title.charAt(0).toUpperCase());
    art.appendChild(letter);
  }

  const titleEl = el('div', 'grid-card__title', title);
  const subEl = el('div', 'grid-card__subtitle muted', subtitleFor(album));
  // Track count line — kept separate so screen-readers + sighted users
  // get a clear hierarchy.
  const tracksEl = el(
    'div',
    'grid-card__meta muted',
    `${album.trackCount} ${album.trackCount === 1 ? 'track' : 'tracks'}`,
  );

  card.append(art, titleEl, subEl, tracksEl);

  card.addEventListener('click', (ev) => {
    ev.preventDefault();
    onTap();
  });

  container.appendChild(card);
  return card;
}
