import { makeAutoObservable, runInAction } from 'mobx'
import { api, TOKEN_KEY } from '../api/client'
import type { UnmappedSourceUser } from '../api/types'

const DISMISSED_UNMAPPED_KEY = 'pantheon_dismissed_unmapped_users'

function unmappedUserKey(u: UnmappedSourceUser): string {
  return `${u.source_id}:${u.external_user_id}`
}

function loadDismissedUnmappedUsers(): Set<string> {
  try {
    const raw = localStorage.getItem(DISMISSED_UNMAPPED_KEY)
    return new Set(raw ? (JSON.parse(raw) as string[]) : [])
  } catch { return new Set() }
}

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
  // localStorage-backed-preference pattern, same as TourStore/HelpTipsStore —
  // persists per-browser across reloads/logins so dismissing the notice
  // actually sticks, instead of reappearing every visit (see
  // unmappedUsersDismissed below for why it's keyed per-user, not a flag).
  dismissedUnmappedUsers: Set<string> = loadDismissedUnmappedUsers()

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

  // True once every currently-detected unmapped user has already been
  // dismissed — persisted in localStorage (dismissedUnmappedUsers), so a
  // fresh login/reload doesn't re-show the notice for accounts the admin
  // already saw and chose to ignore. Comparing by identity (not just count)
  // means a newly-detected profile reopens the notice even if it happens to
  // land on the same total count as before (e.g. one dismissed account got
  // imported the same poll cycle a different new one appeared).
  get unmappedUsersDismissed(): boolean {
    return this.unmappedSourceUsers.every(u => this.dismissedUnmappedUsers.has(unmappedUserKey(u)))
  }

  dismissUnmappedUsersNotice() {
    for (const u of this.unmappedSourceUsers) this.dismissedUnmappedUsers.add(unmappedUserKey(u))
    localStorage.setItem(DISMISSED_UNMAPPED_KEY, JSON.stringify([...this.dismissedUnmappedUsers]))
  }

  private async _pollUnmappedUsers() {
    try {
      const users = await api.getUnmappedSourceUsers()
      runInAction(() => { this.unmappedSourceUsers = users })
    } catch {}
    this._unmappedTimer = setTimeout(() => this._pollUnmappedUsers(), 5 * 60 * 1000)
  }
}
