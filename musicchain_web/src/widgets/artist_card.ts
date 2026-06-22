// Reusable artist tile widget for the musicchain web player.
//
// Rendered into the `.card-grid` containers on the Discover and Library
// screens. Mirrors the Flutter ArtistTile in
// musicchain_player/lib/src/widgets/artist_tile.dart: circular avatar on
// top, centred name beneath, "N albums · M tracks" subtitle.
//
// Shares `.grid-card` base styling with album_card.ts; the circular
// avatar variant is keyed off `.artist-card__art`.

import { gradientFor } from './album_card.js';

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/** Minimal artist shape the widget reads. Callers can pass richer rows
 *  from verbs.ts — extra fields are ignored. */
export interface ArtistCardArtist {
  name: string;
  albumCount: number;
  trackCount: number;
  /** Optional artist photo URL. Absent => gradient placeholder keyed on
   *  the artist name. */
  coverUrl?: string;
}

export interface ArtistCardOptions {
  artist: ArtistCardArtist;
  onTap: () => void;
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

function subtitleFor(artist: ArtistCardArtist): string {
  const albums = `${artist.albumCount} ${artist.albumCount === 1 ? 'album' : 'albums'}`;
  const tracks = `${artist.trackCount} ${artist.trackCount === 1 ? 'track' : 'tracks'}`;
  return `${albums} · ${tracks}`;
}

// ---------------------------------------------------------------------------
// Public render
// ---------------------------------------------------------------------------

/** Append an artist card to `container`. Returns the card element. */
export function renderArtistCard(
  container: HTMLElement,
  opts: ArtistCardOptions,
): HTMLElement {
  const { artist, onTap } = opts;
  const name = artist.name.trim() || 'Unknown artist';

  const card = el('button', 'grid-card grid-card--artist');
  card.type = 'button';
  card.setAttribute('aria-label', name);

  // Circular avatar. Reuses the same gradient generator as album_card so
  // a given name renders consistently across screens.
  const art = el('div', 'grid-card__art grid-card__art--circle');
  if (artist.coverUrl && artist.coverUrl.length > 0) {
    const img = el('img', 'grid-card__art-img');
    img.alt = '';
    img.loading = 'lazy';
    img.decoding = 'async';
    img.src = artist.coverUrl;
    art.appendChild(img);
  } else {
    art.style.background = gradientFor(name);
    const letter = el('span', 'grid-card__art-letter', name.charAt(0).toUpperCase());
    art.appendChild(letter);
  }

  // Name centred (the .grid-card--artist scope adds text-align: center).
  const nameEl = el('div', 'grid-card__title', name);
  const subEl = el('div', 'grid-card__subtitle muted', subtitleFor(artist));

  card.append(art, nameEl, subEl);

  card.addEventListener('click', (ev) => {
    ev.preventDefault();
    onTap();
  });

  container.appendChild(card);
  return card;
}
