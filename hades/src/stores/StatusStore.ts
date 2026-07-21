import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'

export class StatusStore {
  syncing      = false
  matching     = false
  writingBack  = false
  syncDebug    = false
  epgDebug     = false
  // Read by remoteLog.ts to decide whether console.error calls get
  // forwarded to the server log; kept fresh here the same way sync/epg
  // debug are, so a toggle flipped in another tab/session takes effect
  // without a reload.
  hadesDebug   = false
  // Which source's content-ingestion phase is running right now — undefined
  // when idle, or during phases that aren't per-source (orphan cleanup,
  // scraper matching, chapter sync). Drives the Activity page's per-source
  // "currently active" indicator.
  currentSourceId: string | undefined = undefined

  private _timer: ReturnType<typeof setTimeout> | null = null

  constructor() {
    makeAutoObservable(this, { _timer: false } as any)
  }

  get anyRunning() {
    return this.syncing || this.matching
  }

  // Separate from anyRunning deliberately — the Sync Status card's "Sync in
  // progress" label is keyed off anyRunning, and a writeback run isn't a
  // sync; it gets its own indicator (see ActivityPage's Writeback card).
  // Still worth blocking a sync-triggering button on, hence this combined
  // getter for that specific disabled= check.
  get anyBusy() {
    return this.syncing || this.matching || this.writingBack
  }

  get anyDebugEnabled() {
    return this.syncDebug || this.epgDebug
  }

  // Optimistic flip for the instant after a trigger request succeeds —
  // otherwise the indicator can lag up to the idle 15s poll interval before
  // the first fast (2s) poll confirms it. Self-corrects on the next poll
  // regardless (server is the source of truth), so a false positive here
  // can't stick around.
  markWritebackStarted() {
    this.writingBack = true
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
      const [sync, match, writeback, settings] = await Promise.all([
        api.getSyncStatus(),
        api.getMatchStatus(),
        api.getWritebackStatus(),
        api.getSettings(),
      ])
      runInAction(() => {
        this.syncing         = sync.running
        this.matching        = match.running
        this.writingBack     = writeback.running
        this.syncDebug       = settings.sync_debug
        this.epgDebug        = settings.epg_debug
        this.hadesDebug      = settings.hades_debug
        this.currentSourceId = sync.current_source_id
      })
    } catch {}
    this._timer = setTimeout(() => this._poll(), (this.anyRunning || this.writingBack) ? 2000 : 15000)
  }
}

export const statusStore = new StatusStore()
