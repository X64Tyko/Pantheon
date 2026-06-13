import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'
import type { Channel } from '../api/types'

export class ChannelStore {
  channels: Channel[] = []
  loading:  boolean   = false
  error:    string | null = null

  constructor() {
    makeAutoObservable(this)
  }

  async fetchAll() {
    this.loading = true
    this.error   = null
    try {
      const channels = await api.getChannels()
      runInAction(() => { this.channels = channels; this.loading = false })
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.loading = false })
    }
  }

  async add(data: Omit<Channel, 'channel_id'>) {
    await api.createChannel(data)
    await this.fetchAll()
  }

  async remove(id: string) {
    await api.deleteChannel(id)
    runInAction(() => {
      const idx = this.channels.findIndex(c => c.channel_id === id)
      if (idx !== -1) this.channels.splice(idx, 1)
    })
  }
}
