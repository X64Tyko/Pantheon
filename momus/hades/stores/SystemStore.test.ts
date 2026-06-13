import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { SystemStore } from '@/stores/SystemStore'
import { api } from '@/api/client'

vi.mock('@/api/client', () => ({
  api: { getSyncStatus: vi.fn() },
}))

const mockApi = api as { getSyncStatus: ReturnType<typeof vi.fn> }

// ---------------------------------------------------------------------------
// Timing note:
//   _fetchStatus() calls getSyncStatus() synchronously before the first await,
//   so we can assert the call count immediately after startPolling().
//   To check store.syncing (set inside runInAction after the await), we await
//   the returned promise to flush the microtask chain.
// ---------------------------------------------------------------------------

describe('SystemStore', () => {
  let store: SystemStore

  beforeEach(() => {
    vi.useFakeTimers()
    vi.resetAllMocks()
    store = new SystemStore()
  })

  afterEach(() => {
    store.stopPolling()
    vi.useRealTimers()
  })

  // ── initial state ─────────────────────────────────────────────────────────

  it('starts with syncing=false', () => {
    expect(store.syncing).toBe(false)
  })

  // ── startPolling ──────────────────────────────────────────────────────────

  describe('startPolling', () => {
    it('calls getSyncStatus immediately on first poll', () => {
      mockApi.getSyncStatus.mockResolvedValue({ running: false })
      store.startPolling()
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(1)
    })

    it('sets syncing=true when API reports running=true', async () => {
      mockApi.getSyncStatus.mockResolvedValue({ running: true })
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value
      expect(store.syncing).toBe(true)
    })

    it('sets syncing=false when API reports running=false', async () => {
      mockApi.getSyncStatus.mockResolvedValue({ running: false })
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value
      expect(store.syncing).toBe(false)
    })

    it('is idempotent — calling again once the timer is set has no effect', async () => {
      mockApi.getSyncStatus.mockResolvedValue({ running: false })
      store.startPolling()
      // Wait for first poll to finish and _timer to be set
      await mockApi.getSyncStatus.mock.results[0].value
      await Promise.resolve()
      // _timer is now set; second startPolling() should be a no-op
      store.startPolling()
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(1)
    })

    it('schedules next poll after 15s when idle', async () => {
      mockApi.getSyncStatus.mockResolvedValue({ running: false })
      store.startPolling()
      // Let first poll and _poll continuation complete
      await mockApi.getSyncStatus.mock.results[0].value
      await Promise.resolve()

      vi.advanceTimersByTime(15000)
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(2)
    })

    it('does not poll again before 15s interval when idle', async () => {
      mockApi.getSyncStatus.mockResolvedValue({ running: false })
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value
      await Promise.resolve()

      vi.advanceTimersByTime(14999)
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(1)
    })

    it('schedules next poll after 2s when syncing is active', async () => {
      mockApi.getSyncStatus.mockResolvedValue({ running: true })
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value
      await Promise.resolve()

      vi.advanceTimersByTime(2000)
      expect(mockApi.getSyncStatus).toHaveBeenCalledTimes(2)
    })

    it('does not use 2s interval when not syncing', async () => {
      mockApi.getSyncStatus.mockResolvedValue({ running: false })
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
      mockApi.getSyncStatus.mockResolvedValue({ running: false })
      store.startPolling()
      // Wait for first poll + _poll continuation (which sets _timer)
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

    it('keeps syncing=false when API fails', async () => {
      mockApi.getSyncStatus.mockRejectedValue(new Error('fail'))
      store.startPolling()
      await mockApi.getSyncStatus.mock.results[0].value.catch(() => {})
      expect(store.syncing).toBe(false)
    })
  })
})
