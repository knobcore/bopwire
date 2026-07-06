// Runtime config — no build-time env needed. Override the gateway for local
// testing exactly like the old player: bopwire.com/?gateway=http://localhost:8090
export const GATEWAY = (new URLSearchParams(location.search).get('gateway')
  || 'https://api.bopwire.com').replace(/\/+$/, '')

// Auto-refresh cadences (ms). Facets mirror the app's 20 s; collections only
// change at epoch boundaries so a slow poll is plenty.
export const REFRESH_MS = 20000
export const COLLECTIONS_MS = 60000

// "Click here" → download the native app (where the listener DOES earn).
export const DOWNLOADS = {
  linux:   'https://github.com/knobcore/bopwire/releases/latest/download/bopwire-linux-x86_64.AppImage',
  android: 'https://github.com/knobcore/bopwire/releases/latest/download/bopwire-android.apk',
  windows: 'https://github.com/knobcore/bopwire/releases/latest/download/bopwire-windows-x64-setup.exe',
}
