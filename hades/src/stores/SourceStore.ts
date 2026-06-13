import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'
import type { CredentialStatus, Library, LibraryInfo, Source, SourceType } from '../api/types'

export class SourceStore {
  sources:      Source[]     = []
  sourceTypes:  SourceType[] = []
  selectedId:   string | null = null
  libraries:    Library[]    = []
  available:    LibraryInfo[] = []
  credentials:  Record<string, CredentialStatus> = {}
  syncing:      boolean      = false
  loading:      boolean      = false
  error:        string | null = null

  constructor() {
    makeAutoObservable(this)
  }

  get selected(): Source | undefined {
    return this.sources.find(s => s.source_id === this.selectedId)
  }

  async fetchAll() {
    this.loading = true
    this.error   = null
    try {
      const [sources, types] = await Promise.all([api.getSources(), api.getSourceTypes()])
      runInAction(() => {
        this.sources     = sources
        this.sourceTypes = types
        this.loading     = false
      })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.loading = false })
    }
  }

  async select(id: string) {
    this.selectedId = id
    this.libraries  = []
    this.available  = []
    await Promise.all([this.fetchLibraries(id), this.fetchCredentials(id)])
  }

  async fetchLibraries(sourceId: string) {
    try {
      const libs = await api.getLibraries(sourceId)
      runInAction(() => { this.libraries = libs })
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    }
  }

  async fetchAvailable(sourceId: string) {
    try {
      const avail = await api.getAvailableLibs(sourceId)
      runInAction(() => { this.available = avail })
    } catch (e: any) {
      runInAction(() => { this.error = e.message })
    }
  }

  async addSource(data: Omit<Source, 'enabled'>) {
    await api.createSource(data)
    await this.fetchAll()
  }

  async fetchCredentials(sourceId: string) {
    try {
      const c = await api.getCredentials(sourceId)
      runInAction(() => { this.credentials[sourceId] = c })
    } catch {}
  }

  async setCredentials(sourceId: string, token: string, userId = '') {
    await api.setCredentials(sourceId, { token, user_id: userId })
    runInAction(() => {
      this.credentials[sourceId] = { has_token: !!token, has_user_id: !!userId }
    })
  }

  async deleteCredentials(sourceId: string) {
    await api.deleteCredentials(sourceId)
    runInAction(() => {
      this.credentials[sourceId] = { has_token: false, has_user_id: false }
    })
  }

  async removeSource(id: string) {
    await api.deleteSource(id)
    runInAction(() => {
      const idx = this.sources.findIndex(s => s.source_id === id)
      if (idx !== -1) this.sources.splice(idx, 1)
      if (this.selectedId === id) { this.selectedId = null; this.libraries = [] }
    })
  }

  async addLibrary(sourceId: string, external_lib_id: string, display_name: string, library_type: Library['library_type']) {
    await api.addLibrary(sourceId, { external_lib_id, display_name, library_type })
    await this.fetchLibraries(sourceId)
  }

  async removeLibrary(sourceId: string, libraryId: string) {
    await api.removeLibrary(sourceId, libraryId)
    runInAction(() => {
      const idx = this.libraries.findIndex(l => l.library_id === libraryId)
      if (idx !== -1) this.libraries.splice(idx, 1)
    })
  }

  async triggerSync(sourceId: string) {
    this.syncing = true
    try {
      await api.triggerSync(sourceId)
    } finally {
      runInAction(() => { this.syncing = false })
    }
  }
}
