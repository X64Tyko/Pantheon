import { beforeEach } from 'vitest'

// vitest.config.ts runs momus tests under environment: 'node' — no jsdom, so
// none of the browser-only globals api/client.ts (and anything built on it)
// reaches for at call time exist by default. Real, minimal-but-complete
// implementations here (not per-test ad-hoc stubs) so every test file gets
// the same behavior and nothing has to remember to wire this up itself.
class MemoryStorage implements Storage {
  private store = new Map<string, string>()
  get length() { return this.store.size }
  clear() { this.store.clear() }
  getItem(key: string) { return this.store.has(key) ? this.store.get(key)! : null }
  key(index: number) { return Array.from(this.store.keys())[index] ?? null }
  removeItem(key: string) { this.store.delete(key) }
  setItem(key: string, value: string) { this.store.set(key, String(value)) }
}

// Always installed, not just when absent — recent Node versions ship their
// own ambient `localStorage` global, but it's an inert stub outside a real
// browser-shaped runtime (no getItem/setItem at all), which is worse than
// not having one since `typeof` checks no longer catch its absence.
;(globalThis as any).localStorage = new MemoryStorage()

// client.ts dispatches `window.dispatchEvent(new CustomEvent('kairos:unauthorized'))`
// on a 401 — Node has CustomEvent/EventTarget as real globals, just no
// `window` pointing at one. Node has no `window` at all by default.
// `location.origin` is also stubbed here since castMedia.ts's
// toAbsoluteStreamUrl() reads it the same way real Hades code does — Hades
// is always same-origin with Hermes (vite.config.ts proxies /api and
// /stream), so a fixed fake origin is a faithful enough stand-in for tests.
if (typeof (globalThis as any).window === 'undefined') {
  const win = new EventTarget() as unknown as Window
  ;(win as any).location = { origin: 'http://pantheon.local' }
  ;(globalThis as any).window = win
}

beforeEach(() => {
  globalThis.localStorage.clear()
})
