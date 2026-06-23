import { useState, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import { api } from '../api/client'
import { inputStyle } from './styles'
import type { ChannelDetailStore } from './store'
import type { Show, EpisodeSearchResult, Playlist } from '../api/types'

type BumperSlotKey = 'intro' | 'outro' | 'interstitial'
type BumperPickerTab = 'shows' | 'playlists' | 'episodes'
type ArmedItem = { content_type: 'show' | 'playlist' | 'episode'; content_id: string; title: string }

const SLOT_META: Record<BumperSlotKey, { label: string; hint: string }> = {
  intro:         { label: 'INTRO',         hint: 'Plays once before the first content item.' },
  outro:         { label: 'OUTRO',         hint: 'Plays after the last content item when the program count is hit.' },
  interstitial:  { label: 'INTERSTITIAL',  hint: 'Plays between show transitions.' },
}

function slotContentType(store: ChannelDetailStore, slot: BumperSlotKey): string {
  if (slot === 'intro')        return store.draft.intro_content_type        ?? ''
  if (slot === 'outro')        return store.draft.outro_content_type        ?? ''
  if (slot === 'interstitial') return store.draft.interstitial_content_type ?? ''
  return ''
}

function slotContentId(store: ChannelDetailStore, slot: BumperSlotKey): string {
  if (slot === 'intro')        return store.draft.intro_content_id        ?? ''
  if (slot === 'outro')        return store.draft.outro_content_id        ?? ''
  if (slot === 'interstitial') return store.draft.interstitial_content_id ?? ''
  return ''
}

function setSlot(store: ChannelDetailStore, slot: BumperSlotKey, ct: string, cid: string) {
  if (slot === 'intro')        { store.setDraft('intro_content_type' as any, ct);        store.setDraft('intro_content_id' as any, cid) }
  if (slot === 'outro')        { store.setDraft('outro_content_type' as any, ct);        store.setDraft('outro_content_id' as any, cid) }
  if (slot === 'interstitial') { store.setDraft('interstitial_content_type' as any, ct); store.setDraft('interstitial_content_id' as any, cid) }
}

function clearSlot(store: ChannelDetailStore, slot: BumperSlotKey) {
  setSlot(store, slot, '', '')
}

const BumperOverlay = observer(function BumperOverlay({ store }: { store: ChannelDetailStore }) {
  const [tab,     setTab]     = useState<BumperPickerTab>('shows')
  const [q,       setQ]       = useState('')
  const [sFilter, setSFilter] = useState('')
  const [shows,   setShows]   = useState<Show[]>([])
  const [lists,   setLists]   = useState<Playlist[]>([])
  const [eps,     setEps]     = useState<EpisodeSearchResult[]>([])
  const [loading, setLoading] = useState(false)
  const [armed,   setArmed]   = useState<ArmedItem | null>(null)
  const [dragItem,setDragItem]= useState<ArmedItem | null>(null)

  useEffect(() => {
    setLoading(true)
    const season = sFilter.trim() !== '' ? parseInt(sFilter, 10) : undefined
    const p =
      tab === 'shows'     ? api.getShows({ limit: 80, q: q || undefined }).then(r => { setShows(r.items); setLoading(false) }) :
      tab === 'playlists' ? api.getPlaylists().then(r => { setLists(r); setLoading(false) }) :
      api.searchEpisodes({ q: q || undefined, season: Number.isFinite(season) ? season : undefined, limit: 40 }).then(r => { setEps(r.items); setLoading(false) })
    p.catch(() => setLoading(false))
  }, [tab, q, sFilter])

  const assignToSlot = (slot: BumperSlotKey, item: ArmedItem) => {
    setSlot(store, slot, item.content_type, item.content_id)
    setArmed(null)
  }

  const armedHint = armed ? `Click a slot to assign "${armed.title}"` : 'Click a tile, then a slot to assign'

  const tileBase: React.CSSProperties = {
    display: 'flex', flexDirection: 'column', gap: 6, cursor: 'pointer',
    borderRadius: 9, overflow: 'hidden', border: '1px solid var(--hds-line-s)',
    background: 'var(--hds-bg-2)', transition: 'border-color .1s',
  }

  const renderTile = (item: ArmedItem) => {
    const isArmed = armed?.content_type === item.content_type && armed?.content_id === item.content_id
    return (
      <div
        key={item.content_id}
        draggable
        onDragStart={() => setDragItem(item)}
        onDragEnd={() => setDragItem(null)}
        onClick={() => setArmed(isArmed ? null : item)}
        style={{
          ...tileBase,
          borderColor: isArmed ? 'var(--hds-violet)' : 'var(--hds-line-s)',
          boxShadow: isArmed ? '0 0 0 1px var(--hds-violet)' : 'none',
        }}
        onMouseEnter={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-violet)' }}
        onMouseLeave={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line-s)' }}
      >
        <div style={{ width: '100%', aspectRatio: '2/3', background: 'var(--hds-bg-3)', position: 'relative', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
          {item.content_type === 'show' && (
            <img
              src={`/api/shows/${item.content_id}/thumb`} alt=""
              style={{ position: 'absolute', inset: 0, width: '100%', height: '100%', objectFit: 'cover' }}
              onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
            />
          )}
          <span style={{ fontSize: 22, opacity: 0.3, position: 'relative' }}>
            {item.content_type === 'playlist' ? '☰' : item.content_type === 'episode' ? '▶' : '◍'}
          </span>
          {isArmed && (
            <div style={{ position: 'absolute', inset: 0, background: 'oklch(0.55 0.14 292 / 0.3)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
              <span style={{ fontSize: 11, color: 'oklch(0.9 0.1 292)', fontFamily: "'JetBrains Mono', monospace" }}>armed</span>
            </div>
          )}
        </div>
        <div style={{ padding: '5px 7px 7px' }}>
          <div style={{ fontSize: 11, fontWeight: 600, lineHeight: 1.3, overflow: 'hidden', display: '-webkit-box', WebkitLineClamp: 2, WebkitBoxOrient: 'vertical' }}>
            {item.title}
          </div>
          <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 2, fontFamily: "'JetBrains Mono', monospace" }}>
            {item.content_type}
          </div>
        </div>
      </div>
    )
  }

  return (
    <div style={{ position: 'absolute', inset: 0, zIndex: 82, display: 'flex', flexDirection: 'column', background: 'linear-gradient(168deg, oklch(0.18 0.024 290 / 0.96), oklch(0.13 0.018 288 / 0.98))', backdropFilter: 'blur(26px)' }}>
      <div style={{ position: 'absolute', top: 0, left: 0, right: 0, height: 1, background: 'linear-gradient(90deg, transparent, oklch(0.78 0.15 292 / 0.7) 40%, oklch(0.85 0.13 84 / 0.5) 75%, transparent)', pointerEvents: 'none' }} />

      {/* Header */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 14, padding: '16px 22px 14px', borderBottom: '1px solid var(--hds-line-s)' }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 16, letterSpacing: '0.04em' }}>Bumpers</span>
        <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>drag a tile into a slot · or click it, then a slot</span>
        <div style={{ flex: 1 }} />
        <div style={{ display: 'flex', gap: 3, background: 'var(--hds-bg-3)', borderRadius: 9, padding: 3 }}>
          {(['shows', 'playlists', 'episodes'] as BumperPickerTab[]).map(t => (
            <button key={t} onClick={() => { setTab(t); setQ('') }}
              style={{ padding: '5px 12px', border: 'none', borderRadius: 6, background: tab === t ? 'var(--hds-violet)' : 'transparent', color: tab === t ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer', textTransform: 'capitalize' }}>
              {t}
            </button>
          ))}
        </div>
        <input value={q} onInput={e => setQ((e.target as HTMLInputElement).value)} placeholder="Search…"
          style={{ ...inputStyle, width: 210, fontSize: 13 }} />
        {tab === 'episodes' && (
          <input type="number" min={0} value={sFilter} onChange={e => setSFilter(e.target.value)} placeholder="S#"
            style={{ ...inputStyle, width: 52, fontSize: 11 }} />
        )}
        <button onClick={() => { store.bumperOverlayOpen = false }}
          style={{ width: 34, height: 34, display: 'flex', alignItems: 'center', justifyContent: 'center', borderRadius: 9, border: '1px solid var(--hds-line-s)', background: 'transparent', color: 'var(--hds-txt-2)', fontSize: 18, cursor: 'pointer' }}>×</button>
      </div>

      {/* Body */}
      <div style={{ flex: 1, minHeight: 0, display: 'flex' }}>
        {/* Tile grid */}
        <div style={{ flex: 1, minWidth: 0, overflow: 'auto', padding: '18px 22px' }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 14, fontFamily: "'JetBrains Mono', monospace" }}>
            {tab === 'shows' ? 'SHOWS' : tab === 'playlists' ? 'PLAYLISTS' : 'EPISODES'}
          </div>
          {loading ? (
            <div style={{ color: 'var(--hds-txt-3)', fontSize: 12 }}>Loading…</div>
          ) : (
            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(140px, 1fr))', gap: 14, alignContent: 'start' }}>
              {tab === 'shows' && shows.filter(s => !q || s.title.toLowerCase().includes(q.toLowerCase())).map(s =>
                renderTile({ content_type: 'show', content_id: s.show_id, title: s.title })
              )}
              {tab === 'playlists' && lists.filter(p => !q || p.title.toLowerCase().includes(q.toLowerCase())).map(p =>
                renderTile({ content_type: 'playlist', content_id: p.playlist_id, title: p.title })
              )}
              {tab === 'episodes' && eps.map(ep =>
                renderTile({
                  content_type: 'episode',
                  content_id: ep.episode_id,
                  title: `${ep.show_title} S${String(ep.season).padStart(2,'0')}E${String(ep.episode).padStart(2,'0')} — ${ep.title}`,
                })
              )}
            </div>
          )}
          {!loading && tab === 'shows'     && shows.length === 0    && <div style={{ color: 'var(--hds-txt-3)', fontSize: 12 }}>No results.</div>}
          {!loading && tab === 'playlists' && lists.length === 0     && <div style={{ color: 'var(--hds-txt-3)', fontSize: 12 }}>No playlists.</div>}
          {!loading && tab === 'episodes'  && eps.length === 0       && <div style={{ color: 'var(--hds-txt-3)', fontSize: 12 }}>Type to search episodes.</div>}
        </div>

        {/* Slots panel */}
        <div style={{ flexShrink: 0, width: 380, borderLeft: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', background: 'oklch(0.15 0.018 288 / 0.55)', overflow: 'auto', padding: 18 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 15 }}>
            <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 11, letterSpacing: '0.14em', color: 'var(--hds-gold)' }}>SLOTS</span>
            <div style={{ flex: 1 }} />
            <span style={{ fontSize: 10, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{armedHint}</span>
          </div>

          <div style={{ display: 'flex', flexDirection: 'column', gap: 13 }}>
            {(['intro', 'outro', 'interstitial'] as BumperSlotKey[]).map(slot => {
              const meta  = SLOT_META[slot]
              const ct    = slotContentType(store, slot)
              const cid   = slotContentId(store, slot)
              const hasCt = ct !== '' && cid !== ''

              const onDragOver = (e: React.DragEvent) => { e.preventDefault(); (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-violet)' }
              const onDragLeave = (e: React.DragEvent) => { (e.currentTarget as HTMLDivElement).style.borderColor = hasCt ? 'var(--hds-line)' : 'var(--hds-line-s)' }
              const onDrop = (e: React.DragEvent) => {
                e.preventDefault()
                if (dragItem) { assignToSlot(slot, dragItem); setDragItem(null) }
                ;(e.currentTarget as HTMLDivElement).style.borderColor = ''
              }
              const onClick = () => {
                if (armed) assignToSlot(slot, armed)
              }

              return (
                <div key={slot}>
                  <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 6 }}>
                    <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 10, letterSpacing: '0.14em', color: 'var(--hds-txt-2)' }}>{meta.label}</span>
                    <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{meta.hint}</span>
                  </div>
                  <div
                    onClick={onClick}
                    onDragOver={onDragOver}
                    onDragLeave={onDragLeave}
                    onDrop={onDrop}
                    style={{
                      display: 'flex', alignItems: 'center', gap: 8, padding: '9px 11px',
                      borderRadius: 9, border: `1px solid ${hasCt ? 'var(--hds-line)' : 'var(--hds-line-s)'}`,
                      background: hasCt ? 'oklch(0.21 0.024 290 / 0.4)' : 'oklch(0.17 0.018 288 / 0.35)',
                      cursor: armed ? 'pointer' : 'default', minHeight: 40, transition: 'border-color .12s',
                    }}
                  >
                    {hasCt ? (
                      <>
                        <span style={{ fontSize: 9.5, padding: '2px 6px', borderRadius: 3, background: 'var(--hds-bg)', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace', letterSpacing: '0.06em'", flexShrink: 0 }}>{ct}</span>
                        <span style={{ flex: 1, fontSize: 12, color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{cid}</span>
                        <button
                          onClick={e => { e.stopPropagation(); clearSlot(store, slot) }}
                          style={{ width: 22, height: 22, flexShrink: 0, display: 'flex', alignItems: 'center', justifyContent: 'center', borderRadius: 6, background: 'transparent', border: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13 }}
                        >×</button>
                      </>
                    ) : (
                      <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
                        {armed ? '← Click to assign' : '— empty —'}
                      </span>
                    )}
                  </div>
                  {slot === 'interstitial' && (
                    <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginTop: 7 }}>
                      <span style={{ fontSize: 10.5, color: 'var(--hds-txt-3)' }}>Fire every</span>
                      <input
                        type="number" min={1} value={store.draft.interstitial_every_n ?? 1}
                        onChange={e => store.setDraft('interstitial_every_n' as any, Math.max(1, Number(e.target.value)))}
                        style={{ ...inputStyle, width: 56, padding: '4px 7px', fontSize: 11 }}
                      />
                      <span style={{ fontSize: 10.5, color: 'var(--hds-txt-3)' }}>show transition(s)</span>
                    </div>
                  )}
                </div>
              )
            })}
          </div>
        </div>
      </div>

      {/* Footer */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 11, padding: '14px 22px', borderTop: '1px solid var(--hds-line-s)' }}>
        <span style={{ flex: 1, fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
          {(['intro', 'outro', 'interstitial'] as BumperSlotKey[]).filter(s => slotContentId(store, s) !== '').length} slot{(['intro', 'outro', 'interstitial'] as BumperSlotKey[]).filter(s => slotContentId(store, s) !== '').length !== 1 ? 's' : ''} configured
        </span>
        <button
          onClick={() => { store.bumperOverlayOpen = false }}
          style={{ padding: '11px 26px', borderRadius: 10, background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))', color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 14, letterSpacing: '0.04em', border: 'none', cursor: 'pointer' }}
        >Done</button>
      </div>
    </div>
  )
})

export default BumperOverlay
