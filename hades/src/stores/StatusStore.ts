import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'

class StatusStore {
  syncing  = false
  matching = false

  private _timer: ReturnType<typeof setTimeout> | null = null

  constructor() {
    makeAutoObservable(this, { _timer: false } as any)
  }

  get anyRunning() {
    return this.syncing || this.matching
  }

  startPolling() {
    if (this._timer) return
    this._poll()
  }

  stopPolling() {
    if (this._timer) { clearTimeout(this._timer); this._timer = null }
  }

  private async _poll() {
    try {
      const [sync, match] = await Promise.all([
        api.getSyncStatus(),
        api.getMatchStatus(),
      ])
      runInAction(() => {
        this.syncing  = sync.running
        this.matching = match.running
      })
    } catch {}
    this._timer = setTimeout(() => this._poll(), this.anyRunning ? 2000 : 15000)
  }
}

export const statusStore = new StatusStore()
