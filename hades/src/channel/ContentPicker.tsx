import { observer } from 'mobx-react-lite'
import { runInAction } from 'mobx'
import type { PickerTab } from './types'
import { BLOCK_META } from './constants'
import { availablePickerTabs } from './utils'
import { inputStyle } from './styles'
import { FilterSection } from '../components/PickerFilters'
import type { ChannelDetailStore } from './store'

const TAB_LABELS: Record<PickerTab, string> = {
  shows: 'Shows', movies: 'Movies', episodes: 'Episodes',
  playlists: 'Playlists', filler_lists: 'Filler Lists',
}

// ─── Main picker ──────────────────────────────────────────────────────────────

export const ContentPicker = observer(function ContentPicker({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const tabs        = availablePickerTabs(store.draft.block_type)
  const showFilters = store.pickerTab === 'shows' || store.pickerTab === 'movies'
  const libType     = store.pickerTab === 'movies' ? 'movie' : 'show'
  const filteredLibs = store.allLibraries.filter(l => l.library_type === libType || l.library_type === 'mixed')

  return (
    <div className="hds-in" style={{ marginTop: 10, border: '1px solid var(--hds-line)', borderRadius: 10, background: 'oklch(0.16 0.016 286)', overflow: 'hidden' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 6, padding: '10px 10px 7px', borderBottom: '1px solid var(--hds-line-s)' }}>
        <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 7, padding: 3 }}>
          {tabs.map(t => (
            <button key={t} onClick={() => store.setPickerTab(t)} style={{ padding: '4px 10px', border: 'none', borderRadius: 5, background: store.pickerTab === t ? 'var(--hds-violet)' : 'transparent', color: store.pickerTab === t ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer' }}>
              {TAB_LABELS[t]}
            </button>
          ))}
        </div>
        <button onClick={() => store.closePicker()} style={{ color: 'var(--hds-txt-3)', background: 'transparent', border: 'none', cursor: 'pointer', fontSize: 12, padding: '0 3px', marginLeft: 'auto' }}>✕</button>
      </div>
      {store.pickerTab !== 'filler_lists' && store.pickerTab !== 'playlists' && (
        <div style={{ padding: '7px 10px', borderBottom: showFilters ? 'none' : '1px solid var(--hds-line-s)' }}>
          <input value={store.pickerQuery} onChange={e => store.setPickerQuery(e.target.value)} placeholder="Search…" style={{ width: '100%', ...inputStyle, fontSize: 11.5, padding: '6px 9px', boxSizing: 'border-box' }} autoFocus />
        </div>
      )}
      {showFilters && (
        <FilterSection
          rulesOpen={store.filterRulesOpen}
          filterMatch={store.filterMatch}
          filterRules={store.filterRules}
          filteredLibs={filteredLibs}
          onToggleOpen={() => runInAction(() => { store.filterRulesOpen = !store.filterRulesOpen })}
          onSetMatch={m => store.setFilterMatch(m)}
          onAddRule={() => store.addFilterRule()}
          onUpdateRule={(id, patch) => store.updateFilterRule(id, patch)}
          onRemoveRule={id => store.removeFilterRule(id)}
        />
      )}
      <div style={{ maxHeight: 200, overflow: 'auto' }} className="scrollbar-dark">
        {store.pickerLoading ? (
          <div style={{ padding: 12, color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
        ) : store.pickerTab === 'shows' ? (
          <ShowPickerList store={store} channelId={channelId} query={store.pickerQuery.toLowerCase()} />
        ) : store.pickerTab === 'movies' ? (
          <SimplePickerList
            items={store.pickerMovies.filter(m => !store.pickerQuery || m.title.toLowerCase().includes(store.pickerQuery.toLowerCase()))}
            getId={m => m.movie_id} dot={BLOCK_META.movie.edge}
            onAdd={(id, title) => store.addContent(channelId, { content_type: 'movie', content_id: id, title })}
          />
        ) : store.pickerTab === 'episodes' ? (
          <EpisodePickerList store={store} channelId={channelId} query={store.pickerQuery.toLowerCase()} />
        ) : store.pickerTab === 'playlists' ? (
          <SimplePickerList
            items={store.pickerPlaylists} getId={p => p.playlist_id} dot={BLOCK_META.episode.edge}
            onAdd={(id, title) => store.addContent(channelId, { content_type: 'playlist', content_id: id, title })}
          />
        ) : (
          <SimplePickerList
            items={store.pickerFillerLists} getId={f => f.filler_list_id} dot={BLOCK_META.filler.edge}
            onAdd={(id, title) => store.addContent(channelId, { content_type: 'filler_list', content_id: id, title })}
          />
        )}
      </div>
    </div>
  )
})

// ─── Generic list ─────────────────────────────────────────────────────────────

function SimplePickerList<T extends { title: string }>({ items, getId, dot, onAdd }: { items: T[]; getId: (i: T) => string; dot: string; onAdd: (id: string, title: string) => void }) {
  if (items.length === 0) return <p style={{ color: 'var(--hds-txt-3)', fontSize: 12, padding: 12 }}>No results.</p>
  return (
    <>
      {items.map(item => {
        const id = getId(item)
        return (
          <div key={id} draggable onClick={() => onAdd(id, item.title)} style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '9px 13px', cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)' }}
            onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)'}
            onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.background = ''}
          >
            <span style={{ color: 'var(--hds-txt-3)', fontSize: 12 }}>⋮⋮</span>
            <span style={{ width: 7, height: 7, borderRadius: 2, background: dot, flexShrink: 0 }} />
            <span style={{ flex: 1, fontSize: 12.5 }}>{item.title}</span>
            <span style={{ color: 'var(--hds-violet)', fontSize: 11 }}>+</span>
          </div>
        )
      })}
    </>
  )
}

