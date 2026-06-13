import { describe, it, expect, vi, beforeEach } from 'vitest'
import { SourceStore } from '@/stores/SourceStore'
import { api } from '@/api/client'

vi.mock('@/api/client', () => ({
  api: {
    getSources:       vi.fn(),
    getSourceTypes:   vi.fn(),
    createSource:     vi.fn(),
    deleteSource:     vi.fn(),
    getLibraries:     vi.fn(),
    getAvailableLibs: vi.fn(),
    addLibrary:       vi.fn(),
    removeLibrary:    vi.fn(),
    getCredentials:   vi.fn(),
    setCredentials:   vi.fn(),
    deleteCredentials:vi.fn(),
    triggerSync:      vi.fn(),
  },
}))

const mockApi = api as Record<keyof typeof api, ReturnType<typeof vi.fn>>

// ---------------------------------------------------------------------------
// Fixture data
// ---------------------------------------------------------------------------

const SOURCE_1 = { source_id: 's1', source_type: 'plex'     as const, display_name: 'Home Plex', base_url: 'http://plex', enabled: true }
const SOURCE_2 = { source_id: 's2', source_type: 'jellyfin' as const, display_name: 'Jellyfin',  base_url: 'http://jf',   enabled: true }
const SOURCES  = [SOURCE_1, SOURCE_2]

const TYPE_PLEX = { type: 'plex', display_name: 'Plex', supported: true }
const TYPES     = [TYPE_PLEX]

const LIB_1 = { library_id: 'lib1', source_id: 's1', external_lib_id: 'ext1', display_name: 'Movies', library_type: 'movie' as const, enabled: true }
const LIBS   = [LIB_1]

const AVAIL_LIB = { external_lib_id: 'extA', name: 'TV Shows', type: 'show' as const }

const CREDS = { has_token: true, has_user_id: false }

// ---------------------------------------------------------------------------

