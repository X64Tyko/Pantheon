import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'
import type { User } from '../api/types'

export class UserStore {
  users:   User[]        = []
  loading: boolean       = false
  error:   string | null = null

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
}
