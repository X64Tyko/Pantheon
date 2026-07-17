import { observer } from 'mobx-react-lite'
import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import type { DeviceConnection } from '../../api/types'
import { channelStore, userStore } from '../../stores'

const POLL_MS = 5_000
// Roku's own long-poll/state-post cadence is a few seconds; comfortably past
// that means the device has actually dropped, not just between beats.
const STALE_MS = 20_000

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
// re-fetch the same title every 5s.
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

export const DeviceConnectionsPanel = observer(function DeviceConnectionsPanel() {
  const [connections, setConnections] = useState<DeviceConnection[]>([])
  const [error, setError] = useState<string | null>(null)
  const resolveTitle = useTitleCache()

  useEffect(() => {
    channelStore.channels.length === 0 && channelStore.fetchAll()
    userStore.users.length === 0 && userStore.fetchAll()
  }, [])

  useEffect(() => {
    let cancelled = false
    const poll = () => {
      api.getAllDeviceConnections()
        .then(list => { if (!cancelled) { setConnections(list); setError(null) } })
        // 403 here just means "not an admin" — this panel shouldn't render
        // at all for a non-admin viewer (see ActivityPage's own admin gate),
        // but fail quiet rather than looping an error into the log.
        .catch(e => { if (!cancelled) setError(e instanceof Error ? e.message : 'failed to load') })
    }
    poll()
    const interval = setInterval(poll, POLL_MS)
    return () => { cancelled = true; clearInterval(interval) }
  }, [])

  const live = connections.filter(c => Date.now() - c.last_seen_ms < STALE_MS)

  return (
    <div className="rounded-lg border border-violet-900/50 bg-zinc-900 p-4 shrink-0">
      <div className="flex items-center justify-between mb-3">
        <h2 className="section-label">Connected Devices</h2>
        <span className="text-xs text-zinc-400">
          <span className="text-zinc-100 font-semibold text-sm">{live.length}</span> Roku{live.length === 1 ? '' : 's'} connected
        </span>
      </div>

      {error ? (
        <p className="text-xs text-red-400">{error}</p>
      ) : live.length === 0 ? (
        <p className="text-xs text-zinc-600">No Roku devices currently connected.</p>
      ) : (
        <div className="flex flex-col gap-2">
          {live.map(c => {
            const user  = userStore.users.find(u => u.user_id === c.user_id)
            const title = resolveTitle(c.state.contentType, c.state.contentId)
            return (
              <div key={c.id}
                   className="flex items-center gap-3 px-3 py-2 rounded border text-xs
                              bg-zinc-950/60 border-zinc-800/60">
                <span className={`w-1.5 h-1.5 rounded-full shrink-0 ${
                  c.state.playing ? 'bg-emerald-400' : 'bg-zinc-600'
                }`} />
                <span className="text-zinc-300 font-medium shrink-0">{user?.username ?? c.user_id}</span>
                <span className="text-zinc-500 truncate max-w-[320px]">
                  {c.state.contentType
                    ? (title ?? 'Loading…')
                    : 'Connected — nothing playing'}
                </span>
                {c.state.contentType && (
                  <span className="text-zinc-600 uppercase shrink-0">{c.state.contentType}</span>
                )}
                <span className="text-zinc-600 ml-auto shrink-0">{elapsedAgo(c.last_seen_ms)}</span>
              </div>
            )
          })}
        </div>
      )}
    </div>
  )
})
