import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import { mediaUrl } from '../api/client'
import type { Channel, FillerEntryAdvancement, FillerSelectionMode } from '../api/types'
import { FILLER_ADV_OPTS, FILLER_SEL_OPTS } from './constants'
import type { ChannelDetailStore } from './store'
import { ShowMediaTile, MediaTile, MediaInfoPanel, useDetailPanel, BrowserEmpty, LoadMoreSentinel } from './BrowserTiles'
import { useBrowserSearch } from './useBrowserSearch'
import shared from './sharedStyles.module.css'
import styles from './ChannelFillerOverlay.module.css'

type FillerTab = 'shows' | 'movies' | 'episodes' | 'playlists'

const ChannelFillerOverlay = observer(function ChannelFillerOverlay({ channelId, channel, store }: {
  channelId: string
  channel:   Channel
  store:     ChannelDetailStore
}) {
  const [tab,     setTab]     = useState<FillerTab>('shows')
  const [query,   setQuery]   = useState('')
  const [maxDur,  setMaxDur]  = useState('')
  const [sFilter, setSFilter] = useState('')

  const { infoItem, setInfoItem, infoDetail, infoSeasons, detailLoading } = useDetailPanel()

  const {
    shows, showsTotal, showsLoadingMore, loadMoreShows,
    movies, moviesTotal, moviesLoadingMore, loadMoreMovies,
    eps, epsHasMore, epsLoadingMore, loadMoreEps,
    lists: playlists, loading,
  } = useBrowserSearch(tab, query, sFilter)

  const entries  = channel.default_filler_entries
  const selMode  = channel.default_filler_selection
  const showWeight = selMode === 'weighted'

  const isAdded = (ct: string, cid: string, sf?: number) =>
    entries.some(e => e.content_type === ct && e.content_id === cid &&
      (sf === undefined ? true : (e.season_filter ?? undefined) === sf))

  const addFiller = (params: { content_type: string; content_id: string; title: string; season_filter?: number | null; include_specials?: boolean }) => {
    const sf = params.season_filter ?? undefined
    if (isAdded(params.content_type, params.content_id, sf as number | undefined)) return
    store.addChannelFiller(channelId, {
      content_type: params.content_type,
      content_id:   params.content_id,
      title:        params.title,
      advancement:  'sized',
      weight:       1,
      season_filter: sf,
    })
  }

  return (
    <div className={styles.overlayBackdrop}>
    <div className={styles.overlayPanel}>
      <div className={styles.overlayTopGlow} />

      {/* Header */}
      <div className={styles.header}>
        <span className={styles.headerTitle}>Channel Filler</span>
        <span className={styles.headerSubtitle}>default filler for blocks without their own filler list</span>
        <div className={styles.headerSpacer} />
        <button
          onClick={() => { store.channelFillerOverlayOpen = false }}
          className={styles.closeBtn}
        >×</button>
      </div>

      {/* Body */}
      <div className={styles.body}>

        {/* Left: tile browser */}
        <div className={styles.leftPane}>
          {infoItem && (
            <MediaInfoPanel
              item={infoItem}
              detail={infoDetail}
              seasons={infoSeasons}
              detailLoading={detailLoading}
              addLabel="ADD TO CHANNEL FILLER"
              onAdd={addFiller}
              onBack={() => setInfoItem(null)}
            />
          )}
          {/* Kept mounted (just hidden) rather than a ternary — a fresh
              mount on "back" would reset the grid's scroll to the top
              instead of leaving it where the inspected tile still is. */}
          <div className={`${styles.browseWrap} ${infoItem ? styles.browseWrapHidden : ''}`}>
            <>
              <div className={styles.tabsWrap}>
                <div className={styles.tabsRow}>
                  {(['shows', 'movies', 'episodes', 'playlists'] as FillerTab[]).map(t => (
                    <button key={t} onClick={() => { setTab(t); setQuery(''); setSFilter(''); setInfoItem(null) }}
                      className={`${styles.tabButton} ${tab === t ? styles.tabButtonActive : ''}`}>
                      {t}
                    </button>
                  ))}
                </div>
                <div className={styles.searchRow}>
                  {tab !== 'playlists' && (
                    <input value={query} onChange={e => setQuery(e.target.value)} placeholder="Search…"
                      className={`${shared.input} ${styles.searchInput}`} />
                  )}
                  {tab === 'playlists' && <div className={styles.searchSpacer} />}
                  {tab === 'episodes' && (
                    <input type="number" min={0} value={sFilter} onChange={e => setSFilter(e.target.value)}
                      placeholder="S#" title="Filter by season"
                      className={`${shared.input} ${styles.seasonInput}`} />
                  )}
                  {(tab === 'movies' || tab === 'episodes' || tab === 'playlists') && (
                    <input type="number" min={1} value={maxDur} onChange={e => setMaxDur(e.target.value)}
                      placeholder="≤m" title="Max duration in minutes"
                      className={`${shared.input} ${styles.durInput}`} />
                  )}
                </div>
              </div>

              <div className={`${styles.gridScroll} scrollbar-dark`}>
                {loading ? (
                  <div className={styles.loadingText}>Loading…</div>
                ) : tab === 'shows' ? (
                  shows.length === 0 ? <BrowserEmpty /> : (
                    <>
                      <div className={styles.tileGrid}>
                        {shows.map(s => (
                          <ShowMediaTile key={s.show_id}
                            show={s}
                            isAdded={isAdded('show', s.show_id)}
                            onAdd={addFiller}
                            onInfoOpen={() => setInfoItem({ kind: 'show', id: s.show_id, seed: s })}
                          />
                        ))}
                      </div>
                      {shows.length < showsTotal && <LoadMoreSentinel loading={showsLoadingMore} onVisible={loadMoreShows} />}
                    </>
                  )
                ) : tab === 'movies' ? (() => {
                  const maxMs = maxDur.trim() !== '' ? parseInt(maxDur) * 60_000 : undefined
                  const filtered = maxMs !== undefined ? movies.filter(m => m.duration_ms === 0 || m.duration_ms <= maxMs) : movies
                  return filtered.length === 0 ? <BrowserEmpty hint={movies.length === 0 ? undefined : 'No movies match the duration filter.'} /> : (
                    <>
                      <div className={styles.tileGrid}>
                        {filtered.map(m => (
                          <MediaTile key={m.movie_id}
                            imgUrl={mediaUrl(`/api/movies/${m.movie_id}/thumb`)}
                            title={m.title}
                            sub={m.year ? String(m.year) : undefined}
                            badge={isAdded('movie', m.movie_id)}
                            onClick={() => setInfoItem({ kind: 'movie', id: m.movie_id, seed: m })}
                          />
                        ))}
                      </div>
                      {movies.length < moviesTotal && <LoadMoreSentinel loading={moviesLoadingMore} onVisible={loadMoreMovies} />}
                    </>
                  )
                })() : tab === 'episodes' ? (() => {
                  const maxMs = maxDur.trim() !== '' ? parseInt(maxDur) * 60_000 : undefined
                  const filtered = eps.filter(ep => maxMs === undefined || ep.duration_ms === 0 || ep.duration_ms <= maxMs)
                  return filtered.length === 0 ? <BrowserEmpty hint={eps.length === 0 ? 'Type to search episodes.' : 'No episodes match the filter.'} /> : (
                    <>
                      <div className={styles.tileGrid}>
                        {filtered.map(ep => (
                          <MediaTile key={ep.episode_id}
                            imgUrl={mediaUrl(`/api/shows/${ep.show_id}/thumb`)}
                            title={`S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}`}
                            sub={ep.show_title}
                            badge={isAdded('episode', ep.episode_id)}
                            onClick={() => addFiller({
                              content_type: 'episode',
                              content_id: ep.episode_id,
                              title: `${ep.show_title} S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}`,
                            })}
                          />
                        ))}
                      </div>
                      {epsHasMore && <LoadMoreSentinel loading={epsLoadingMore} onVisible={loadMoreEps} />}
                    </>
                  )
                })() : (() => {
                  const maxMs = maxDur.trim() !== '' ? parseInt(maxDur) * 60_000 : undefined
                  const filtered = maxMs !== undefined ? playlists.filter(p => p.total_ms === 0 || p.total_ms <= maxMs) : playlists
                  return filtered.length === 0 ? <BrowserEmpty hint={playlists.length === 0 ? 'No playlists found.' : 'No playlists match the duration filter.'} /> : (
                    <div className={styles.tileGrid}>
                      {filtered.map(p => (
                        <MediaTile key={p.playlist_id}
                          title={p.title}
                          sub={`${p.item_count} items`}
                          placeholder="☰"
                          badge={isAdded('playlist', p.playlist_id)}
                          onClick={() => addFiller({ content_type: 'playlist', content_id: p.playlist_id, title: p.title })}
                        />
                      ))}
                    </div>
                  )
                })()}
              </div>
            </>
          </div>
        </div>

        {/* Right: filler entries */}
        <div className={styles.rightPane}>
          <div className={styles.rightPaneHeader}>
            <span className={styles.rightPaneTitle}>CHANNEL FILLER</span>
            <span className={styles.rightPaneCount}>{entries.length}</span>
          </div>

          {entries.length > 1 && (
            <div className={styles.selectByWrap}>
              <div className={styles.selectByLabel}>SELECT BY</div>
              <select value={selMode} onChange={e => store.saveChannelFiller(channelId, { default_filler_selection: e.target.value as FillerSelectionMode })} className={shared.input}>
                {FILLER_SEL_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
              </select>
            </div>
          )}

          <div className={styles.entryList}>
            {entries.map(entry => (
              <div key={entry.id} className={styles.entryRow}>
                <span className={styles.fillerDot} />
                <div className={styles.entryMain}>
                  <div className={styles.entryTitle}>{entry.title || entry.content_id}</div>
                  {entry.season_filter !== undefined && (
                    <div className={styles.entrySeasonTag}>
                      S{String(entry.season_filter).padStart(2, '0')} only
                    </div>
                  )}
                </div>
                <span className={styles.entryTypeTag}>{entry.content_type}</span>
                <select
                  value={entry.advancement}
                  onChange={e => store.updateChannelFiller(channelId, entry.id, { advancement: e.target.value as FillerEntryAdvancement })}
                  className={`${shared.filterInput} ${styles.advancementSelect}`}
                >
                  {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                </select>
                {showWeight && (
                  <input type="number" min={1} value={entry.weight}
                    onChange={e => store.updateChannelFiller(channelId, entry.id, { weight: Math.max(1, +e.target.value || 1) })}
                    className={`${shared.filterInput} ${styles.weightInput}`} title="Weight" />
                )}
                <button
                  onClick={() => store.removeChannelFiller(channelId, entry.id)}
                  className={styles.entryDeleteBtn}
                >×</button>
              </div>
            ))}
            {entries.length === 0 && (
              <div className={styles.entryEmpty}>
                No filler configured for this channel
              </div>
            )}
          </div>
        </div>
      </div>

      {/* Footer */}
      <div className={styles.footer}>
        <span className={styles.footerText}>
          {entries.length > 0 ? `${entries.length} source${entries.length !== 1 ? 's' : ''} · ${selMode}` : 'No default channel filler configured'}
        </span>
        {store.channelFillerErr && (
          <span className={styles.footerErr}>{store.channelFillerErr}</span>
        )}
        <button
          onClick={() => { store.channelFillerOverlayOpen = false }}
          className={`${styles.doneBtn} hds-btn-gold`}
        >Done</button>
      </div>
    </div>
    </div>
  )
})

export default ChannelFillerOverlay
