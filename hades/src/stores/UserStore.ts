import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'
import type { ContentOverride, User } from '../api/types'

export class UserStore {
  users:   User[]        = []
  loading: boolean       = false
  error:   string | null = null

  // Keyed by user_id — only fetched on demand when the override manager for
  // that user is opened, not as part of the main user list.
  overrides: Record<string, ContentOverride[]> = {}

  constructor() {
    makeAutoObservable(this)
  }

  async fetchAll() {
    this.loading = true
    this.error   = null
    try {
      const users = await api.getUsers()
      runInAction(() => { this.users = users; this.loading = false })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.loading = false })
    }
  }

  async create(username: string, password: string, role: 'admin' | 'viewer') {
    await api.createUser(username, password, role)
    await this.fetchAll()
  }

  async update(userId: string, data: { password?: string; role?: 'admin' | 'viewer' }) {
    await api.updateUser(userId, data)
    await this.fetchAll()
  }

  async remove(userId: string) {
    await api.deleteUser(userId)
    runInAction(() => {
      this.users = this.users.filter(u => u.user_id !== userId)
    })
  }

  async updateRestriction(userId: string, patch: { restricted: boolean; max_tv_rating: string; max_movie_rating: string; max_channel_rating: string }) {
    await api.updateUserRestriction(userId, patch)
    await this.fetchAll()
  }

  async fetchOverrides(userId: string) {
    const list = await api.getUserOverrides(userId)
    runInAction(() => { this.overrides[userId] = list })
  }

  async addOverride(userId: string, o: ContentOverride) {
    await api.addUserOverride(userId, o)
    await this.fetchOverrides(userId)
  }

  async removeOverride(userId: string, entityType: string, entityId: string) {
    await api.removeUserOverride(userId, entityType, entityId)
    runInAction(() => {
      this.overrides[userId] = (this.overrides[userId] ?? []).filter(
        o => !(o.entity_type === entityType && o.entity_id === entityId))
    })
  }
}
