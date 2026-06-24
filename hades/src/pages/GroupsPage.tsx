import { observer } from 'mobx-react-lite'
import { makeAutoObservable, runInAction } from 'mobx'
import { useEffect } from 'react'
import { api } from '../api/client'
import type { EpisodeGroup, GroupingCandidate, ShowGroupingResult } from '../api/types'

// ─── Store ───────────────────────────────────────────────────────────────────

class GroupsStore {
  shows:          ShowGroupingResult[] = []
  loading:        boolean = false
  selectedShowId: string | null = null

  // Per-show confirmed groups (loaded on demand)
  confirmedGroups:  EpisodeGroup[] = []
  groupsLoading:    boolean = false
  saving:           boolean = false

  constructor() { makeAutoObservable(this) }

  get selectedShow(): ShowGroupingResult | null {
    return this.shows.find(s => s.show_id === this.selectedShowId) ?? null
  }

  get pendingCount(): number {
    return this.shows.reduce((n, s) => n + s.candidates.filter(c => !c.already_grouped).length, 0)
  }

  async load() {
    this.loading = true
    try {
      const results = await api.getAllGroupingCandidates()
      runInAction(() => {
        this.shows   = results
        this.loading = false
        if (this.selectedShowId && !results.find(s => s.show_id === this.selectedShowId))
          this.selectedShowId = null
        if (!this.selectedShowId && results.length > 0)
          this.selectShow(results[0].show_id)
      })
    } catch {
      runInAction(() => { this.loading = false })
    }
  }

  selectShow(id: string) {
    this.selectedShowId  = id
    this.confirmedGroups = []
    this.groupsLoading   = true
    api.getEpisodeGroups(id)
      .then(gs => runInAction(() => { this.confirmedGroups = gs; this.groupsLoading = false }))
      .catch(()  => runInAction(() => { this.groupsLoading = false }))
  }

  async confirmGroup(candidate: GroupingCandidate) {
    const showId = this.selectedShowId
    if (!showId) return
    this.saving = true
    try {
      const { group_id } = await api.createEpisodeGroup(showId, { name: candidate.base_title, group_type: 'multipart' })
      await Promise.all(candidate.parts.map(p => api.addGroupMember(showId, group_id, { episode_id: p.episode_id, part_num: p.part_num })))
    } catch {}
    runInAction(() => { this.saving = false })
    // Reload both the cross-show list and confirmed groups for this show.
    this.load()
    api.getEpisodeGroups(showId)
      .then(gs => runInAction(() => { this.confirmedGroups = gs }))
      .catch(() => {})
  }

  async deleteGroup(groupId: string) {
    const showId = this.selectedShowId
    if (!showId) return
    this.saving = true
    try {
      await api.deleteEpisodeGroup(showId, groupId)
    } catch {}
    runInAction(() => { this.saving = false })
    this.load()
    api.getEpisodeGroups(showId)
      .then(gs => runInAction(() => { this.confirmedGroups = gs }))
      .catch(() => {})
  }

  async confirmAllHigh() {
    const showId = this.selectedShowId
    const show   = this.selectedShow
    if (!showId || !show) return
    const high = show.candidates.filter(c => !c.already_grouped && c.confidence >= 80)
    this.saving = true
    try {
      for (const c of high) {
        const { group_id } = await api.createEpisodeGroup(showId, { name: c.base_title, group_type: 'multipart' })
        await Promise.all(c.parts.map(p => api.addGroupMember(showId, group_id, { episode_id: p.episode_id, part_num: p.part_num })))
      }
    } catch {}
    runInAction(() => { this.saving = false })
    this.load()
    api.getEpisodeGroups(showId)
      .then(gs => runInAction(() => { this.confirmedGroups = gs }))
      .catch(() => {})
  }
}

const store = new GroupsStore()

// ─── Page ────────────────────────────────────────────────────────────────────

