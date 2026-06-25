import { useEffect, useState } from 'react'
import { observer } from 'mobx-react-lite'
import { BLOCK_META } from './constants'
import { getLimitMode } from './utils'
import { goldBtnStyle, ghostBtnStyle, dangerBtnStyle, inputStyle } from './styles'
import { EditorForm } from './EditorForm'
import { LibraryBrowser } from './LibraryBrowser'
import FillerOverlay from './FillerOverlay'
import BumperOverlay from './BumperOverlay'
import { api } from '../api/client'
import type { ChannelDetailStore } from './store'
import type { Source } from '../api/types'

function fmtMs(ms: number) {
  const d = new Date(ms)
  return d.toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit', hour12: false })
}

function fmtDur(ms: number) {
  const m = Math.round(ms / 60000)
  return m >= 60 ? `${Math.floor(m / 60)}h ${m % 60}m` : `${m}m`
}

const BlockEditorModal = observer(function BlockEditorModal({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const d         = store.draft
  const m         = BLOCK_META[d.block_type]
  const limitMode = getLimitMode(d)

  const [exportOpen,    setExportOpen]    = useState(false)
  const [exportTitle,   setExportTitle]   = useState('')
  const [exportSrcId,   setExportSrcId]   = useState('')
  const [exportSaving,  setExportSaving]  = useState(false)
  const [exportResult,  setExportResult]  = useState<{ playlist_id: string; item_count: number; plex_playlist_id?: string } | null>(null)
  const [exportError,   setExportError]   = useState<string | null>(null)
  const [plexSources,   setPlexSources]   = useState<Source[]>([])

  const openExport = () => {
    setExportTitle(store.draft.name?.trim() || 'Block Playlist')
    setExportSrcId('')
    setExportResult(null)
    setExportError(null)
    setExportOpen(true)
    api.getSources().then(ss => setPlexSources(ss.filter(s => s.source_type === 'plex'))).catch(() => {})
  }

  const doExport = async () => {
    if (!store.editing) return
    setExportSaving(true)
    setExportError(null)
    try {
      const r = await api.blockExportPlaylist(channelId, store.editing.block_id, {
        title: exportTitle.trim(),
        ...(exportSrcId ? { source_id: exportSrcId } : {}),
      })
      setExportResult(r)
    } catch (e: any) {
      setExportError(e?.message ?? 'Failed to create playlist')
    } finally {
      setExportSaving(false)
    }
  }

  useEffect(() => {
    const esc = (e: KeyboardEvent) => {
      if (e.key !== 'Escape') return
      if (store.pickerOpen)        { store.pickerOpen        = false; return }
      if (store.fillerOverlayOpen) { store.fillerOverlayOpen = false; return }
      if (store.bumperOverlayOpen) { store.bumperOverlayOpen = false; return }
      store.modalOpen = false
    }
    document.addEventListener('keydown', esc)
    return () => document.removeEventListener('keydown', esc)
  }, [])

  const blockEpg = store.epgItems.filter(p => p.block_id === store.editing?.block_id)

  return (
    <div
      style={{ position: 'fixed', inset: 0, zIndex: 200, display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'oklch(0.08 0.015 286 / 0.85)' }}
      onClick={e => { if (e.target === e.currentTarget) store.modalOpen = false }}
    >
      <div style={{ position: 'relative', display: 'flex', flexDirection: 'column', width: 'min(98vw, 1340px)', height: '92vh', background: 'var(--hds-bg-2)', borderRadius: 14, border: '1px solid var(--hds-line)', boxShadow: '0 32px 80px -16px rgba(0,0,0,0.8)', overflow: 'hidden', fontFamily: "'JetBrains Mono', monospace", fontSize: 13, color: 'var(--hds-txt)' }}>

        {/* ── Header ── */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 12, padding: '13px 18px 11px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
          <span style={{
            padding: '3px 9px', borderRadius: 5, flexShrink: 0,
            background: `${m.edge}22`, border: `1px solid ${m.edge}66`,
            color: m.edge, fontFamily: "'JetBrains Mono', monospace",
            fontSize: 10, fontWeight: 700, letterSpacing: '0.12em',
          }}>
            {m.name.toUpperCase()}
          </span>
          <input
            type="text"
            placeholder="Block name (optional)"
            value={d.name ?? ''}
            onChange={e => store.setDraft('name', e.target.value)}
            style={{ ...inputStyle, flex: 1, fontFamily: "'Space Grotesk', sans-serif", fontSize: 14, fontWeight: 500 }}
          />
          <button
            onClick={() => store.toggleHints()}
            style={{ padding: '4px 10px', border: '1px solid var(--hds-line)', borderRadius: 6, background: 'transparent', color: store.showHints ? 'var(--hds-violet)' : 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer', letterSpacing: '0.08em', whiteSpace: 'nowrap', flexShrink: 0 }}
          >
            {store.showHints ? '— hints' : '+ hints'}
          </button>
          <button
            onClick={() => { store.modalOpen = false }}
            style={{ width: 30, height: 30, border: 'none', borderRadius: 8, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 18, flexShrink: 0 }}
          >×</button>
        </div>

        {/* ── 3-column body ── */}
        <div style={{ display: 'flex', flex: 1, minHeight: 0, position: 'relative' }}>

          {/* Left: Schedule & Rules */}
          <div style={{ width: 336, flexShrink: 0, borderRight: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
            <div style={{ padding: '9px 14px 5px', flexShrink: 0 }}>
              <span style={{ fontSize: 9, letterSpacing: '0.2em', color: 'var(--hds-txt-3)' }}>① SCHEDULE & RULES</span>
            </div>
            <EditorForm channelId={channelId} store={store} limitMode={limitMode} hidePicker compact />
          </div>

          {/* Center: Content */}
          <div style={{ flex: 1, minWidth: 0, borderRight: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
            <div style={{ padding: '9px 14px 5px', flexShrink: 0, display: 'flex', alignItems: 'center', gap: 8 }}>
              <span style={{ fontSize: 9, letterSpacing: '0.2em', color: 'var(--hds-txt-3)', flex: 1 }}>② CONTENT</span>
              {store.draftContent.length > 0 && (
                <span style={{ fontSize: 10, color: 'var(--hds-violet)' }}>{store.draftContent.length}</span>
              )}
              <button
                onClick={() => store.openPicker()}
                style={{ padding: '3px 10px', border: '1px solid var(--hds-violet)', borderRadius: 5, background: 'transparent', color: 'var(--hds-violet)', fontSize: 10, cursor: 'pointer' }}
              >+ Add</button>
            </div>

            <div style={{ flex: 1, minHeight: 0, overflow: 'auto', padding: '6px 14px 16px' }} className="scrollbar-dark">
              {store.draftContent.length === 0 ? (
                <div
                  style={{ height: '100%', display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 10, color: 'var(--hds-txt-3)', cursor: 'pointer' }}
                  onClick={() => store.openPicker()}
                >
                  <span style={{ fontSize: 32, opacity: 0.2 }}>+</span>
                  <span style={{ fontSize: 11 }}>Add content to this block</span>
                </div>
              ) : (
                <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(108px, 1fr))', gap: 10, alignContent: 'start' }}>
                  {store.draftContent.map(item => {
                    const dot      = BLOCK_META[item.content_type === 'movie' ? 'movie' : 'episode'].edge
                    const thumbUrl = item.content_type === 'show'  ? `/api/shows/${item.content_id}/thumb`
                                   : item.content_type === 'movie' ? `/api/movies/${item.content_id}/thumb`
                                   : null
                    return (
                      <div key={item.id} style={{ display: 'flex', flexDirection: 'column', borderRadius: 9, overflow: 'hidden', border: '1px solid var(--hds-line-s)', background: 'var(--hds-bg-3)' }}>
                        <div style={{ width: '100%', aspectRatio: '2/3', background: 'var(--hds-bg)', display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden' }}>
                          {thumbUrl ? (
                            <img src={thumbUrl} loading="lazy"
                              style={{ width: '100%', height: '100%', objectFit: 'cover', opacity: 0, transition: 'opacity .2s' }}
                              onLoad={e  => { (e.target as HTMLImageElement).style.opacity = '1' }}
                              onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
                          ) : (
                            <span style={{ fontSize: 22, opacity: 0.2 }}>·</span>
                          )}
                        </div>
                        <div style={{ padding: '5px 7px 3px', flex: 1 }}>
                          <div style={{ fontSize: 11, fontWeight: 600, lineHeight: 1.3, overflow: 'hidden', display: '-webkit-box', WebkitLineClamp: 2, WebkitBoxOrient: 'vertical' }}>
                            {item.title || item.content_id}
                          </div>
                          <div style={{ display: 'flex', alignItems: 'center', gap: 4, marginTop: 3 }}>
                            <span style={{ width: 5, height: 5, borderRadius: 1, background: dot, flexShrink: 0 }} />
                            <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)' }}>{item.content_type}</span>
                          </div>
                        </div>
                        <button
                          onClick={() => store.removeContent(channelId, item.id)}
                          style={{ margin: '2px 6px 6px', padding: '2px 0', border: 'none', borderRadius: 4, background: 'oklch(0.45 0.1 18 / 0.15)', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 10 }}
                        >remove</button>
                      </div>
                    )
                  })}
                </div>
              )}
            </div>
          </div>

          {/* Right: Playout timeline */}
          <div style={{ width: 310, flexShrink: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
            <div style={{ padding: '9px 14px 5px', flexShrink: 0 }}>
              <span style={{ fontSize: 9, letterSpacing: '0.2em', color: 'var(--hds-txt-3)' }}>③ PLAYOUT</span>
            </div>
            <div style={{ flex: 1, minHeight: 0, overflow: 'auto', padding: '4px 14px 16px' }} className="scrollbar-dark">
              {blockEpg.length === 0 ? (
                <div style={{ padding: '32px 0', textAlign: 'center', color: 'var(--hds-txt-3)', fontSize: 11 }}>
                  {store.editing ? 'No EPG data yet' : 'Preview available after saving'}
                </div>
              ) : blockEpg.map((prog, i) => (
                <div key={i} style={{ display: 'flex', alignItems: 'flex-start', gap: 8, padding: '7px 0', borderBottom: '1px solid var(--hds-line-s)' }}>
                  <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', flexShrink: 0, paddingTop: 1, minWidth: 40 }}>
                    {fmtMs(prog.wall_clock_start_ms)}
                  </span>
                  <div style={{ flex: 1, minWidth: 0 }}>
                    <div style={{ fontSize: 11, fontWeight: 500, lineHeight: 1.3, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                      {prog.title}
                    </div>
                    {prog.show_title && (
                      <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                        {prog.show_title}
                      </div>
                    )}
                  </div>
                  <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', flexShrink: 0, paddingTop: 1 }}>
                    {fmtDur(prog.duration_ms)}
                  </span>
                </div>
              ))}
            </div>
          </div>

          {/* ── Overlays (rendered above the 3 columns) ── */}
          {store.pickerOpen && (
            <div style={{ position: 'absolute', inset: 0, zIndex: 80, display: 'flex', flexDirection: 'column', background: 'linear-gradient(168deg, oklch(0.18 0.024 290 / 0.97), oklch(0.13 0.018 288 / 0.99))', backdropFilter: 'blur(24px)' }}>
              <div style={{ position: 'absolute', top: 0, left: 0, right: 0, height: 1, background: 'linear-gradient(90deg, transparent, oklch(0.78 0.15 292 / 0.7) 40%, oklch(0.85 0.13 84 / 0.5) 75%, transparent)', pointerEvents: 'none' }} />
              <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 12, padding: '16px 22px 14px', borderBottom: '1px solid var(--hds-line-s)' }}>
                <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 16, letterSpacing: '0.04em' }}>Add Content</span>
                <div style={{ flex: 1 }} />
                <button
                  onClick={() => { store.pickerOpen = false }}
                  style={{ width: 34, height: 34, display: 'flex', alignItems: 'center', justifyContent: 'center', borderRadius: 9, border: '1px solid var(--hds-line-s)', background: 'transparent', color: 'var(--hds-txt-2)', fontSize: 18, cursor: 'pointer' }}
                >×</button>
              </div>
              <div style={{ flex: 1, minHeight: 0, display: 'flex' }}>
                {/* Left: tile browser */}
                <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
                  <LibraryBrowser channelId={channelId} store={store} />
                </div>
                {/* Right: content list */}
                <div style={{ flexShrink: 0, width: 360, borderLeft: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', background: 'oklch(0.15 0.018 288 / 0.55)', overflow: 'hidden' }}>
                  <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 8, padding: '12px 16px 10px', borderBottom: '1px solid var(--hds-line-s)' }}>
                    <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 11, letterSpacing: '0.14em', color: 'var(--hds-gold)' }}>CONTENT</span>
                    {store.draftContent.length > 0 && <span style={{ fontSize: 11, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{store.draftContent.length}</span>}
                  </div>
                  <div style={{ flex: 1, overflow: 'auto', padding: '8px 12px 16px' }} className="scrollbar-dark">
                    {store.draftContent.length === 0 ? (
                      <div style={{ padding: '24px 0', textAlign: 'center', fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
                        Nothing added yet
                      </div>
                    ) : (
                      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(100px, 1fr))', gap: 8, alignContent: 'start' }}>
                        {store.draftContent.map(item => {
                          const dot      = BLOCK_META[item.content_type === 'movie' ? 'movie' : 'episode'].edge
                          const thumbUrl = item.content_type === 'show'  ? `/api/shows/${item.content_id}/thumb`
                                         : item.content_type === 'movie' ? `/api/movies/${item.content_id}/thumb`
                                         : null
                          return (
                            <div key={item.id} style={{ display: 'flex', flexDirection: 'column', borderRadius: 8, overflow: 'hidden', border: '1px solid var(--hds-line-s)', background: 'var(--hds-bg-3)' }}>
                              <div style={{ width: '100%', aspectRatio: '2/3', background: 'var(--hds-bg)', display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden' }}>
                                {thumbUrl ? (
                                  <img src={thumbUrl} loading="lazy"
                                    style={{ width: '100%', height: '100%', objectFit: 'cover', opacity: 0, transition: 'opacity .2s' }}
                                    onLoad={e  => { (e.target as HTMLImageElement).style.opacity = '1' }}
                                    onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
                                ) : (
                                  <span style={{ fontSize: 18, opacity: 0.2 }}>·</span>
                                )}
                              </div>
                              <div style={{ padding: '4px 6px 3px', flex: 1 }}>
                                <div style={{ fontSize: 10.5, fontWeight: 600, lineHeight: 1.3, overflow: 'hidden', display: '-webkit-box', WebkitLineClamp: 2, WebkitBoxOrient: 'vertical' }}>
                                  {item.title || item.content_id}
                                </div>
                                <div style={{ display: 'flex', alignItems: 'center', gap: 3, marginTop: 2 }}>
                                  <span style={{ width: 5, height: 5, borderRadius: 1, background: dot, flexShrink: 0 }} />
                                  <span style={{ fontSize: 9, color: 'var(--hds-txt-3)' }}>{item.content_type}</span>
                                </div>
                              </div>
                              <button
                                onClick={() => store.removeContent(channelId, item.id)}
                                style={{ margin: '2px 5px 5px', padding: '2px 0', border: 'none', borderRadius: 4, background: 'oklch(0.45 0.1 18 / 0.15)', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 10 }}
                              >remove</button>
                            </div>
                          )
                        })}
                      </div>
                    )}
                  </div>
                </div>
              </div>
            </div>
          )}

          {store.fillerOverlayOpen && <FillerOverlay channelId={channelId} store={store} />}
          {store.bumperOverlayOpen && <BumperOverlay store={store} />}

          {exportOpen && (
            <div style={{ position: 'absolute', inset: 0, zIndex: 80, display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'oklch(0.08 0.015 286 / 0.88)', backdropFilter: 'blur(6px)' }}>
              <div style={{ width: 420, background: 'var(--hds-bg-2)', borderRadius: 12, border: '1px solid var(--hds-line)', padding: '20px 24px 24px', display: 'flex', flexDirection: 'column', gap: 14 }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                  <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 14, letterSpacing: '0.03em' }}>Create Playlist from Block</span>
                  <div style={{ flex: 1 }} />
                  <button onClick={() => setExportOpen(false)} style={{ width: 28, height: 28, border: 'none', background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 18 }}>×</button>
                </div>

                {exportResult ? (
                  <div style={{ fontSize: 12, color: 'oklch(0.78 0.18 140)', lineHeight: 1.7 }}>
                    <div>Kairos playlist created with {exportResult.item_count} items.</div>
                    {exportResult.plex_playlist_id && <div>Plex playlist created (ID {exportResult.plex_playlist_id}).</div>}
                    <div style={{ marginTop: 14 }}>
                      <button onClick={() => setExportOpen(false)} style={ghostBtnStyle}>Done</button>
                    </div>
                  </div>
                ) : (
                  <>
                    <div style={{ display: 'flex', flexDirection: 'column', gap: 5 }}>
                      <label style={{ fontSize: 10, letterSpacing: '0.14em', color: 'var(--hds-txt-3)' }}>PLAYLIST TITLE</label>
                      <input
                        autoFocus
                        type="text"
                        value={exportTitle}
                        onChange={e => setExportTitle(e.target.value)}
                        onKeyDown={e => { if (e.key === 'Enter' && exportTitle.trim()) doExport() }}
                        style={inputStyle}
                      />
                    </div>

                    {plexSources.length > 0 && (
                      <div style={{ display: 'flex', flexDirection: 'column', gap: 5 }}>
                        <label style={{ fontSize: 10, letterSpacing: '0.14em', color: 'var(--hds-txt-3)' }}>ALSO CREATE IN PLEX (OPTIONAL)</label>
                        <select
                          value={exportSrcId}
                          onChange={e => setExportSrcId(e.target.value)}
                          style={{ ...inputStyle, background: 'var(--hds-bg)', cursor: 'pointer' }}
                        >
                          <option value="">None — Kairos only</option>
                          {plexSources.map(s => <option key={s.source_id} value={s.source_id}>{s.display_name}</option>)}
                        </select>
                      </div>
                    )}

                    {exportError && (
                      <div style={{ fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{exportError}</div>
                    )}

                    <div style={{ display: 'flex', gap: 8 }}>
                      <button
                        onClick={doExport}
                        disabled={!exportTitle.trim() || exportSaving}
                        style={goldBtnStyle}
                      >
                        {exportSaving ? 'Creating…' : 'Create Playlist'}
                      </button>
                      <button onClick={() => setExportOpen(false)} style={ghostBtnStyle}>Cancel</button>
                    </div>
                  </>
                )}
              </div>
            </div>
          )}
        </div>

        {/* ── Footer ── */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '14px 22px', borderTop: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
          {store.editing ? (
            <>
              <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle}>
                {store.saving ? 'Saving…' : 'Save Changes'}
              </button>
              <button onClick={() => store.duplicate(channelId)} style={ghostBtnStyle}>⧉ Duplicate</button>
              <button onClick={openExport} style={ghostBtnStyle}>+ Playlist</button>
              <div style={{ flex: 1 }} />
              <button onClick={() => store.deleteBlock(channelId, store.editing!.block_id)} style={dangerBtnStyle}>Delete</button>
            </>
          ) : (
            <>
              <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle}>
                {store.saving ? 'Saving…' : 'Create Block'}
              </button>
              <button onClick={() => { store.closeEditor(); store.modalOpen = false }} style={ghostBtnStyle}>Cancel</button>
            </>
          )}
        </div>

        {store.saveErr && (
          <div style={{ padding: '7px 22px', fontSize: 11, color: 'oklch(0.72 0.16 22)', background: 'oklch(0.2 0.05 22 / 0.3)', borderTop: '1px solid oklch(0.4 0.1 22 / 0.4)', flexShrink: 0 }}>
            {store.saveErr}
          </div>
        )}
      </div>
    </div>
  )
})

export default BlockEditorModal
