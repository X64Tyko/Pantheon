import { observer } from 'mobx-react-lite'
import { runInAction } from 'mobx'
import { useState, useEffect } from 'react'
import { useDebounce } from '../hooks/useDebounce'
import { mediaUrl } from '../api/client'
import type { PickerTab } from './types'
import { availablePickerTabs } from './utils'
import { inputStyle } from './styles'
import { FilterSection } from '../components/PickerFilters'
import { HelpTip, HelpSection, GifSlot } from './HelpTip'
import { MediaTile, ShowMediaTile, MediaInfoPanel, useDetailPanel, LoadMoreSentinel, BrowserEmpty } from './BrowserTiles'
import type { AddContentParams, InfoItem } from './BrowserTiles'
import type { ChannelDetailStore } from './store'

const TAB_LABELS: Record<PickerTab, string> = {
  shows: 'Shows', movies: 'Movies', episodes: 'Episodes', playlists: 'Playlists',
}

// ─── Main component ───────────────────────────────────────────────────────────

export const LibraryBrowser = observer(function LibraryBrowser({ channelId, store, onAdd }: {
  channelId: string
  store:     ChannelDetailStore
  onAdd?:    (params: AddContentParams) => void
}) {
  const { infoItem, setInfoItem, infoDetail, infoSeasons, detailLoading } = useDetailPanel()
  const handleAdd = onAdd ?? ((params: AddContentParams) => store.addContent(channelId, params))

  const [rawQ, setRawQ] = useState(store.pickerQuery)
  const debouncedQ = useDebounce(rawQ, 300)
  useEffect(() => { store.setPickerQuery(debouncedQ) }, [debouncedQ])

  const tabs        = availablePickerTabs(store.draft.block_type)
  const showFilters = !infoItem && (store.pickerTab === 'shows' || store.pickerTab === 'movies')
  const libType     = store.pickerTab === 'movies' ? 'movie' : 'show'
  const filteredLibs = store.allLibraries.filter(l => l.library_type === libType || l.library_type === 'mixed')

  if (tabs.length === 0) {
    return (
      <div style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center', color: 'var(--hds-txt-3)', fontSize: 12, padding: 20, textAlign: 'center' }}>
        Use the Filler section to add filler lists to this block.
      </div>
    )
  }

  return (
    <div style={{ flex: 1, display: 'flex', flexDirection: 'column', minHeight: 0, background: 'oklch(0.13 0.015 286)' }}>
      {!infoItem && (
        <div style={{ flexShrink: 0, borderBottom: '1px solid var(--hds-line-s)' }}>
          <div style={{ padding: '10px 14px 8px' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 8 }}>
              <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 7, padding: 3 }}>
                {tabs.map(t => (
                  <button key={t} onClick={() => store.setPickerTab(t)}
                    style={{ padding: '4px 12px', border: 'none', borderRadius: 5, background: store.pickerTab === t ? 'var(--hds-violet)' : 'transparent', color: store.pickerTab === t ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer' }}>
                    {TAB_LABELS[t]}
                  </button>
                ))}
              </div>
              <HelpTip title="Using the Library Browser" tip="Drag, click, and filter your library" down>
                <HelpSection title="Drag to Add">
                  Grab any tile and drop it onto the CONTENT list in the left panel. A dashed violet drop zone appears when you hover over a valid target. Shows, movies, individual episodes, and playlists can all be dragged in.
                  <GifSlot label="Dragging a show tile onto the Content list" />
                </HelpSection>
                <HelpSection title="Click to Inspect">
                  Clicking a tile opens the info panel with the full synopsis, season list, runtime, and content rating. From there you can add the entire show, individual seasons, or just Season 00 (specials only).
                  <GifSlot label="Clicking a tile to open the info panel and add a specific season" />
                </HelpSection>
                <HelpSection title="Filters">
                  The Filters toggle (below the search bar) opens a rule builder. Add rules to filter by genre, year, content rating, network, library, and more. Rules combine with <b style={{ color: 'var(--hds-txt)' }}>ALL</b> (every rule must match) or <b style={{ color: 'var(--hds-txt)' }}>ANY</b> (at least one must match). Active rules are shown in violet on the Filters toggle.
                  <GifSlot label="Building filter rules to narrow the library by genre and year" />
                </HelpSection>
              </HelpTip>
            </div>
            {store.pickerTab !== 'playlists' && (
              <div style={{ display: 'flex', gap: 6 }}>
                <input value={rawQ} onChange={e => setRawQ(e.target.value)} placeholder="Search…" style={{ ...inputStyle, flex: 1, fontSize: 11.5, padding: '6px 9px' }} />
                {store.pickerTab === 'episodes' && (
                  <input type="number" min={0} value={store.pickerSeasonFilter} onChange={e => store.setPickerSeasonFilter(e.target.value)} placeholder="S#" title="Filter by season" style={{ ...inputStyle, width: 48, fontSize: 11, padding: '5px 6px' }} />
                )}
              </div>
            )}
          </div>
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
        </div>
      )}

      {infoItem ? (
        <MediaInfoPanel
          item={infoItem}
          detail={infoDetail}
          seasons={infoSeasons}
          detailLoading={detailLoading}
          onAdd={handleAdd}
          onBack={() => setInfoItem(null)}
        />
      ) : (
        <TileGrid store={store} channelId={channelId} onSelect={setInfoItem} onAdd={handleAdd} />
      )}
    </div>
  )
})

