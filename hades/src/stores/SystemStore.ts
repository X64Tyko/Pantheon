import { makeAutoObservable, runInAction } from 'mobx'
import { api, TOKEN_KEY } from '../api/client'
import type { UnmappedSourceUser } from '../api/types'

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
  logs:         LogEntry[]      = []
  liveStatus:   'connecting' | 'live' | 'disconnected' = 'disconnected'
  unreadErrors: number          = 0
  toast:        ErrorToast | null = null

  // Source-reported accounts with no local Pantheon account imported yet —
  // drives the persistent corner notification + SourcesPage's pill.
  unmappedSourceUsers: UnmappedSourceUser[] = []
  unmappedUsersDismissed = false

  private _es:            EventSource | null = null
  private _toastTimer:    ReturnType<typeof setTimeout> | null = null
  private _unmappedTimer: ReturnType<typeof setTimeout> | null = null

  constructor() {
    makeAutoObservable(this, {
      _es:            false,
      _toastTimer:    false,
      _unmappedTimer: false,
    } as any)
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

  // ── Unmapped source users ───────────────────────────────────────────────────

  startUnmappedUsersPolling() {
    if (this._unmappedTimer) return
    this._pollUnmappedUsers()
  }

  stopUnmappedUsersPolling() {
    if (this._unmappedTimer) { clearTimeout(this._unmappedTimer); this._unmappedTimer = null }
  }

  // One-shot fetch, outside the poll cadence — called right after an import
  // so SourcesPage's pill and the corner notice both drop the row immediately
  // instead of waiting up to 5 minutes for the next scheduled poll.
  async refreshUnmappedUsers() {
    try {
      const users = await api.getUnmappedSourceUsers()
      runInAction(() => { this.unmappedSourceUsers = users })
    } catch {}
  }

  // Session-only dismiss (not persisted) — reappears next reload until the
  // list is actually resolved (imported), since there's no other "seen"
  // state to track against yet.
  dismissUnmappedUsersNotice() {
    this.unmappedUsersDismissed = true
  }

  private async _pollUnmappedUsers() {
    try {
      const users = await api.getUnmappedSourceUsers()
      runInAction(() => {
        // A newly-discovered user re-opens a notice the admin already
        // dismissed this session — the count changed, so it's new information.
        if (users.length > this.unmappedSourceUsers.length) this.unmappedUsersDismissed = false
        this.unmappedSourceUsers = users
      })
    } catch {}
    this._unmappedTimer = setTimeout(() => this._pollUnmappedUsers(), 5 * 60 * 1000)
  }
}
