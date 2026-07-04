import { useState, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import { api, mediaUrl, channelLogoUrl } from '../api/client'
import { userStore } from '../stores'
import { MediaTile, BrowserEmpty, LoadMoreSentinel } from '../channel/BrowserTiles'
import { useBrowserSearch } from '../channel/useBrowserSearch'
import { useDebounce } from '../hooks/useDebounce'
import type { Channel, User } from '../api/types'

type OverrideTab = 'shows' | 'movies' | 'channels'

const inputStyle: React.CSSProperties = {
  padding: '7px 10px', background: 'var(--hds-bg-3)',
  border: '1px solid var(--hds-line)', borderRadius: 7,
  color: 'var(--hds-txt)', fontSize: 12,
  fontFamily: "'JetBrains Mono', monospace", outline: 'none',
}

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

  const gridStyle: React.CSSProperties = {
    display: 'grid',
    gridTemplateColumns: 'repeat(auto-fill, minmax(128px, 1fr))',
    gap: 10, padding: 14, alignContent: 'start',
  }

  return (
    <div style={{ position: 'fixed', inset: 0, zIndex: 200, display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'oklch(0.08 0.015 286 / 0.85)' }}>
      <div style={{ position: 'relative', display: 'flex', flexDirection: 'column', width: 'min(98vw, 1200px)', height: '86vh', background: 'linear-gradient(168deg, oklch(0.18 0.024 290 / 0.98), oklch(0.13 0.018 288 / 0.99))', borderRadius: 14, border: '1px solid var(--hds-line)', boxShadow: '0 32px 80px -16px rgba(0,0,0,0.8)', overflow: 'hidden' }}>
        <div style={{ position: 'absolute', top: 0, left: 0, right: 0, height: 1, background: 'linear-gradient(90deg, transparent, oklch(0.78 0.15 292 / 0.7) 40%, oklch(0.85 0.13 84 / 0.5) 75%, transparent)', pointerEvents: 'none' }} />

        {/* Header */}
        <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 14, padding: '16px 22px 14px', borderBottom: '1px solid var(--hds-line-s)' }}>
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 16, letterSpacing: '0.04em' }}>Content Overrides</span>
          <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{user.username} — always wins over the rating ceiling</span>
          <div style={{ flex: 1 }} />
          <button onClick={onClose}
            style={{ width: 34, height: 34, display: 'flex', alignItems: 'center', justifyContent: 'center', borderRadius: 9, border: '1px solid var(--hds-line-s)', background: 'transparent', color: 'var(--hds-txt-2)', fontSize: 18, cursor: 'pointer' }}>×</button>
        </div>

        {/* Body */}
        <div style={{ flex: 1, minHeight: 0, display: 'flex' }}>

          {/* Left: tile browser */}
          <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
            <div style={{ flexShrink: 0, padding: '10px 14px 0', borderBottom: '1px solid var(--hds-line-s)' }}>
              <div style={{ display: 'flex', gap: 10, marginBottom: 8, alignItems: 'center' }}>
                <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 7, padding: 3, width: 'fit-content' }}>
                  {(['shows', 'movies', 'channels'] as OverrideTab[]).map(t => (
                    <button key={t} onClick={() => { setTab(t); setQ('') }}
                      style={{ padding: '4px 12px', border: 'none', borderRadius: 5, background: tab === t ? 'var(--hds-violet)' : 'transparent', color: tab === t ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer', textTransform: 'capitalize' }}>
                      {t}
                    </button>
                  ))}
                </div>
                <div style={{ flex: 1 }} />
                <div style={{ display: 'flex', gap: 2, background: 'var(--hds-bg-3)', borderRadius: 7, padding: 3 }}>
                  <button onClick={() => setMode('allow')}
                    style={{ padding: '4px 12px', border: 'none', borderRadius: 5, background: mode === 'allow' ? 'oklch(0.6 0.15 145)' : 'transparent', color: mode === 'allow' ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, fontWeight: 700, cursor: 'pointer' }}>
                    Allow
                  </button>
                  <button onClick={() => setMode('block')}
                    style={{ padding: '4px 12px', border: 'none', borderRadius: 5, background: mode === 'block' ? 'oklch(0.6 0.2 22)' : 'transparent', color: mode === 'block' ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, fontWeight: 700, cursor: 'pointer' }}>
                    Block
                  </button>
                </div>
              </div>
              <div style={{ marginBottom: 10 }}>
                <input value={q} onChange={e => setQ(e.target.value)} placeholder="Search…"
                  style={{ ...inputStyle, width: '100%', boxSizing: 'border-box', fontSize: 11.5, padding: '6px 9px' }} />
              </div>
            </div>

            <div style={{ flex: 1, overflow: 'auto' }} className="scrollbar-dark">
              {tab === 'shows' ? (
                loading ? <div style={{ padding: '20px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
                : shows.length === 0 ? <BrowserEmpty /> : (
                  <>
                    <div style={gridStyle}>
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
                loading ? <div style={{ padding: '20px 14px', color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
                : movies.length === 0 ? <BrowserEmpty /> : (
                  <>
                    <div style={gridStyle}>
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
                  <div style={gridStyle}>
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
          <div style={{ flexShrink: 0, width: 340, borderLeft: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', background: 'oklch(0.15 0.018 288 / 0.55)', overflow: 'auto', padding: 18 }} className="scrollbar-dark">
            <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 15 }}>
              <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 11, letterSpacing: '0.14em', color: 'var(--hds-gold)' }}>OVERRIDES</span>
              <span style={{ fontSize: 11, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{overrides.length}</span>
            </div>

            <div style={{ display: 'flex', flexDirection: 'column', gap: 7 }}>
              {overrides.map(o => (
                <div key={`${o.entity_type}:${o.entity_id}`} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '9px 11px', background: 'oklch(0.21 0.024 290 / 0.5)', border: '1px solid var(--hds-line-s)', borderRadius: 9 }}>
                  <span style={{
                    fontSize: 9.5, padding: '2px 6px', borderRadius: 3,
                    background: o.mode === 'allow' ? 'oklch(0.6 0.15 145 / 0.15)' : 'oklch(0.6 0.2 22 / 0.15)',
                    color:      o.mode === 'allow' ? 'oklch(0.75 0.15 145)'       : 'oklch(0.75 0.18 22)',
                    fontFamily: "'JetBrains Mono', monospace", flexShrink: 0, fontWeight: 700,
                  }}>{o.mode}</span>
                  <span style={{ fontSize: 9.5, padding: '2px 6px', borderRadius: 3, background: 'var(--hds-bg)', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", flexShrink: 0 }}>{o.entity_type}</span>
                  <span style={{ flex: 1, fontSize: 11.5, fontWeight: 500, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{o.title || o.entity_id}</span>
                  <button onClick={() => remove(o.entity_type, o.entity_id)}
                    style={{ width: 22, height: 22, border: 'none', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13, flexShrink: 0 }}>×</button>
                </div>
              ))}
              {overrides.length === 0 && (
                <div style={{ padding: '16px 12px', textAlign: 'center', fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", border: '1px dashed var(--hds-line-s)', borderRadius: 8 }}>
                  No overrides — the rating ceiling alone applies
                </div>
              )}
            </div>
          </div>
        </div>

        {/* Footer */}
        <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 11, padding: '14px 22px', borderTop: '1px solid var(--hds-line-s)' }}>
          <span style={{ flex: 1, fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
            Click a tile to {mode === 'allow' ? 'allow' : 'block'} it for this account
          </span>
          {err && <span style={{ fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{err}</span>}
          <button onClick={onClose}
            style={{ padding: '11px 26px', borderRadius: 10, background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))', color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 14, letterSpacing: '0.04em', border: 'none', cursor: 'pointer' }}
            className="hds-btn-gold">Done</button>
        </div>
      </div>
    </div>
  )
})

export default UserOverridesOverlay
