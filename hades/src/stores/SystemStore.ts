import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'

export class SystemStore {
  syncing = false
  private _timer: ReturnType<typeof setInterval> | null = null

  constructor() { makeAutoObservable(this) }

  startPolling() {
    if (this._timer) return
    this._fetchStatus()
    this._timer = setInterval(() => this._fetchStatus(), 2000)
  }

  stopPolling() {
    if (this._timer) { clearInterval(this._timer); this._timer = null }
  }

  private async _fetchStatus() {
    try {
      const { running } = await api.getSyncStatus()
      runInAction(() => { this.syncing = running })
    } catch {}
  }
}
