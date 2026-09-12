import { observer } from 'mobx-react-lite'
import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import type { PlaybackHistoryEntry } from '../../api/types'
import { userStore } from '../../stores'

const POLL_MS = 15_000
const PAGE_SIZE = 100

const DEVICE_LABELS: Record<PlaybackHistoryEntry['device_type'], string> = {
  web: 'Web', 'android-mobile': 'Android', 'android-tv': 'Android TV', roku: 'Roku', cast: 'Cast', '': 'Unknown',
}

function relativeTime(ms: number): string {
  const secs = Math.max(0, Math.floor((Date.now() - ms) / 1000))
  if (secs < 60) return `${secs}s ago`
  const mins = Math.floor(secs / 60)
  if (mins < 60) return `${mins}m ago`
  const hours = Math.floor(mins / 60)
  if (hours < 24) return `${hours}h ago`
  return `${Math.floor(hours / 24)}d ago`
}

function formatDuration(ms: number): string {
  const secs = Math.round(ms / 1000)
  const h = Math.floor(secs / 3600)
  const m = Math.floor((secs % 3600) / 60)
  const s = secs % 60
  return h > 0 ? `${h}h ${m}m` : m > 0 ? `${m}m ${s}s` : `${s}s`
}

// Tautulli-style past-sessions table — see kairos/src/db/PlaybackHistoryRepository.h.
// Append-only: a row per "sitting" (piggybacked on the player's own
// watch-progress pings), never edited except to extend ended_at_ms/
// last_position_ms while the same sitting continues. Admin sees everyone by
// default; a non-admin viewer only ever gets their own rows back regardless
// (enforced server-side, not just hidden here).
export const PlayHistoryPanel = observer(function PlayHistoryPanel() {
  const [rows,  setRows]  = useState<PlaybackHistoryEntry[]>([])
  const [error, setError] = useState<string | null>(null)
  const [userFilter, setUserFilter] = useState('')

  useEffect(() => {
    userStore.users.length === 0 && userStore.fetchAll()
  }, [])

  useEffect(() => {
    let cancelled = false
    const poll = () => {
      api.getActivityHistory({ userId: userFilter || undefined, limit: PAGE_SIZE })
        .then(list => { if (!cancelled) { setRows(list); setError(null) } })
        .catch(e => { if (!cancelled) setError(e instanceof Error ? e.message : 'failed to load') })
    }
    poll()
    const interval = setInterval(poll, POLL_MS)
    return () => { cancelled = true; clearInterval(interval) }
  }, [userFilter])

  return (
    <div className="rounded-lg border border-violet-900/50 bg-zinc-900 p-4 shrink-0">
      <div className="flex items-center justify-between mb-3">
        <h2 className="section-label">Play History</h2>
        {userStore.users.length > 1 && (
          <select
            value={userFilter}
            onChange={e => setUserFilter(e.target.value)}
            className="text-xs bg-zinc-950/60 border border-zinc-800/60 rounded px-2 py-1 text-zinc-300"
          >
            <option value="">Everyone</option>
            {userStore.users.map(u => (
              <option key={u.user_id} value={u.user_id}>{u.username}</option>
            ))}
          </select>
        )}
      </div>

      {error ? (
        <p className="text-xs text-red-400">{error}</p>
      ) : rows.length === 0 ? (
        <p className="text-xs text-zinc-600">No playback history yet.</p>
      ) : (
        <div className="overflow-x-auto">
          <table className="w-full text-xs">
            <thead>
              <tr className="text-zinc-600 text-left">
                <th className="py-1 pr-4 font-normal">Title</th>
                <th className="py-1 pr-4 font-normal">User</th>
                <th className="py-1 pr-4 font-normal">Device</th>
                  <th className="py-1 pr-4 font-normal">Direct Stream</th>
                <th className="py-1 pr-4 font-normal">Progress</th>
                <th className="py-1 pr-4 font-normal">When</th>
              </tr>
            </thead>
            <tbody>
              {rows.map(r => {
                const user = userStore.users.find(u => u.user_id === r.user_id)
                  const isChannel = r.content_type === 'channel'
                const pct = r.duration_ms > 0 ? Math.min(1, r.last_position_ms / r.duration_ms) : 0
                return (
                  <tr key={r.event_id} className="border-t border-zinc-800/60">
                      <td className="py-2 pr-4 text-zinc-300 max-w-[280px] truncate">
                          {isChannel && <span className="text-emerald-500 mr-1.5" title="Live channel">&#9679;</span>}
                          {r.title || r.content_id}
                      </td>
                    <td className="py-2 pr-4 text-zinc-400 whitespace-nowrap">{user?.username ?? r.user_id.slice(0, 8)}</td>
                    <td className="py-2 pr-4 text-zinc-500 whitespace-nowrap">{DEVICE_LABELS[r.device_type]}</td>
                    <td className="py-2 pr-4 whitespace-nowrap">
                      <span className={r.direct_stream ? 'text-emerald-400' : 'text-amber-400'}>
                        {r.direct_stream ? 'Direct' : 'Transcode'}
                      </span>
                    </td>
                    <td className="py-2 pr-4 whitespace-nowrap">
                        {isChannel ? (
                            // Channels have no fixed duration/position — a
                            // progress bar makes no sense, so this shows how
                            // long the sitting lasted instead.
                            <span
                                className="text-zinc-500">Watched {formatDuration(r.ended_at_ms - r.started_at_ms)}</span>
                        ) : (
                            <div className="flex items-center gap-2">
                                <div className="w-20 h-1.5 rounded-full bg-zinc-800 overflow-hidden shrink-0">
                                    <div
                                        className={`h-full ${r.completed ? 'bg-emerald-500' : 'bg-violet-500'}`}
                                        style={{width: `${Math.round(pct * 100)}%`}}
                                    />
                                </div>
                                <span className="text-zinc-500">
                            {r.completed ? 'Watched' : `${Math.round(pct * 100)}% · ${formatDuration(r.duration_ms)}`}
                          </span>
                            </div>
                        )}
                    </td>
                    <td className="py-2 pr-4 text-zinc-600 whitespace-nowrap" title={new Date(r.started_at_ms).toLocaleString()}>
                      {relativeTime(r.started_at_ms)}
                    </td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
})