export default observer(function GroupsPage() {
  useEffect(() => { store.load() }, [])

  return (
    <div className="flex flex-col h-[calc(100vh-3.5rem)] gap-0">
      <div className="flex items-center justify-between pb-4 shrink-0">
        <h1 className="text-xl font-semibold text-zinc-100">Episode Groups</h1>
        {store.pendingCount > 0 && (
          <span className="text-xs text-zinc-500">
            {store.pendingCount} pending across {store.shows.length} {store.shows.length === 1 ? 'show' : 'shows'}
          </span>
        )}
      </div>

      {store.loading && store.shows.length === 0 ? (
        <div className="flex items-center gap-2 text-zinc-600 text-sm py-6">
          <span className="w-1.5 h-1.5 rounded-full bg-violet-500 animate-pulse" />
          Scanning all shows for multi-part patterns…
        </div>
      ) : store.shows.length === 0 ? (
        <div className="py-10 text-center">
          <p className="text-zinc-500 text-sm">No pending grouping candidates found.</p>
          <p className="text-zinc-700 text-xs mt-1">All multi-part episodes are already grouped, or none were detected.</p>
        </div>
      ) : (
        <div className="flex gap-4 flex-1 min-h-0">
          {/* Show list sidebar */}
          <aside className="w-52 shrink-0 flex flex-col gap-0.5 overflow-y-auto scrollbar-dark pr-1">
            {store.shows.map(s => {
              const pending = s.candidates.filter(c => !c.already_grouped).length
              const isActive = s.show_id === store.selectedShowId
              return (
                <button
                  key={s.show_id}
                  onClick={() => store.selectShow(s.show_id)}
                  className={`w-full flex items-center justify-between gap-2 px-2.5 py-1.5 rounded text-sm transition-all text-left ${
                    isActive
                      ? 'bg-amber-500/10 text-amber-400 ring-1 ring-amber-500/20'
                      : 'text-zinc-400 hover:text-zinc-200 hover:bg-violet-950/30'
                  }`}
                >
                  <span className="truncate">{s.show_title}</span>
                  <span className={`shrink-0 text-[10px] font-semibold px-1.5 py-0.5 rounded ${
                    isActive ? 'bg-amber-500/20 text-amber-300' : 'bg-zinc-800 text-zinc-500'
                  }`}>{pending}</span>
                </button>
              )
            })}
          </aside>

          {/* Detail panel */}
          <div className="flex-1 overflow-y-auto scrollbar-dark min-h-0 pr-1">
            {store.selectedShow ? (
              <ShowGroupPanel
                show={store.selectedShow}
                confirmedGroups={store.confirmedGroups}
                groupsLoading={store.groupsLoading}
                saving={store.saving}
                onConfirm={c => store.confirmGroup(c)}
                onDelete={gid => store.deleteGroup(gid)}
                onConfirmAllHigh={() => store.confirmAllHigh()}
              />
            ) : null}
          </div>
        </div>
      )}
    </div>
  )
})

// ─── Show detail panel ────────────────────────────────────────────────────────

