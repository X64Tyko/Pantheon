import { makeAutoObservable, runInAction } from 'mobx'
import { api, TOKEN_KEY } from '../api/client'

export interface LogEntry {
  id:      number
  ts:      string
  line:    string
  isError: boolean
}

export interface ErrorToast {
  id:  number
  msg: string
  ts:  string
}

let _logId = 0

export class SystemStore {
  syncing       = false
  logs:         LogEntry[]      = []
  liveStatus:   'connecting' | 'live' | 'disconnected' = 'disconnected'
  unreadErrors: number          = 0
  toast:        ErrorToast | null = null

  private _pollTimer:  ReturnType<typeof setTimeout> | null = null
  private _es:         EventSource | null = null
  private _toastTimer: ReturnType<typeof setTimeout> | null = null

  constructor() {
    makeAutoObservable(this, {
      _pollTimer:  false,
      _es:         false,
      _toastTimer: false,
    } as any)
  }

  // ── Sync polling ─────────────────────────────────────────────────────────────

  startPolling() {
    if (this._pollTimer) return
    this._poll()
  }

  stopPolling() {
    if (this._pollTimer) { clearTimeout(this._pollTimer); this._pollTimer = null }
  }

  private async _poll() {
    await this._fetchStatus()
    this._pollTimer = setTimeout(() => this._poll(), this.syncing ? 2000 : 15000)
  }

  private async _fetchStatus() {
    try {
      const { running } = await api.getSyncStatus()
      runInAction(() => { this.syncing = running })
    } catch {}
  }

  // ── Log stream ───────────────────────────────────────────────────────────────

  connectLogs() {
    if (this._es) return
    this._openSSE()
  }

  private _openSSE() {
    const token = localStorage.getItem(TOKEN_KEY)
    const url   = token ? `/api/logs/stream?token=${encodeURIComponent(token)}` : '/api/logs/stream'
    const es = new EventSource(url)
    this._es = es
    runInAction(() => { this.liveStatus = 'connecting' })

    es.onopen = () => runInAction(() => { this.liveStatus = 'live' })

    es.onerror = () => {
      runInAction(() => { this.liveStatus = 'disconnected' })
      es.close()
      this._es = null
      setTimeout(() => { if (!this._es) this._openSSE() }, 5000)
    }

    es.onmessage = (e: MessageEvent) => {
      const line: string = e.data
      const isError = /^\[error\]/i.test(line)
      const entry: LogEntry = {
        id:      _logId++,
        ts:      new Date().toLocaleTimeString('en-US', { hour12: false }),
        line,
        isError,
      }
      runInAction(() => {
        this.logs.push(entry)
        if (this.logs.length > 1000) this.logs.splice(0, this.logs.length - 1000)

        if (isError) {
          this.unreadErrors++
          // Strip the [error] tag to get a human-readable summary.
          const msg = line.replace(/^\[[^\]]+\]\s*/, '').slice(0, 160)
          if (this._toastTimer) clearTimeout(this._toastTimer)
          this.toast = { id: entry.id, msg, ts: entry.ts }
          this._toastTimer = setTimeout(
            () => runInAction(() => { this.toast = null }),
            8000,
          )
        }
      })
    }
  }

  clearUnreadErrors() {
    this.unreadErrors = 0
  }

  dismissToast() {
    if (this._toastTimer) { clearTimeout(this._toastTimer); this._toastTimer = null }
    this.toast = null
  }
}
