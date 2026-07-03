import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { SystemStore } from '@/stores/SystemStore'

// SystemStore only pulls the TOKEN_KEY string constant out of api/client (no
// api calls — it talks to Kairos directly via a raw EventSource), so the
// whole module is stubbed rather than mocked call-by-call.
vi.mock('@/api/client', () => ({ TOKEN_KEY: 'kairos_token' }))

// vitest's `environment: 'node'` (vitest.config.ts) has no EventSource
// ambient global — stubbed per-test below rather than pulling in jsdom/
// happy-dom for this one store. localStorage comes from momus/hades/setup.ts.
class FakeEventSource {
  static instances: FakeEventSource[] = []
  url: string
  onopen:    (() => void) | null = null
  onerror:   (() => void) | null = null
  onmessage: ((e: { data: string }) => void) | null = null
  closed = false

  constructor(url: string) {
    this.url = url
    FakeEventSource.instances.push(this)
  }
  close() { this.closed = true }
}

function currentES(): FakeEventSource {
  return FakeEventSource.instances[FakeEventSource.instances.length - 1]
}

describe('SystemStore', () => {
  let store: SystemStore

  beforeEach(() => {
    vi.useFakeTimers()
    FakeEventSource.instances = []
    ;(globalThis as any).EventSource = FakeEventSource
    store = new SystemStore()
  })

  afterEach(() => {
    vi.useRealTimers()
  })

  // ── connectLogs ────────────────────────────────────────────────────────────

  describe('connectLogs', () => {
    it('opens an EventSource against /api/logs/stream when no token is set', () => {
      store.connectLogs()
      expect(FakeEventSource.instances).toHaveLength(1)
      expect(currentES().url).toBe('/api/logs/stream')
    })

    it('appends ?token= when a token is present', () => {
      localStorage.setItem('kairos_token', 'abc123')
      store.connectLogs()
      expect(currentES().url).toBe('/api/logs/stream?token=abc123')
    })

    it('sets liveStatus to connecting immediately, then live on open', () => {
      store.connectLogs()
      expect(store.liveStatus).toBe('connecting')
      currentES().onopen?.()
      expect(store.liveStatus).toBe('live')
    })

    it('is idempotent — a second call while already connected opens no new EventSource', () => {
      store.connectLogs()
      store.connectLogs()
      expect(FakeEventSource.instances).toHaveLength(1)
    })
  })

  // ── onerror / reconnect ────────────────────────────────────────────────────

  describe('reconnect on error', () => {
    it('sets liveStatus=disconnected and closes the source', () => {
      store.connectLogs()
      const es = currentES()
      es.onerror?.()
      expect(store.liveStatus).toBe('disconnected')
      expect(es.closed).toBe(true)
    })

    it('reconnects after 5s', () => {
      store.connectLogs()
      currentES().onerror?.()
      expect(FakeEventSource.instances).toHaveLength(1)
      vi.advanceTimersByTime(5000)
      expect(FakeEventSource.instances).toHaveLength(2)
    })
  })

  // ── onmessage / log entries ────────────────────────────────────────────────

  describe('log messages', () => {
    it('appends a log entry', () => {
      store.connectLogs()
      currentES().onmessage?.({ data: 'hello world' })
      expect(store.logs).toHaveLength(1)
      expect(store.logs[0].line).toBe('hello world')
      expect(store.logs[0].isError).toBe(false)
    })

    it('flags lines starting with [error] as errors and increments unreadErrors', () => {
      store.connectLogs()
      currentES().onmessage?.({ data: '[error] something broke' })
      expect(store.logs[0].isError).toBe(true)
      expect(store.unreadErrors).toBe(1)
    })

    it('caps logs at 1000 entries, dropping the oldest', () => {
      store.connectLogs()
      const es = currentES()
      for (let i = 0; i < 1005; i++) es.onmessage?.({ data: `line ${i}` })
      expect(store.logs).toHaveLength(1000)
      expect(store.logs[0].line).toBe('line 5')
      expect(store.logs[999].line).toBe('line 1004')
    })
  })

  // ── error toast ─────────────────────────────────────────────────────────────

  describe('error toast', () => {
    it('shows a toast with the [error] tag stripped', () => {
      store.connectLogs()
      currentES().onmessage?.({ data: '[error] disk full' })
      expect(store.toast?.msg).toBe('disk full')
    })

    it('auto-dismisses after 8s', () => {
      store.connectLogs()
      currentES().onmessage?.({ data: '[error] disk full' })
      expect(store.toast).not.toBeNull()
      vi.advanceTimersByTime(8000)
      expect(store.toast).toBeNull()
    })

    it('a second error resets the auto-dismiss timer instead of stacking', () => {
      store.connectLogs()
      const es = currentES()
      es.onmessage?.({ data: '[error] first' })
      vi.advanceTimersByTime(6000)
      es.onmessage?.({ data: '[error] second' })
      vi.advanceTimersByTime(6000) // 12s since first, only 6s since second
      expect(store.toast?.msg).toBe('second')
      vi.advanceTimersByTime(2000) // now 8s since second
      expect(store.toast).toBeNull()
    })

    it('dismissToast clears the toast immediately and cancels the pending auto-dismiss', () => {
      store.connectLogs()
      currentES().onmessage?.({ data: '[error] disk full' })
      store.dismissToast()
      expect(store.toast).toBeNull()
    })
  })

  // ── unread errors ──────────────────────────────────────────────────────────

  describe('clearUnreadErrors', () => {
    it('resets the counter to 0', () => {
      store.connectLogs()
      currentES().onmessage?.({ data: '[error] one' })
      currentES().onmessage?.({ data: '[error] two' })
      expect(store.unreadErrors).toBe(2)
      store.clearUnreadErrors()
      expect(store.unreadErrors).toBe(0)
    })
  })
})
