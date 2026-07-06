import { GATEWAY } from './config'

export async function apiGet(path) {
  const r = await fetch(GATEWAY + path, { mode: 'cors' })
  if (!r.ok) {
    let msg = `HTTP ${r.status}`
    try { msg = (await r.json()).error || msg } catch { /* keep code */ }
    throw new Error(msg)
  }
  return r.json()
}

export async function apiPost(path, body) {
  const r = await fetch(GATEWAY + path, {
    method: 'POST', mode: 'cors',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  if (!r.ok) throw new Error(`HTTP ${r.status}`)
  return r.json()
}

export const streamUrl = (hash) => `${GATEWAY}/api/stream/${hash}`

/// Server-side search / facet page — the catalog never ships whole.
export async function fetchSongs(params) {
  const qs = new URLSearchParams(params).toString()
  const r = await apiGet('/api/songs?' + qs)
  return Array.isArray(r) ? r : (r.songs || [])
}

export const fetchFacets = () => apiGet('/api/facets')
export const fetchCollections = () => apiGet('/api/collections')

/// A collection member is kept even when unseeded — the UI dims it.
export const avail = (s) => s.available !== false