// ─── Tile grid ────────────────────────────────────────────────────────────────

const TileGrid = observer(function TileGrid({ store, channelId, onSelect, onAdd }: { store: ChannelDetailStore; channelId: string; onSelect: (item: InfoItem) => void; onAdd: (params: AddContentParams) => void }) {
  if (store.pickerLoading) {
    return <div style={{ padding: '20px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
  }

  const startDrag = (e: React.DragEvent, content_type: 'show' | 'movie' | 'episode' | 'playlist', content_id: string, title: string) => {
    e.dataTransfer.effectAllowed = 'copy'
    runInAction(() => { store.dragContent = { content_type, content_id, title } })
  }
  const endDrag = () => runInAction(() => { store.dragContent = null })

  const gridStyle: React.CSSProperties = {
    display: 'grid',
    gridTemplateColumns: 'repeat(auto-fill, minmax(128px, 1fr))',
    gap: 10,
    padding: 14,
    alignContent: 'start',
  }

  if (store.pickerTab === 'shows') {
    const items = store.pickerShows
    if (items.length === 0) return <BrowserEmpty />
    return (
      <div style={{ overflow: 'auto', flex: 1 }} className="scrollbar-dark">
        <div style={gridStyle}>
          {items.map(s => (
            <ShowMediaTile key={s.show_id}
              show={s}
              isAdded={store.draftContent.some(c => c.content_type === 'show' && c.content_id === s.show_id)}
              onAdd={onAdd}
              onInfoOpen={() => onSelect({ kind: 'show', id: s.show_id, seed: s })}
              onDragStart={e => startDrag(e, 'show', s.show_id, s.title)}
              onDragEnd={endDrag}
            />
          ))}
        </div>
        {items.length < store.pickerTotal && (
          <LoadMoreSentinel loading={store.pickerLoadingMore} onVisible={() => store.loadMorePicker()} />
        )}
      </div>
    )
  }

  if (store.pickerTab === 'movies') {
    const items = store.pickerMovies
    if (items.length === 0) return <BrowserEmpty />
    return (
      <div style={{ overflow: 'auto', flex: 1 }} className="scrollbar-dark">
        <div style={gridStyle}>
          {items.map(m => (
            <MediaTile key={m.movie_id}
              imgUrl={mediaUrl(`/api/movies/${m.movie_id}/thumb`)}
              title={m.title}
              sub={m.year ? String(m.year) : undefined}
              badge={store.draftContent.some(c => c.content_type === 'movie' && c.content_id === m.movie_id)}
              onDragStart={e => startDrag(e, 'movie', m.movie_id, m.title)}
              onDragEnd={endDrag}
              onClick={() => onSelect({ kind: 'movie', id: m.movie_id, seed: m })}
            />
          ))}
        </div>
        {items.length < store.pickerTotal && (
          <LoadMoreSentinel loading={store.pickerLoadingMore} onVisible={() => store.loadMorePicker()} />
        )}
      </div>
    )
  }

  if (store.pickerTab === 'episodes') {
    const items = store.pickerEpisodes
    if (items.length === 0) return <BrowserEmpty hint="Type to search episodes." />
    return (
      <div style={{ ...gridStyle, overflow: 'auto', flex: 1 }} className="scrollbar-dark">
        {items.map(ep => {
          const code  = `S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')}`
          const title = `${ep.show_title} ${code} — ${ep.title}`
          return (
            <MediaTile key={ep.episode_id}
              imgUrl={mediaUrl(`/api/shows/${ep.show_id}/thumb`)}
              title={`${code} — ${ep.title}`}
              sub={ep.show_title}
              onDragStart={e => startDrag(e, 'episode', ep.episode_id, title)}
              onDragEnd={endDrag}
              onClick={() => onSelect({ kind: 'episode', ep })}
            />
          )
        })}
      </div>
    )
  }

  if (store.pickerTab === 'playlists') {
    const items = store.pickerPlaylists
    if (items.length === 0) return <BrowserEmpty />
    return (
      <div style={{ ...gridStyle, overflow: 'auto', flex: 1 }} className="scrollbar-dark">
        {items.map(p => (
          <MediaTile key={p.playlist_id}
            title={p.title}
            sub={`${p.item_count} items`}
            placeholder="☰"
            badge={store.draftContent.some(c => c.content_type === 'playlist' && c.content_id === p.playlist_id)}
            onDragStart={e => startDrag(e, 'playlist', p.playlist_id, p.title)}
            onDragEnd={endDrag}
            onClick={() => onSelect({ kind: 'playlist', pl: p })}
          />
        ))}
      </div>
    )
  }

  return null
})
