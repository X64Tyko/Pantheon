import { useState, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import { api, mediaUrl } from '../api/client'
import type { BumperContentType, BumperMode, ChannelBumper } from '../api/types'
import type { ChannelDetailStore } from './store'
import { MediaTile, MediaInfoPanel, useDetailPanel, BrowserEmpty, LoadMoreSentinel } from './BrowserTiles'
import type { InfoItem, AddContentParams } from './BrowserTiles'
import { useBrowserSearch } from './useBrowserSearch'
import shared from './sharedStyles.module.css'
import styles from './ChannelBumperOverlay.module.css'

type BumperTab = 'shows' | 'playlists' | 'episodes'

const ChannelBumperOverlay = observer(function ChannelBumperOverlay({ channelId, store }: {
  channelId: string
  store:     ChannelDetailStore
}) {
  const [tab,       setTab]       = useState<BumperTab>('shows')
  const [q,         setQ]         = useState('')
  const [sFilter,   setSFilter]   = useState('')
  const [epsDurMax, setEpsDurMax] = useState('')

  const [bumpers,   setBumpers]   = useState<ChannelBumper[]>([])
  const [bumperErr, setBumperErr] = useState('')
  const [saving,    setSaving]    = useState(false)

  const [pendingMode, setPendingMode] = useState<BumperMode>('between')
  const [pendingN,    setPendingN]    = useState(3)

  const { infoItem, setInfoItem, infoDetail, infoSeasons, detailLoading } = useDetailPanel()

  const {
    shows, showsTotal, showsLoadingMore: showsLoadMore, loadMoreShows,
    eps, epsHasMore, epsLoadingMore: epsLoadMore, loadMoreEps,
    lists, loading,
  } = useBrowserSearch(tab, q, sFilter, !!infoItem)

  useEffect(() => {
    api.getBumpers(channelId).then(setBumpers).catch(() => {})
  }, [channelId])

  async function addBumper(item: AddContentParams) {
    setSaving(true); setBumperErr('')
    try {
      const b = await api.createBumper(channelId, { content_type: item.content_type as BumperContentType, content_id: item.content_id, mode: pendingMode, every_n: pendingN })
      setBumpers(prev => [...prev, b as ChannelBumper])
      setInfoItem(null)
    } catch (e: any) { setBumperErr(e.message) }
    finally { setSaving(false) }
  }

  async function deleteBumper(id: number) {
    try {
      await api.deleteBumper(channelId, id)
      setBumpers(prev => prev.filter(b => b.id !== id))
    } catch (e: any) { setBumperErr(e.message) }
  }

  const renderBumperAdd = (item: InfoItem, _seasons: {number: number; name: string}[], _onAdd: (p: AddContentParams) => void) => {
    const ct    = item.kind === 'show' ? 'show' : item.kind === 'movie' ? 'movie' : item.kind === 'episode' ? 'episode' : 'playlist'
    const cid   = item.kind === 'show' ? item.id : item.kind === 'movie' ? item.id : item.kind === 'episode' ? item.ep.episode_id : item.pl.playlist_id
    const title = item.kind === 'show' ? item.seed.title : item.kind === 'movie' ? item.seed.title : item.kind === 'episode' ? item.ep.title : item.pl.title

    return (
      <div className={styles.addFormWrap}>
        <div className={styles.addFormRow}>
          <select value={pendingMode} onChange={e => setPendingMode(e.target.value as BumperMode)} className={`${shared.filterInput} ${styles.modeSelect}`}>
            <option value="between">Between programs</option>
            <option value="filler">During filler</option>
          </select>
          <div className={styles.everyRow}>
            <span className={styles.everyText}>Every</span>
            <input type="number" min={1} value={pendingN} onChange={e => setPendingN(Math.max(1, +e.target.value || 1))}
              className={`${shared.filterInput} ${styles.everyInput}`} />
            <span className={styles.everyText}>progs</span>
          </div>
        </div>
        <button
          onClick={() => addBumper({ content_type: ct, content_id: cid, title })}
          disabled={saving}
          className={`${styles.addBumperBtn} ${saving ? styles.addBumperBtnSaving : ''}`}
        >
          {saving ? '…' : 'Add Bumper'}
        </button>
        {bumperErr && <div className={styles.addFormErr}>{bumperErr}</div>}
      </div>
    )
  }

  return (
    <div className={styles.overlayBackdrop}>
    <div className={styles.overlayPanel}>
      <div className={styles.overlayTopGlow} />

      {/* Header */}
      <div className={styles.header}>
        <span className={styles.headerTitle}>Channel Bumpers</span>
        <span className={styles.headerSubtitle}>injected between or during filler content</span>
        <div className={styles.headerSpacer} />
        <button onClick={() => { store.channelBumperOverlayOpen = false }} className={styles.closeBtn}>×</button>
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
              addLabel="ADD BUMPER"
              onAdd={() => {}}
              onBack={() => { setInfoItem(null); setBumperErr('') }}
              renderAdd={renderBumperAdd}
            />
          )}
          {/* Kept mounted (just hidden) rather than a ternary — a fresh
              mount on "back" would reset the grid's scroll to the top
              instead of leaving it where the inspected tile still is. */}
          <div className={`${styles.browseWrap} ${infoItem ? styles.browseWrapHidden : ''}`}>
            <>
              <div className={styles.tabsWrap}>
                <div className={styles.tabsRow}>
                  {(['shows', 'playlists', 'episodes'] as BumperTab[]).map(t => (
                    <button key={t} onClick={() => { setTab(t); setQ(''); setSFilter('') }}
                      className={`${styles.tabButton} ${tab === t ? styles.tabButtonActive : ''}`}>
                      {t}
                    </button>
                  ))}
                </div>
                <div className={styles.searchRow}>
                  <input value={q} onChange={e => setQ(e.target.value)} placeholder="Search…"
                    className={`${shared.input} ${styles.searchInput}`} />
                  {tab === 'episodes' && (
                    <input type="number" min={0} value={sFilter} onChange={e => setSFilter(e.target.value)} placeholder="S#"
                      className={`${shared.input} ${styles.seasonInput}`} />
                  )}
                  {tab === 'episodes' && (
                    <input type="number" min={1} value={epsDurMax} onChange={e => setEpsDurMax(e.target.value)} placeholder="≤m"
                      title="Max duration in minutes"
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
                          <MediaTile key={s.show_id}
                            imgUrl={mediaUrl(`/api/shows/${s.show_id}/thumb`)}
                            title={s.title}
                            sub={s.year ? String(s.year) : undefined}
                            badge={bumpers.some(b => b.content_type === 'show' && b.content_id === s.show_id)}
                            onClick={() => setInfoItem({ kind: 'show', id: s.show_id, seed: s })}
                          />
                        ))}
                      </div>
                      {shows.length < showsTotal && <LoadMoreSentinel loading={showsLoadMore} onVisible={loadMoreShows} />}
                    </>
                  )
                ) : tab === 'playlists' ? (
                  lists.length === 0 ? <BrowserEmpty hint="No playlists." /> : (
                    <div className={styles.tileGrid}>
                      {lists.map(p => (
                        <MediaTile key={p.playlist_id}
                          title={p.title} sub={`${p.item_count} items`} placeholder="☰"
                          badge={bumpers.some(b => b.content_type === 'playlist' && b.content_id === p.playlist_id)}
                          onClick={() => setInfoItem({ kind: 'playlist', pl: p })}
                        />
                      ))}
                    </div>
                  )
                ) : (() => {
                  const maxMs = epsDurMax.trim() !== '' ? parseInt(epsDurMax) * 60_000 : undefined
                  const filtered = eps.filter(ep => maxMs === undefined || ep.duration_ms === 0 || ep.duration_ms <= maxMs)
                  return filtered.length === 0 ? <BrowserEmpty hint={eps.length === 0 ? 'Type to search episodes.' : 'No episodes match the duration filter.'} /> : (
                    <>
                      <div className={styles.tileGrid}>
                        {filtered.map(ep => (
                          <MediaTile key={ep.episode_id}
                            imgUrl={mediaUrl(`/api/shows/${ep.show_id}/thumb`)}
                            title={`S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}`}
                            sub={ep.show_title}
                            badge={bumpers.some(b => b.content_type === 'episode' && b.content_id === ep.episode_id)}
                            onClick={() => setInfoItem({ kind: 'episode', ep })}
                          />
                        ))}
                      </div>
                      {epsHasMore && <LoadMoreSentinel loading={epsLoadMore} onVisible={loadMoreEps} />}
                    </>
                  )
                })()}
              </div>
            </>
          </div>
        </div>

        {/* Right: bumpers list */}
        <div className={`${styles.rightPane} scrollbar-dark`}>
          <div className={styles.rightPaneHeader}>
            <span className={styles.rightPaneTitle}>BUMPERS</span>
            <span className={styles.rightPaneCount}>{bumpers.length}</span>
          </div>

          <div className={styles.bumperList}>
            {bumpers.map(b => (
              <div key={b.id} className={styles.bumperRow}>
                <span className={styles.bumperTypeTag}>{b.content_type}</span>
                <span className={styles.bumperTitle}>{b.title || b.content_id}</span>
                <span className={styles.bumperModeText}>{b.mode} / {b.every_n}</span>
                <button onClick={() => deleteBumper(b.id)} className={styles.bumperDeleteBtn}>×</button>
              </div>
            ))}
            {bumpers.length === 0 && (
              <div className={styles.bumperEmpty}>
                No bumpers configured for this channel
              </div>
            )}
          </div>
        </div>
      </div>

      {/* Footer */}
      <div className={styles.footer}>
        <span className={styles.footerText}>
          {bumpers.length > 0
            ? `${bumpers.length} bumper${bumpers.length !== 1 ? 's' : ''} · click a tile to add more`
            : 'No channel bumpers configured'}
        </span>
        {bumperErr && !infoItem && (
          <span className={styles.footerErr}>{bumperErr}</span>
        )}
        <button onClick={() => { store.channelBumperOverlayOpen = false }}
          className={`${styles.doneBtn} hds-btn-gold`}>Done</button>
      </div>
    </div>
    </div>
  )
})

export default ChannelBumperOverlay
