import { describe, it, expect, vi, beforeEach } from 'vitest'
import { ChannelStore } from '@/stores/ChannelStore'
import { api } from '@/api/client'

// ---------------------------------------------------------------------------
// Mock the entire api module so no fetch ever fires
// ---------------------------------------------------------------------------

vi.mock('@/api/client', () => ({
  api: {
    getChannels:   vi.fn(),
    createChannel: vi.fn(),
    deleteChannel: vi.fn(),
  },
}))

const mockApi = api as {
  getChannels:   ReturnType<typeof vi.fn>
  createChannel: ReturnType<typeof vi.fn>
  deleteChannel: ReturnType<typeof vi.fn>
}

// ---------------------------------------------------------------------------
// Fixture data
// ---------------------------------------------------------------------------

const CHANNEL_1 = {
  channel_id: 'c1', name: 'CNN',  number: 1, timezone: 'UTC',
  default_filler_entries: [], default_filler_selection: 'round_robin' as const,
}
const CHANNEL_2 = {
  channel_id: 'c2', name: 'ESPN', number: 2, timezone: 'UTC',
  default_filler_entries: [], default_filler_selection: 'round_robin' as const,
}
const CHANNELS = [CHANNEL_1, CHANNEL_2]

// ---------------------------------------------------------------------------

describe('ChannelStore', () => {
  let store: ChannelStore

  beforeEach(() => {
    vi.resetAllMocks()
    store = new ChannelStore()
  })

  // ── fetchAll ──────────────────────────────────────────────────────────────

  describe('fetchAll', () => {
    it('populates channels on success', async () => {
      mockApi.getChannels.mockResolvedValue(CHANNELS)
      await store.fetchAll()
      expect(store.channels).toHaveLength(2)
      expect(store.channels[0].name).toBe('CNN')
      expect(store.channels[1].name).toBe('ESPN')
    })

    it('sets loading=false after successful fetch', async () => {
      mockApi.getChannels.mockResolvedValue(CHANNELS)
      await store.fetchAll()
      expect(store.loading).toBe(false)
    })

    it('clears error on success', async () => {
      mockApi.getChannels.mockRejectedValueOnce(new Error('first fail'))
      await store.fetchAll()
      mockApi.getChannels.mockResolvedValue(CHANNELS)
      await store.fetchAll()
      expect(store.error).toBeNull()
    })

    it('sets error message on failure', async () => {
      mockApi.getChannels.mockRejectedValue(new Error('Network error'))
      await store.fetchAll()
      expect(store.error).toBe('Network error')
    })

    it('sets loading=false even on failure', async () => {
      mockApi.getChannels.mockRejectedValue(new Error('fail'))
      await store.fetchAll()
      expect(store.loading).toBe(false)
    })

    it('does not overwrite channels on failure', async () => {
      mockApi.getChannels.mockResolvedValueOnce(CHANNELS)
      await store.fetchAll()
      mockApi.getChannels.mockRejectedValue(new Error('fail'))
      await store.fetchAll()
      expect(store.channels).toHaveLength(2);
    })

    it('replaces stale channels on subsequent successful fetch', async () => {
      mockApi.getChannels.mockResolvedValueOnce(CHANNELS)
      await store.fetchAll()
      const updated = [CHANNEL_1]
      mockApi.getChannels.mockResolvedValueOnce(updated)
      await store.fetchAll()
      expect(store.channels).toHaveLength(1)
    })
  })

  // ── add ───────────────────────────────────────────────────────────────────

  describe('add', () => {
    it('calls createChannel with the supplied data', async () => {
      mockApi.createChannel.mockResolvedValue({ channel_id: 'c3' })
      mockApi.getChannels.mockResolvedValue(CHANNELS)
      await store.add({ name: 'FOX', number: 3, timezone: 'UTC' })
      expect(mockApi.createChannel).toHaveBeenCalledWith({
        name: 'FOX', number: 3, timezone: 'UTC',
      })
    })

    it('refetches the channel list after creating', async () => {
      mockApi.createChannel.mockResolvedValue({ channel_id: 'c3' })
      const withFox = [...CHANNELS, { channel_id: 'c3', name: 'FOX', number: 3,
        timezone: 'UTC', default_filler_entries: [], default_filler_selection: 'round_robin' as const }]
      mockApi.getChannels.mockResolvedValue(withFox)
      await store.add({ name: 'FOX', number: 3, timezone: 'UTC' })
      expect(mockApi.getChannels).toHaveBeenCalledTimes(1)
      expect(store.channels).toHaveLength(3)
    })
  })

  // ── remove ────────────────────────────────────────────────────────────────

  describe('remove', () => {
    beforeEach(async () => {
      mockApi.getChannels.mockResolvedValue(CHANNELS)
      await store.fetchAll()
      mockApi.deleteChannel.mockResolvedValue(undefined)
    })

    it('calls deleteChannel with the correct id', async () => {
      await store.remove('c1')
      expect(mockApi.deleteChannel).toHaveBeenCalledWith('c1')
    })

    it('removes the channel from the local list optimistically', async () => {
      await store.remove('c1')
      expect(store.channels).toHaveLength(1)
      expect(store.channels[0].channel_id).toBe('c2')
    })

    it('preserves remaining channels after removal', async () => {
      await store.remove('c2')
      expect(store.channels[0].channel_id).toBe('c1')
    })

    it('is a no-op on an unknown id', async () => {
      await store.remove('does-not-exist')
      expect(store.channels).toHaveLength(2)
    })
  })

  // ── initial state ─────────────────────────────────────────────────────────

  describe('initial state', () => {
    it('starts with empty channels', () => {
      expect(store.channels).toHaveLength(0)
    })

    it('starts with loading=false', () => {
      expect(store.loading).toBe(false)
    })

    it('starts with error=null', () => {
      expect(store.error).toBeNull()
    })
  })
})