// ─── Show list ────────────────────────────────────────────────────────────────

const ShowPickerList = observer(function ShowPickerList({ store, channelId, query }: { store: ChannelDetailStore; channelId: string; query: string }) {
  const shows = store.pickerShows.filter(s => !query || s.title.toLowerCase().includes(query))
  if (shows.length === 0) return <p style={{ color: 'var(--hds-txt-3)', fontSize: 12, padding: 12 }}>No results.</p>
  return (
    <>
      {shows.map(show => {
        const expanded = store.expandedShowId === show.show_id
        return (
          <div key={show.show_id}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '9px 13px', cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)' }}
              onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)'}
              onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.background = ''}
              onClick={() => store.expandShow(show.show_id)}
            >
              <span style={{ color: 'var(--hds-txt-3)', fontSize: 12 }}>⋮⋮</span>
              <span style={{ width: 7, height: 7, borderRadius: 2, background: BLOCK_META.episode.edge, flexShrink: 0 }} />
              <span style={{ flex: 1, fontSize: 12.5 }}>{show.title}</span>
              <span style={{ color: 'var(--hds-txt-3)', fontSize: 11 }}>{expanded ? '▲' : '▼'}</span>
            </div>
            {expanded && (
              <div style={{ display: 'flex', flexWrap: 'wrap', gap: 4, padding: '6px 14px 10px' }}>
                {store.expandedSeasonsLoading ? (
                  <span style={{ fontSize: 11, color: 'var(--hds-txt-3)' }}>Loading…</span>
                ) : (
                  <>
                    <button onClick={() => store.addContent(channelId, { content_type: 'show', content_id: show.show_id, season_filter: null, title: show.title })} style={{ padding: '2px 6px', borderRadius: 4, border: '1px solid var(--hds-line)', background: 'transparent', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer' }}>All seasons</button>
                    {store.expandedSeasons.map(s => (
                      <button key={s} onClick={() => store.addContent(channelId, { content_type: 'show', content_id: show.show_id, season_filter: s, title: `${show.title} S${String(s).padStart(2, '0')}` })} style={{ padding: '2px 6px', borderRadius: 4, border: '1px solid var(--hds-line)', background: 'transparent', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer' }}>S{String(s).padStart(2, '0')}</button>
                    ))}
                  </>
                )}
              </div>
            )}
          </div>
        )
      })}
    </>
  )
})

// ─── Episode list ─────────────────────────────────────────────────────────────

function EpisodePickerList({ store, channelId, query }: { store: ChannelDetailStore; channelId: string; query: string }) {
  const eps = store.pickerEpisodes.filter(e => !query || e.title.toLowerCase().includes(query) || e.show_title.toLowerCase().includes(query))
  if (eps.length === 0) return <p style={{ color: 'var(--hds-txt-3)', fontSize: 12, padding: 12 }}>No results. Type to search episodes.</p>
  return (
    <>
      {eps.map(ep => (
        <div key={ep.episode_id} style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '9px 13px', cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)' }}
          onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)'}
          onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.background = ''}
          onClick={() => store.addContent(channelId, { content_type: 'episode', content_id: ep.episode_id, title: `${ep.show_title} S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}` })}
        >
          <span style={{ width: 7, height: 7, borderRadius: 2, background: BLOCK_META.episode.edge, flexShrink: 0 }} />
          <div style={{ minWidth: 0 }}>
            <div style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>{ep.show_title}</div>
            <div style={{ fontSize: 12.5 }}>S{String(ep.season).padStart(2,'0')}E{String(ep.episode).padStart(2,'0')} — {ep.title}</div>
          </div>
        </div>
      ))}
    </>
  )
}
