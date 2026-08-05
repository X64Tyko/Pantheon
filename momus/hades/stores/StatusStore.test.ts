import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { StatusStore } from '@/stores/StatusStore'
import { api } from '@/api/client'

vi.mock('@/api/client', () => ({
  api: {
    getSyncStatus:      vi.fn(),
    getMatchStatus:     vi.fn(),
    getWritebackStatus: vi.fn(),
    getSettings:        vi.fn(),
      getChapterDetectStatus: vi.fn(),
  },
}))

const mockApi = api as Record<'getSyncStatus' | 'getMatchStatus' | 'getWritebackStatus' | 'getSettings' | 'getChapterDetectStatus', ReturnType<typeof vi.fn>>

const SETTINGS = {
  epg_debug: false, sync_debug: false, sync_threads: 6, stream_buffer_size: 65536,
  image_cache_ttl_hours: 2, verbose_transcode_logs: false, cast_app_id: '',
}

// ---------------------------------------------------------------------------
// Timing note:
//   _poll() awaits Promise.all([getSyncStatus(), getMatchStatus(), getSettings()])
//   before the first assignment, so all three mocks resolve together — await
//   any one of them (they settle in the same microtask batch) to flush state.
// ---------------------------------------------------------------------------

describe('StatusStore', () => {
  let store: StatusStore

  beforeEach(() => {
    vi.useFakeTimers()
    vi.resetAllMocks()
    mockApi.getSyncStatus.mockResolvedValue({ running: false })
    mockApi.getMatchStatus.mockResolvedValue({ running: false })
    mockApi.getWritebackStatus.mockResolvedValue({ running: false })
    mockApi.getSettings.mockResolvedValue(SETTINGS)
      mockApi.getChapterDetectStatus.mockResolvedValue({running: false})
    store = new StatusStore()
  })

  afterEach(() => {
    store.stopPolling()
    vi.useRealTimers()
  })

  // ── initial state ─────────────────────────────────────────────────────────

  it('starts with syncing=false, matching=false, anyRunning=false', () => {
    expect(store.syncing).toBe(false)
    expect(store.matching).toBe(false)
    expect(store.anyRunning).toBe(false)
      expect(store.detecting).toBe(false)
  })

  // ── startPolling ──────────────────────────────────────────────────────────

  describe('startPolling', () => {
    it('calls getSyncStatus/getMatchStatus/getWritebackStatus/getSettings immediately on first poll', () => {
      store.startPolling()
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(1)
      expect(mockApi.getMatchStatus).toHaveBeenCalledTimes(1)
      expect(mockApi.getWritebackStatus).toHaveBeenCalledTimes(1)
      expect(mockApi.getSettings).toHaveBeenCalledTimes(1)
        expect(mockApi.getChapterDetectStatus).toHaveBeenCalledTimes(1)
    })

    it('sets syncing=true when the API reports running=true', async () => {
      mockApi.getSyncStatus.mockResolvedValue({ running: true })
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value
      await Promise.resolve()
      expect(store.syncing).toBe(true)
    })

    it('sets matching=true when the API reports running=true', async () => {
      mockApi.getMatchStatus.mockResolvedValue({ running: true })
      store.startPolling()
      await mockApi.getMatchStatus.mock.results[0].value
      await Promise.resolve()
      expect(store.matching).toBe(true)
    })

    it('mirrors sync_debug/epg_debug from settings', async () => {
      mockApi.getSettings.mockResolvedValue({ ...SETTINGS, sync_debug: true, epg_debug: true })
      store.startPolling()
      await mockApi.getSettings.mock.results[0].value
      await Promise.resolve()
      expect(store.syncDebug).toBe(true)
      expect(store.epgDebug).toBe(true)
    })

    it('is idempotent — calling again once the timer is set has no effect', async () => {
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value
      await Promise.resolve()
      // _timer is now set; second startPolling() should be a no-op
      store.startPolling()
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(1)
    })

    it('schedules next poll after 15s when idle', async () => {
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value
      await Promise.resolve()

      vi.advanceTimersByTime(15000)
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(2)
    })

    it('does not poll again before 15s interval when idle', async () => {
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value
      await Promise.resolve()

      vi.advanceTimersByTime(14999)
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(1)
    })

    it('schedules next poll after 2s when syncing or matching is active', async () => {
      mockApi.getSyncStatus.mockResolvedValue({ running: true })
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value
      await Promise.resolve()

      vi.advanceTimersByTime(2000)
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(2)
    })

    it('does not use the 2s interval when nothing is running', async () => {
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value
      await Promise.resolve()

      vi.advanceTimersByTime(2000)
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(1) // 15s interval, not fired yet
    })
  })

  // ── stopPolling ───────────────────────────────────────────────────────────

  describe('stopPolling', () => {
    it('prevents future polls after the timer is set', async () => {
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value
      await Promise.resolve()

      store.stopPolling()
      vi.advanceTimersByTime(20000)

      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(1)
    })

    it('can be called when not polling without throwing', () => {
      expect(() => store.stopPolling()).not.toThrow()
    })
  })

  // ── error resilience ──────────────────────────────────────────────────────

  describe('error handling', () => {
    it('swallows API errors and does not propagate them', async () => {
      mockApi.getSyncStatus.mockRejectedValue(new Error('Network error'))
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value.catch(() => {})
      // If errors weren't caught, the unhandled rejection would fail the test
    })

    it('keeps syncing=false when the API fails', async () => {
      mockApi.getSyncStatus.mockRejectedValue(new Error('fail'))
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value.catch(() => {})
      await Promise.resolve()
      expect(store.syncing).toBe(false)
    })

    it('still schedules the next poll (at the idle interval) after a failure', async () => {
      mockApi.getSyncStatus.mockRejectedValue(new Error('fail'))
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value.catch(() => {})
      await Promise.resolve()

      vi.advanceTimersByTime(15000)
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(2)
    })
  })
})
