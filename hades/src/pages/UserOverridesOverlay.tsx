import { useState, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import { api, mediaUrl, channelLogoUrl } from '../api/client'
import { userStore } from '../stores'
import { MediaTile, BrowserEmpty, LoadMoreSentinel } from '../channel/BrowserTiles'
import { useBrowserSearch } from '../channel/useBrowserSearch'
import { useDebounce } from '../hooks/useDebounce'
import type { Channel, User } from '../api/types'
import styles from './UserOverridesOverlay.module.css'

type OverrideTab = 'shows' | 'movies' | 'channels'

// Search-and-click content picker, same interaction shape as
// ChannelBumperOverlay — but overrides are whole-title allow/block with no
// season scoping, so there's no info-panel step: a tile click applies the
// currently selected mode immediately.
const UserOverridesOverlay = observer(function UserOverridesOverlay({ user, onClose }: {
  user:    User
  onClose: () => void
}) {
  const [tab,  setTab]  = useState<OverrideTab>('shows')
  const [q,    setQ]    = useState('')
  const [mode, setMode] = useState<'allow' | 'block'>('block')
  const [err,  setErr]  = useState('')

  const [channels, setChannels] = useState<Channel[]>([])
  const dq = useDebounce(q, 300)

  const {
    shows, showsTotal, showsLoadingMore, loadMoreShows,
    movies, moviesTotal, moviesLoadingMore, loadMoreMovies,
    loading,
  } = useBrowserSearch(tab === 'channels' ? 'shows' : tab, q, '', tab === 'channels')

  useEffect(() => { userStore.fetchOverrides(user.user_id).catch(() => {}) }, [user.user_id])

  useEffect(() => {
    if (tab !== 'channels' || channels.length) return
    api.getChannels().then(setChannels).catch(() => {})
  }, [tab, channels.length])

  const overrides   = userStore.overrides[user.user_id] ?? []
  const overrideFor = (entityType: string, entityId: string) =>
    overrides.find(o => o.entity_type === entityType && o.entity_id === entityId)

  const apply = async (entityType: 'show' | 'movie' | 'channel', entityId: string) => {
    setErr('')
    try {
      await userStore.addOverride(user.user_id, { entity_type: entityType, entity_id: entityId, mode })
    } catch (e: any) { setErr(e.message ?? 'Failed to save override') }
  }

  const remove = async (entityType: string, entityId: string) => {
    setErr('')
    try {
      await userStore.removeOverride(user.user_id, entityType, entityId)
    } catch (e: any) { setErr(e.message ?? 'Failed to remove override') }
  }

  const filteredChannels = channels.filter(c => c.name.toLowerCase().includes(dq.toLowerCase()))

  return (
    <div className={styles.backdrop}>
      <div className={styles.panel}>
        <div className={styles.panelTopAccent} />

        {/* Header */}
        <div className={styles.header}>
          <span className={styles.headerTitle}>Content Overrides</span>
          <span className={styles.headerSubtitle}>{user.username} — always wins over the rating ceiling</span>
          <div className={styles.spacer} />
          <button onClick={onClose} className={styles.closeBtn}>×</button>
        </div>

        {/* Body */}
        <div className={styles.body}>

          {/* Left: tile browser */}
          <div className={styles.browserCol}>
            <div className={styles.browserTopBar}>
              <div className={styles.browserControlsRow}>
                <div className={`${styles.pillGroup} ${styles.pillGroupFit}`}>
                  {(['shows', 'movies', 'channels'] as OverrideTab[]).map(t => (
                    <button key={t} onClick={() => { setTab(t); setQ('') }}
                      className={`${styles.tabBtn} ${tab === t ? styles.tabBtnActive : styles.tabBtnInactive}`}>
                      {t}
                    </button>
                  ))}
                </div>
                <div className={styles.spacer} />
                <div className={styles.pillGroup}>
                  <button onClick={() => setMode('allow')}
                    className={`${styles.modeBtn} ${mode === 'allow' ? styles.modeBtnAllowActive : styles.modeBtnInactive}`}>
                    Allow
                  </button>
                  <button onClick={() => setMode('block')}
                    className={`${styles.modeBtn} ${mode === 'block' ? styles.modeBtnBlockActive : styles.modeBtnInactive}`}>
                    Block
                  </button>
                </div>
              </div>
              <div className={styles.searchWrap}>
                <input value={q} onChange={e => setQ(e.target.value)} placeholder="Search…"
                  className={`${styles.input} ${styles.searchInput}`} />
              </div>
            </div>

            <div className={`${styles.browserScroll} scrollbar-dark`}>
              {tab === 'shows' ? (
                loading ? <div className={styles.browserLoading}>Loading…</div>
                : shows.length === 0 ? <BrowserEmpty /> : (
                  <>
                    <div className={styles.grid}>
                      {shows.map(s => (
                        <MediaTile key={s.show_id}
                          imgUrl={mediaUrl(`/api/shows/${s.show_id}/thumb`)}
                          title={s.title}
                          sub={s.year ? String(s.year) : undefined}
                          badge={!!overrideFor('show', s.show_id)}
                          onClick={() => apply('show', s.show_id)}
                        />
                      ))}
                    </div>
                    {shows.length < showsTotal && <LoadMoreSentinel loading={showsLoadingMore} onVisible={loadMoreShows} />}
                  </>
                )
              ) : tab === 'movies' ? (
                loading ? <div className={styles.browserLoading}>Loading…</div>
                : movies.length === 0 ? <BrowserEmpty /> : (
                  <>
                    <div className={styles.grid}>
                      {movies.map(m => (
                        <MediaTile key={m.movie_id}
                          imgUrl={mediaUrl(`/api/movies/${m.movie_id}/thumb`)}
                          title={m.title}
                          sub={m.year ? String(m.year) : undefined}
                          badge={!!overrideFor('movie', m.movie_id)}
                          onClick={() => apply('movie', m.movie_id)}
                        />
                      ))}
                    </div>
                    {movies.length < moviesTotal && <LoadMoreSentinel loading={moviesLoadingMore} onVisible={loadMoreMovies} />}
                  </>
                )
              ) : (
                filteredChannels.length === 0 ? <BrowserEmpty /> : (
                  <div className={styles.grid}>
                    {filteredChannels.map(c => (
                      <MediaTile key={c.channel_id}
                        imgUrl={c.logo_path ? channelLogoUrl(c.channel_id) : undefined}
                        placeholder="📺"
                        title={c.name}
                        sub={`Ch ${c.number}`}
                        badge={!!overrideFor('channel', c.channel_id)}
                        onClick={() => apply('channel', c.channel_id)}
                      />
                    ))}
                  </div>
                )
              )}
            </div>
          </div>

          {/* Right: current overrides */}
          <div className={`${styles.overridesCol} scrollbar-dark`}>
            <div className={styles.overridesHeader}>
              <span className={styles.overridesHeaderLabel}>OVERRIDES</span>
              <span className={styles.overridesCount}>{overrides.length}</span>
            </div>

            <div className={styles.overridesList}>
              {overrides.map(o => (
                <div key={`${o.entity_type}:${o.entity_id}`} className={styles.overrideRow}>
                  <span className={`${styles.modeBadge} ${o.mode === 'allow' ? styles.modeBadgeAllow : styles.modeBadgeBlock}`}>{o.mode}</span>
                  <span className={styles.entityTypeBadge}>{o.entity_type}</span>
                  <span className={styles.overrideTitle}>{o.title || o.entity_id}</span>
                  <button onClick={() => remove(o.entity_type, o.entity_id)} className={styles.overrideRemoveBtn}>×</button>
                </div>
              ))}
              {overrides.length === 0 && (
                <div className={styles.overridesEmpty}>
                  No overrides — the rating ceiling alone applies
                </div>
              )}
            </div>
          </div>
        </div>

        {/* Footer */}
        <div className={styles.footer}>
          <span className={styles.footerHint}>
            Click a tile to {mode === 'allow' ? 'allow' : 'block'} it for this account
          </span>
          {err && <span className={styles.footerError}>{err}</span>}
          <button onClick={onClose} className={`${styles.doneBtn} hds-btn-gold`}>Done</button>
        </div>
      </div>
    </div>
  )
})

export default UserOverridesOverlay