const ShowGroupPanel = observer(function ShowGroupPanel({
  show, confirmedGroups, groupsLoading, saving,
  onConfirm, onDelete, onConfirmAllHigh,
}: {
  show:            ShowGroupingResult
  confirmedGroups: EpisodeGroup[]
  groupsLoading:   boolean
  saving:          boolean
  onConfirm:       (c: GroupingCandidate) => void
  onDelete:        (groupId: string) => void
  onConfirmAllHigh: () => void
}) {
  const unconfirmed = show.candidates.filter(c => !c.already_grouped)
  const highConf    = unconfirmed.filter(c => c.confidence >= 80)

  return (
    <div className="space-y-5">
      {/* Show header */}
      <div className="flex items-center justify-between">
        <h2 className="text-base font-semibold text-zinc-100">{show.show_title}</h2>
        {highConf.length > 1 && (
          <button
            disabled={saving}
            onClick={onConfirmAllHigh}
            className="text-xs px-3 py-1 rounded border border-violet-700/50 text-violet-400
                       hover:border-violet-500 hover:text-violet-300 transition-colors disabled:opacity-40"
          >
            Confirm all high-confidence ({highConf.length})
          </button>
        )}
      </div>

      {/* Confirmed groups */}
      <section>
        <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60 mb-2">
          Confirmed Groups
        </div>
        {groupsLoading ? (
          <p className="text-xs text-zinc-600">Loading…</p>
        ) : confirmedGroups.length === 0 ? (
          <p className="text-xs text-zinc-600">No confirmed groups yet.</p>
        ) : (
          <div className="space-y-2">
            {confirmedGroups.map(g => (
              <div key={g.group_id}
                className="rounded-lg border border-emerald-900/40 bg-emerald-950/10 p-3 space-y-1.5"
              >
                <div className="flex items-center gap-2">
                  <span className="text-xs font-medium text-zinc-200 flex-1 truncate">{g.name}</span>
                  <span className="text-[10px] text-emerald-400">{g.members.length} parts</span>
                  <button
                    disabled={saving}
                    onClick={() => onDelete(g.group_id)}
                    className="text-[10px] text-zinc-600 hover:text-red-400 transition-colors disabled:opacity-40"
                  >Delete</button>
                </div>
                <div className="space-y-0.5">
                  {g.members.map(m => (
                    <div key={m.id} className="flex items-center gap-2 text-[11px] text-zinc-400">
                      <span className="w-12 shrink-0 text-zinc-600">Part {m.part_num}</span>
                      <span className="text-zinc-500 w-14 shrink-0">S{String(m.season).padStart(2,'0')}E{String(m.episode).padStart(2,'0')}</span>
                      <span className="truncate">{m.title}</span>
                    </div>
                  ))}
                </div>
              </div>
            ))}
          </div>
        )}
      </section>

      {/* Candidates */}
      <section>
        <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60 mb-2">
          Detected Candidates
        </div>
        {unconfirmed.length === 0 ? (
          <p className="text-xs text-zinc-600">All detected candidates are already confirmed.</p>
        ) : (
          <div className="space-y-2">
            {unconfirmed.sort((a, b) => b.confidence - a.confidence).map((c, i) => (
              <div key={i}
                className="rounded-lg border p-3 space-y-1.5"
                style={{ borderColor: c.confidence >= 80 ? 'oklch(0.6 0.18 260 / 0.4)' : 'oklch(0.4 0.05 260 / 0.25)' }}
              >
                <div className="flex items-center gap-2">
                  <span className="text-xs font-medium text-zinc-200 flex-1 truncate">{c.base_title}</span>
                  <span
                    className="text-[10px] font-semibold px-1.5 py-0.5 rounded"
                    style={{
                      background: c.confidence >= 80 ? 'oklch(0.55 0.18 260 / 0.25)' : 'oklch(0.4 0.08 260 / 0.15)',
                      color:      c.confidence >= 80 ? 'oklch(0.75 0.18 260)'         : 'oklch(0.6 0.08 260)',
                    }}
                  >{c.confidence}%</span>
                  {!c.adjacent && <span className="text-[10px] text-amber-400">non-adjacent</span>}
                  <button
                    disabled={saving}
                    onClick={() => onConfirm(c)}
                    className="text-[10px] px-2 py-0.5 rounded border border-violet-700/50 text-violet-400
                               hover:border-violet-500 hover:text-violet-300 transition-colors disabled:opacity-40"
                  >Confirm</button>
                </div>
                <div className="space-y-0.5">
                  {c.parts.map(p => (
                    <div key={p.episode_id} className="flex items-center gap-2 text-[11px] text-zinc-400">
                      <span className="w-12 shrink-0 text-zinc-600">Part {p.part_num}</span>
                      <span className="text-zinc-500 w-14 shrink-0">S{String(p.season).padStart(2,'0')}E{String(p.episode).padStart(2,'0')}</span>
                      <span className="truncate">{p.title}</span>
                    </div>
                  ))}
                </div>
              </div>
            ))}
          </div>
        )}
      </section>
    </div>
  )
})