describe('SourceStore', () => {
  let store: SourceStore

  beforeEach(() => {
    vi.resetAllMocks()
    store = new SourceStore()
  })

  // ── initial state ─────────────────────────────────────────────────────────

  describe('initial state', () => {
    it('has empty sources and types', () => {
      expect(store.sources).toHaveLength(0)
      expect(store.sourceTypes).toHaveLength(0)
    })

    it('has no selection', () => {
      expect(store.selectedId).toBeNull()
      expect(store.selected).toBeUndefined()
    })

    it('starts with loading=false and error=null', () => {
      expect(store.loading).toBe(false)
      expect(store.error).toBeNull()
    })
  })

  // ── fetchAll ──────────────────────────────────────────────────────────────

  describe('fetchAll', () => {
    it('loads sources and types in parallel', async () => {
      mockApi.getSources.mockResolvedValue(SOURCES)
      mockApi.getSourceTypes.mockResolvedValue(TYPES)
      await store.fetchAll()
      expect(mockApi.getSources).toHaveBeenCalledTimes(1)
      expect(mockApi.getSourceTypes).toHaveBeenCalledTimes(1)
      expect(store.sources).toHaveLength(2)
      expect(store.sourceTypes).toHaveLength(1)
    })

    it('sets loading=false after success', async () => {
      mockApi.getSources.mockResolvedValue(SOURCES)
      mockApi.getSourceTypes.mockResolvedValue(TYPES)
      await store.fetchAll()
      expect(store.loading).toBe(false)
    })

    it('sets error when fetch fails', async () => {
      mockApi.getSources.mockRejectedValue(new Error('offline'))
      mockApi.getSourceTypes.mockResolvedValue(TYPES)
      await store.fetchAll()
      expect(store.error).toBe('offline')
      expect(store.loading).toBe(false)
    })

    it('clears previous error on success', async () => {
      mockApi.getSources.mockRejectedValueOnce(new Error('fail'))
      mockApi.getSourceTypes.mockResolvedValue(TYPES)
      await store.fetchAll()
      mockApi.getSources.mockResolvedValue(SOURCES)
      await store.fetchAll()
      expect(store.error).toBeNull()
    })
  })

  // ── selected computed ─────────────────────────────────────────────────────

  describe('selected (computed)', () => {
    it('returns undefined when no selection', async () => {
      mockApi.getSources.mockResolvedValue(SOURCES)
      mockApi.getSourceTypes.mockResolvedValue(TYPES)
      await store.fetchAll()
      expect(store.selected).toBeUndefined()
    })

    it('returns the matching source after selection', async () => {
      mockApi.getSources.mockResolvedValue(SOURCES)
      mockApi.getSourceTypes.mockResolvedValue(TYPES)
      await store.fetchAll()
      mockApi.getLibraries.mockResolvedValue(LIBS)
      mockApi.getCredentials.mockResolvedValue(CREDS)
      await store.select('s1')
      expect(store.selected?.source_id).toBe('s1')
      expect(store.selected?.display_name).toBe('Home Plex')
    })

    it('returns undefined after the selected source is removed', async () => {
      mockApi.getSources.mockResolvedValue(SOURCES)
      mockApi.getSourceTypes.mockResolvedValue(TYPES)
      await store.fetchAll()
      mockApi.getLibraries.mockResolvedValue(LIBS)
      mockApi.getCredentials.mockResolvedValue(CREDS)
      await store.select('s1')
      mockApi.deleteSource.mockResolvedValue(undefined)
      await store.removeSource('s1')
      expect(store.selected).toBeUndefined()
    })
  })

  // ── select ────────────────────────────────────────────────────────────────

  describe('select', () => {
    it('sets selectedId', async () => {
      mockApi.getLibraries.mockResolvedValue(LIBS)
      mockApi.getCredentials.mockResolvedValue(CREDS)
      await store.select('s1')
      expect(store.selectedId).toBe('s1')
    })

    it('fetches libraries and credentials in parallel', async () => {
      mockApi.getLibraries.mockResolvedValue(LIBS)
      mockApi.getCredentials.mockResolvedValue(CREDS)
      await store.select('s1')
      expect(mockApi.getLibraries).toHaveBeenCalledWith('s1')
      expect(mockApi.getCredentials).toHaveBeenCalledWith('s1')
    })

    it('resets libraries and available on re-selection', async () => {
      mockApi.getLibraries.mockResolvedValue(LIBS)
      mockApi.getCredentials.mockResolvedValue(CREDS)
      await store.select('s1')
      mockApi.getLibraries.mockResolvedValue([])
      await store.select('s2')
      expect(store.libraries).toHaveLength(0)
      expect(store.available).toHaveLength(0)
    })
  })

  // ── addSource ─────────────────────────────────────────────────────────────

  describe('addSource', () => {
    it('creates the source then refetches the full list', async () => {
      mockApi.createSource.mockResolvedValue({ source_id: 's3' })
      mockApi.getSources.mockResolvedValue([...SOURCES, { source_id: 's3', source_type: 'emby' as const, display_name: 'Emby', base_url: 'http://emby', enabled: true }])
      mockApi.getSourceTypes.mockResolvedValue(TYPES)
      const data = { source_id: 's3', source_type: 'emby' as const, display_name: 'Emby', base_url: 'http://emby' }
      await store.addSource(data)
      expect(mockApi.createSource).toHaveBeenCalledWith(data)
      expect(mockApi.getSources).toHaveBeenCalledTimes(1)
      expect(store.sources).toHaveLength(3)
    })
  })

  // ── removeSource ──────────────────────────────────────────────────────────

  describe('removeSource', () => {
    beforeEach(async () => {
      mockApi.getSources.mockResolvedValue(SOURCES)
      mockApi.getSourceTypes.mockResolvedValue(TYPES)
      await store.fetchAll()
      mockApi.deleteSource.mockResolvedValue(undefined)
    })

    it('calls deleteSource with correct id', async () => {
      await store.removeSource('s1')
      expect(mockApi.deleteSource).toHaveBeenCalledWith('s1')
    })

    it('removes source from local list', async () => {
      await store.removeSource('s1')
      expect(store.sources.find(s => s.source_id === 's1')).toBeUndefined()
      expect(store.sources).toHaveLength(1)
    })

    it('clears selection and libraries when removing selected source', async () => {
      mockApi.getLibraries.mockResolvedValue(LIBS)
      mockApi.getCredentials.mockResolvedValue(CREDS)
      await store.select('s1')
      await store.removeSource('s1')
      expect(store.selectedId).toBeNull()
      expect(store.libraries).toHaveLength(0)
    })

    it('does not clear selection when removing a non-selected source', async () => {
      mockApi.getLibraries.mockResolvedValue(LIBS)
      mockApi.getCredentials.mockResolvedValue(CREDS)
      await store.select('s1')
      await store.removeSource('s2')
      expect(store.selectedId).toBe('s1')
    })
  })

  // ── library management ────────────────────────────────────────────────────

  describe('fetchAvailable', () => {
    it('populates available libraries', async () => {
      mockApi.getAvailableLibs.mockResolvedValue([AVAIL_LIB])
      await store.fetchAvailable('s1')
      expect(store.available).toHaveLength(1)
      expect(store.available[0].name).toBe('TV Shows')
    })
  })

  describe('addLibrary', () => {
    it('calls addLibrary API then refreshes library list', async () => {
      mockApi.addLibrary.mockResolvedValue({ library_id: 'lib2' })
      mockApi.getLibraries.mockResolvedValue([...LIBS, { ...LIB_1, library_id: 'lib2', external_lib_id: 'extB' }])
      await store.addLibrary('s1', 'extB', 'TV', 'show')
      expect(mockApi.addLibrary).toHaveBeenCalledWith('s1', { external_lib_id: 'extB', display_name: 'TV', library_type: 'show' })
      expect(mockApi.getLibraries).toHaveBeenCalledWith('s1')
      expect(store.libraries).toHaveLength(2)
    })
  })

  describe('removeLibrary', () => {
    it('calls removeLibrary API and removes from local list', async () => {
      mockApi.getLibraries.mockResolvedValue(LIBS)
      mockApi.getCredentials.mockResolvedValue(CREDS)
      await store.select('s1')
      mockApi.removeLibrary.mockResolvedValue(undefined)
      await store.removeLibrary('s1', 'lib1')
      expect(mockApi.removeLibrary).toHaveBeenCalledWith('s1', 'lib1')
      expect(store.libraries.find(l => l.library_id === 'lib1')).toBeUndefined()
    })
  })

  // ── credentials ───────────────────────────────────────────────────────────

  describe('credentials', () => {
    it('fetchCredentials stores result keyed by sourceId', async () => {
      mockApi.getCredentials.mockResolvedValue(CREDS)
      await store.fetchCredentials('s1')
      expect(store.credentials['s1']).toEqual(CREDS)
    })

    it('setCredentials updates credentials optimistically', async () => {
      mockApi.setCredentials.mockResolvedValue({ ok: true })
      await store.setCredentials('s1', 'tok123')
      expect(mockApi.setCredentials).toHaveBeenCalledWith('s1', { token: 'tok123', user_id: '' })
      expect(store.credentials['s1']).toEqual({ has_token: true, has_user_id: false })
    })

    it('setCredentials marks has_user_id=true when userId provided', async () => {
      mockApi.setCredentials.mockResolvedValue({ ok: true })
      await store.setCredentials('s1', 'tok', 'user123')
      expect(store.credentials['s1']).toEqual({ has_token: true, has_user_id: true })
    })

    it('deleteCredentials clears credential flags', async () => {
      mockApi.deleteCredentials.mockResolvedValue({ ok: true })
      await store.deleteCredentials('s1')
      expect(mockApi.deleteCredentials).toHaveBeenCalledWith('s1')
      expect(store.credentials['s1']).toEqual({ has_token: false, has_user_id: false })
    })
  })

  // ── triggerSync ───────────────────────────────────────────────────────────

  describe('triggerSync', () => {
    it('calls triggerSync API with correct source id', async () => {
      mockApi.triggerSync.mockResolvedValue({ status: 'started' })
      await store.triggerSync('s1')
      expect(mockApi.triggerSync).toHaveBeenCalledWith('s1')
    })

    it('sets syncing=false after completion', async () => {
      mockApi.triggerSync.mockResolvedValue({ status: 'started' })
      await store.triggerSync('s1')
      expect(store.syncing).toBe(false)
    })

    it('sets syncing=false even on error', async () => {
      mockApi.triggerSync.mockRejectedValue(new Error('timeout'))
      await store.triggerSync('s1').catch(() => {})
      expect(store.syncing).toBe(false)
    })
  })
})
