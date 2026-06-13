import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'

export class SystemStore {
  syncing = false
  private _timer: ReturnType<typeof setTimeout> | null = null

  constructor() { makeAutoObservable(this) }

  startPolling() {
    if (this._timer) return
    this._poll()
  }

  stopPolling() {
    if (this._timer) { clearTimeout(this._timer); this._timer = null }
  }

  private async _poll() {
    await this._fetchStatus()
    // Poll every 2s while a sync is running; every 15s when idle.
    this._timer = setTimeout(() => this._poll(), this.syncing ? 2000 : 15000)
  }

  private async _fetchStatus() {
    try {
      const { running } = await api.getSyncStatus()
      runInAction(() => { this.syncing = running })
    } catch {}
  }
}
