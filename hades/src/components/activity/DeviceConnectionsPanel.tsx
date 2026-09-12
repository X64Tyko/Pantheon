import { observer } from 'mobx-react-lite'
import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import type { DeviceConnection, PlaybackHistoryEntry } from '../../api/types'
import { channelStore, userStore } from '../../stores'

const POLL_MS = 5_000
// Roku's own long-poll/state-post cadence is a few seconds; comfortably past
// that means the device has actually dropped, not just between beats.
const STALE_MS = 20_000

const DEVICE_LABELS: Record<PlaybackHistoryEntry['device_type'], string> = {
  web: 'Web', 'android-mobile': 'Android', 'android-tv': 'Android TV', roku: 'Roku', cast: 'Cast', '': 'Unknown',
}

function elapsedAgo(ms: number): string {
  const secs = Math.max(0, Math.floor((Date.now() - ms) / 1000))
  if (secs < 60) return `${secs}s ago`
  const mins = Math.floor(secs / 60)
  if (mins < 60) return `${mins}m ago`
  return `${Math.floor(mins / 60)}h ${mins % 60}m ago`
}

// contentType/contentId are the bare identifiers Roku's own heartbeat
// reports (see PlayerScreen.brs) — no title threaded through that whole
// call chain just for this, so it's resolved here on demand instead:
// channels come from the already-loaded channelStore for free; movies/
// episodes need a per-id lookup, cached by id so a repeated poll doesn't
// re-fetch the same title every 5s. Cross-platform active sessions (see
// below) already carry a resolved title from the server, so this cache is
// Roku-only.
function useTitleCache() {
  const [cache, setCache] = useState<Record<string, string>>({})

  const resolve = (contentType: string | undefined, contentId: string | undefined) => {
    if (!contentType || !contentId) return null
    const key = `${contentType}:${contentId}`
    if (cache[key]) return cache[key]

    if (contentType === 'channel') {
      const ch = channelStore.channels.find(c => c.channel_id === contentId)
      if (ch) { setCache(c => ({ ...c, [key]: ch.name })); return ch.name }
      return null
    }

    // Fire the lookup once, cache whatever comes back (or a "not found"
    // placeholder so a deleted/bad id doesn't get re-fetched every poll).
    if (!(key in cache)) {
      setCache(c => ({ ...c, [key]: '' })) // claim the slot immediately
      const req = contentType === 'movie' ? api.getMovie(contentId)
        : contentType === 'episode' ? api.getEpisodeBrief(contentId).then(e => ({ title: `${e.show_title} S${e.season}E${e.episode} - ${e.title}` }))
        : Promise.resolve(null)
      req
        .then(r => setCache(c => ({ ...c, [key]: r?.title ?? '(unknown content)' })))
        .catch(() => setCache(c => ({ ...c, [key]: '(unknown content)' })))
    }
    return null
  }

  return resolve
}

// One row regardless of source — Roku's own ECP heartbeat (DeviceConnection,
// which also reports idle-but-connected and true playing/paused state) or a
// recent watch-progress ping from any other platform (PlaybackHistoryEntry,
// see kairos's GET /api/activity/active — no idle-but-connected concept for
// these, only "was pinging within the staleness window", so `playing` is
// left null rather than guessed).
interface UnifiedDevice {
  key:         string
  userId:      string
  deviceLabel: string
  playing:     boolean | null
  contentType?: string
  contentId?:   string
  title?:       string | null
  lastSeenMs:  number
}

export const DeviceConnectionsPanel = observer(function DeviceConnectionsPanel() {
  const [connections, setConnections]   = useState<DeviceConnection[]>([])
  const [activeOther, setActiveOther]   = useState<PlaybackHistoryEntry[]>([])
  const [error, setError] = useState<string | null>(null)
  const resolveTitle = useTitleCache()

  useEffect(() => {
    channelStore.channels.length === 0 && channelStore.fetchAll()
    userStore.users.length === 0 && userStore.fetchAll()
  }, [])

  useEffect(() => {
    let cancelled = false
    const poll = () => {
      Promise.all([api.getAllDeviceConnections(), api.getActiveSessions()])
        .then(([roku, active]) => {
          if (cancelled) return
          setConnections(roku)
          // Roku already has its own richer (playing/paused, idle-but-
          // connected) source above — drop it here rather than show every
          // Roku session twice, since the native channel app also pings
          // watch-progress like every other platform does.
          setActiveOther(active.filter(a => a.device_type !== 'roku'))
          setError(null)
        })
        // 403 here just means "not an admin" — this panel shouldn't render
        // at all for a non-admin viewer (see ActivityPage's own admin gate),
        // but fail quiet rather than looping an error into the log.
        .catch(e => { if (!cancelled) setError(e instanceof Error ? e.message : 'failed to load') })
    }
    poll()
    const interval = setInterval(poll, POLL_MS)
    return () => { cancelled = true; clearInterval(interval) }
  }, [])

  const rokuLive: UnifiedDevice[] = connections
    .filter(c => Date.now() - c.last_seen_ms < STALE_MS)
    .map(c => ({
      key: c.id, userId: c.user_id, deviceLabel: 'Roku', playing: c.state.playing ?? false,
      contentType: c.state.contentType, contentId: c.state.contentId,
      title: resolveTitle(c.state.contentType, c.state.contentId),
      lastSeenMs: c.last_seen_ms,
    }))
  const otherLive: UnifiedDevice[] = activeOther.map(a => ({
    key: a.event_id, userId: a.user_id, deviceLabel: DEVICE_LABELS[a.device_type], playing: null,
    contentType: a.content_type, contentId: a.content_id, title: a.title,
    lastSeenMs: a.ended_at_ms,
  }))
  const live = [...rokuLive, ...otherLive].sort((a, b) => b.lastSeenMs - a.lastSeenMs)

  return (
    <div className="rounded-lg border border-violet-900/50 bg-zinc-900 p-4 shrink-0">
      <div className="flex items-center justify-between mb-3">
        <h2 className="section-label">Connected Devices</h2>
        <span className="text-xs text-zinc-400">
          <span className="text-zinc-100 font-semibold text-sm">{live.length}</span> device{live.length === 1 ? '' : 's'} connected
        </span>
      </div>

      {error ? (
        <p className="text-xs text-red-400">{error}</p>
      ) : live.length === 0 ? (
        <p className="text-xs text-zinc-600">No devices currently connected.</p>
      ) : (
        <div className="flex flex-col gap-2">
          {live.map(d => {
            const user = userStore.users.find(u => u.user_id === d.userId)
            return (
              <div key={d.key}
                   className="flex items-center gap-3 px-3 py-2 rounded border text-xs
                              bg-zinc-950/60 border-zinc-800/60">
                <span className={`w-1.5 h-1.5 rounded-full shrink-0 ${
                  d.playing === false ? 'bg-zinc-600' : 'bg-emerald-400'
                }`} />
                <span className="text-zinc-300 font-medium shrink-0">{user?.username ?? d.userId}</span>
                <span className="text-zinc-500 truncate max-w-[320px]">
                  {d.contentType
                    ? (d.title ?? 'Loading…')
                    : 'Connected — nothing playing'}
                </span>
                {d.contentType && (
                  <span className="text-zinc-600 uppercase shrink-0">{d.contentType}</span>
                )}
                <span className="text-violet-400 shrink-0">{d.deviceLabel}</span>
                <span className="text-zinc-600 ml-auto shrink-0">{elapsedAgo(d.lastSeenMs)}</span>
              </div>
            )
          })}
        </div>
      )}
    </div>
  )
})
