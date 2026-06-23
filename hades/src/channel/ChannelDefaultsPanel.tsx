import { useState, useRef, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import type { AdvanceMode, BumperContentType, BumperMode, Channel, ChannelBumper, EpisodeSearchResult, FillerEntryAdvancement, FillerSelectionMode, Show, Playlist } from '../api/types'
import { FILLER_ADV_OPTS, FILLER_SEL_OPTS } from './constants'
import { inputStyle, filterInputStyle } from './styles'
import { CardSection } from './EditorForm'
import { FillerEntryRow } from './FillerPanel'
import type { ChannelDetailStore } from './store'
import { api } from '../api/client'

const ChannelDefaultsPanel = observer(function ChannelDefaultsPanel({ channel, channelId, store }: {
  channel:   Channel | undefined
  channelId: string
  store:     ChannelDetailStore
}) {
  const [addOpen, setAddOpen]     = useState(false)
  const [addListId, setAddListId] = useState('')
  const [addAdv, setAddAdv]       = useState<FillerEntryAdvancement>('sequential')
  const [addWeight, setAddWeight] = useState(1)

  const selectionMode = channel?.default_filler_selection ?? 'round_robin'
  const entries       = channel?.default_filler_entries   ?? []
  const showWeight    = selectionMode === 'weighted'

  // Channel bumpers
  const [bumpers,     setBumpers]     = useState<ChannelBumper[]>([])
  const [bumperErr,   setBumperErr]   = useState('')
  const [addBumpOpen, setAddBumpOpen] = useState(false)
  const [bCt,  setBCt]  = useState<BumperContentType>('show')
  const [bCid, setBCid] = useState('')
  const [bMode, setBMode] = useState<BumperMode>('between')
  const [bN,   setBN]   = useState(3)

  useEffect(() => {
    api.getBumpers(channelId).then(setBumpers).catch(() => {})
  }, [channelId])

  async function addBumper() {
    if (!bCid) return
    try {
      const { id } = await api.createBumper(channelId, { content_type: bCt, content_id: bCid, mode: bMode, every_n: bN })
      setBumpers(prev => [...prev, { id, channel_id: channelId, content_type: bCt, content_id: bCid, mode: bMode, every_n: bN, position: prev.length }])
      setBCid(''); setAddBumpOpen(false); setBumperErr('')
    } catch (e: any) { setBumperErr(e.message) }
  }

  async function deleteBumper(id: number) {
    try {
      await api.deleteBumper(channelId, id)
      setBumpers(prev => prev.filter(b => b.id !== id))
    } catch (e: any) { setBumperErr(e.message) }
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      <div style={{ padding: '18px 20px 14px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 15, letterSpacing: '0.04em' }}>Channel Settings</span>
      </div>

      <div style={{ flex: 1, overflow: 'auto', padding: '14px 16px 20px' }} className="scrollbar-dark">
        <div style={{ fontSize: 11.5, color: 'var(--hds-txt-3)', lineHeight: 1.6, marginBottom: 16, fontFamily: "'JetBrains Mono', monospace" }}>
          Select a block to edit it, or press <span style={{ color: 'var(--hds-gold)' }}>Add Block</span> to create one.
        </div>

        <CardSection title="CHANNEL">
        <div style={{ display: 'flex', gap: 7, marginBottom: 8 }}>
          <div style={{ flex: 1 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>NAME</div>
            <input value={store.channelDraftName} onChange={e => store.setChannelDraft({ name: e.target.value })} style={inputStyle} />
          </div>
          <div style={{ width: 64 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>CH #</div>
            <input type="number" min={1} value={store.channelDraftNumber} onChange={e => store.setChannelDraft({ number: Math.max(1, +e.target.value || 1) })} style={inputStyle} />
          </div>
        </div>
        <div style={{ marginBottom: 8 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>TIMEZONE</div>
          <input
            list="tz-suggestions"
            value={store.channelDraftTimezone}
            onChange={e => store.setChannelDraft({ timezone: e.target.value })}
            style={inputStyle}
            placeholder="America/Denver"
            spellCheck={false}
          />
          <datalist id="tz-suggestions">
            <option value="UTC" />
            <option value="America/New_York" />
            <option value="America/Chicago" />
            <option value="America/Denver" />
            <option value="America/Los_Angeles" />
            <option value="America/Anchorage" />
            <option value="Pacific/Honolulu" />
            <option value="Europe/London" />
            <option value="Europe/Paris" />
            <option value="Europe/Berlin" />
            <option value="Australia/Sydney" />
            <option value="Asia/Tokyo" />
          </datalist>
          <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4 }}>
            IANA format — e.g. <span style={{ fontFamily: "'JetBrains Mono', monospace" }}>America/Denver</span>
          </div>
        </div>
        <div style={{ marginBottom: 8 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>EPG SEED</div>
          <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
            <input type="number" min={0} value={store.channelDraftSeed} onChange={e => store.setChannelDraft({ seed: Math.max(0, +e.target.value || 0) })} style={{ ...inputStyle, flex: 1 }} />
            <button
              onClick={() => store.setChannelDraft({ seed: Math.floor(Math.random() * 99999) + 1 })}
              title="Randomize seed"
              style={{ padding: '5px 9px', border: '1px solid var(--hds-line)', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer' }}
            >⚄</button>
          </div>
          <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
            Controls the starting position for EPG simulation when no live schedule exists. Change to get a different ordering.
          </div>
        </div>
        <div style={{ marginBottom: 14 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>ADVANCE MODE</div>
          <select
            value={store.channelDraftAdvanceMode}
            onChange={e => store.setChannelDraft({ advance_mode: e.target.value as AdvanceMode })}
            style={inputStyle}
          >
            <option value="scheduled">Scheduled — advance on EPG clock</option>
            <option value="on_play">On Play — advance only when confirmed played</option>
          </select>
          <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
            On Play pauses the channel when nobody is streaming. Episodes only advance after Tunarr confirms playback.
          </div>
        </div>

        {store.channelSaveErr && (
          <div style={{ padding: '7px 10px', marginBottom: 10, borderRadius: 6, background: 'oklch(0.2 0.05 22 / 0.3)', border: '1px solid oklch(0.4 0.1 22 / 0.4)', color: 'oklch(0.72 0.16 22)', fontSize: 11 }}>
            {store.channelSaveErr}
          </div>
        )}

        <button
          onClick={() => store.saveChannel(channelId)}
          disabled={store.channelSaving}
          style={{ width: '100%', padding: '9px 0', border: 'none', borderRadius: 8, background: store.channelDirty ? 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))' : 'var(--hds-bg-3)', color: store.channelDirty ? 'oklch(0.2 0.04 70)' : 'var(--hds-txt-3)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 13, cursor: store.channelSaving ? 'default' : 'pointer', marginBottom: 22, opacity: store.channelSaving ? 0.6 : 1, transition: 'background 0.15s, color 0.15s' }}
        >
          {store.channelSaving ? 'Saving…' : 'Save Channel'}
        </button>
        </CardSection>

        <CardSection title="LOGO">
        <div style={{ marginBottom: 8 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>LOGO PATH</div>
          <input
            value={store.channelDraftLogoPath}
            onChange={e => store.setChannelDraft({ logo_path: e.target.value })}
            style={inputStyle}
            placeholder="/path/to/logo.png or https://…"
            spellCheck={false}
          />
          <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4 }}>
            Container path or URL to a PNG or JPG logo for this channel.
          </div>
        </div>
        </CardSection>

        <CardSection title="DEFAULT FILLER" summary="Used when a block has no filler lists of its own">


        {entries.length > 1 && (
          <div style={{ marginBottom: 12 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>SELECT BY</div>
            <select value={selectionMode} onChange={e => store.saveChannelFiller(channelId, { default_filler_selection: e.target.value as FillerSelectionMode })} style={inputStyle}>
              {FILLER_SEL_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
          </div>
        )}

        <div style={{ display: 'flex', flexDirection: 'column', gap: 7, marginBottom: 10 }}>
          {entries.map(entry => (
            <FillerEntryRow
              key={entry.id}
              entry={entry}
              showWeight={showWeight}
              onAdvancement={adv => store.updateChannelFiller(channelId, entry.id, { advancement: adv })}
              onWeight={w   => store.updateChannelFiller(channelId, entry.id, { weight: w })}
              onRemove={()  => store.removeChannelFiller(channelId, entry.id)}
            />
          ))}
        </div>

        {entries.length === 0 && !addOpen && (
          <div style={{ textAlign: 'center', padding: '10px 6px', color: 'var(--hds-txt-3)', fontSize: 11 }}>
            No default filler lists configured
          </div>
        )}

        <button
          onClick={() => setAddOpen(o => !o)}
          style={{ padding: '6px 12px', border: '1px solid var(--hds-line)', borderRadius: 7, background: 'transparent', color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer', marginBottom: addOpen ? 8 : 0 }}
        >
          {addOpen ? '✕ Cancel' : '+ Add filler list'}
        </button>

        {addOpen && (
          <div style={{ padding: '11px 12px', background: 'oklch(0.16 0.016 286)', border: '1px solid var(--hds-line)', borderRadius: 9 }}>
            <div style={{ display: 'flex', gap: 7, alignItems: 'center', flexWrap: 'wrap' }}>
              <select value={addListId} onChange={e => setAddListId(e.target.value)} style={{ ...filterInputStyle, flex: '1 1 140px' }}>
                <option value="">Select filler list…</option>
                {store.allFillerLists.map(f => <option key={f.filler_list_id} value={f.filler_list_id}>{f.title}</option>)}
              </select>
              <select value={addAdv} onChange={e => setAddAdv(e.target.value as FillerEntryAdvancement)} style={{ ...filterInputStyle, width: 96 }}>
                {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
              </select>
              {showWeight && (
                <input type="number" min={1} value={addWeight} onChange={e => setAddWeight(Math.max(1, +e.target.value || 1))} style={{ ...filterInputStyle, width: 48 }} placeholder="Wt" />
              )}
              <button
                onClick={() => { if (addListId) { store.addChannelFiller(channelId, { filler_list_id: addListId, advancement: addAdv, weight: addWeight }); setAddListId(''); setAddOpen(false) } }}
                disabled={!addListId}
                style={{ padding: '5px 12px', border: 'none', borderRadius: 6, background: 'var(--hds-violet)', color: 'oklch(0.15 0.02 286)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700, cursor: addListId ? 'pointer' : 'default', opacity: addListId ? 1 : 0.4 }}
              >
                Add
              </button>
            </div>
          </div>
        )}

        {store.channelFillerErr && (
          <div style={{ marginTop: 8, fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{store.channelFillerErr}</div>
        )}
        </CardSection>

        <CardSection title="CHANNEL BUMPERS" summary="Between / filler mode injection">


        <div style={{ display: 'flex', flexDirection: 'column', gap: 7, marginBottom: 10 }}>
          {bumpers.map(b => (
            <div key={b.id} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '7px 10px', background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line)', borderRadius: 7 }}>
              <span style={{ fontSize: 9.5, padding: '2px 6px', borderRadius: 3, background: 'var(--hds-bg)', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", letterSpacing: '0.06em', flexShrink: 0 }}>{b.content_type}</span>
              <span style={{ flex: 1, fontSize: 11, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap', color: 'var(--hds-txt-2)' }}>{b.content_id}</span>
              <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', flexShrink: 0 }}>{b.mode} / {b.every_n}</span>
              <button onClick={() => deleteBumper(b.id)} style={{ background: 'transparent', border: 'none', cursor: 'pointer', color: 'var(--hds-txt-3)', fontSize: 12, padding: '0 2px', flexShrink: 0 }}>✕</button>
            </div>
          ))}
        </div>

        {bumpers.length === 0 && !addBumpOpen && (
          <div style={{ textAlign: 'center', padding: '8px 6px', color: 'var(--hds-txt-3)', fontSize: 11, marginBottom: 8 }}>No channel bumpers configured</div>
        )}

        <button onClick={() => setAddBumpOpen(o => !o)} style={{ padding: '6px 12px', border: '1px solid var(--hds-line)', borderRadius: 7, background: 'transparent', color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer', marginBottom: addBumpOpen ? 10 : 0 }}>
          {addBumpOpen ? '✕ Cancel' : '+ Add bumper'}
        </button>

        {addBumpOpen && (
          <div style={{ padding: '11px 12px', background: 'oklch(0.16 0.016 286)', border: '1px solid var(--hds-line)', borderRadius: 9 }}>
            <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap', marginBottom: 8 }}>
              <select value={bCt} onChange={e => setBCt(e.target.value as BumperContentType)} style={{ ...filterInputStyle, width: 100 }}>
                <option value="show">Show</option>
                <option value="playlist">Playlist</option>
                <option value="episode">Episode</option>
              </select>
              <select value={bMode} onChange={e => setBMode(e.target.value as BumperMode)} style={{ ...filterInputStyle, width: 110 }}>
                <option value="between">Between</option>
                <option value="filler">Filler</option>
              </select>
              <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
                <span style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>Every</span>
                <input type="number" min={1} value={bN} onChange={e => setBN(Math.max(1, +e.target.value || 1))} style={{ ...filterInputStyle, width: 48 }} />
                <span style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>progs</span>
              </div>
            </div>
            <BumperContentPicker contentType={bCt} onPick={cid => setBCid(cid)} currentId={bCid} />
            {bCid && (
              <div style={{ marginTop: 8, display: 'flex', alignItems: 'center', gap: 8 }}>
                <span style={{ fontSize: 11, color: 'var(--hds-txt-2)', flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{bCid}</span>
                <button onClick={() => setBCid('')} style={{ background: 'transparent', border: 'none', cursor: 'pointer', color: 'var(--hds-txt-3)', fontSize: 11 }}>✕</button>
              </div>
            )}
            <button onClick={addBumper} disabled={!bCid} style={{ marginTop: 10, padding: '6px 14px', border: 'none', borderRadius: 6, background: 'var(--hds-violet)', color: 'oklch(0.15 0.02 286)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700, cursor: bCid ? 'pointer' : 'default', opacity: bCid ? 1 : 0.4 }}>
              Add
            </button>
          </div>
        )}

        {bumperErr && (
          <div style={{ marginTop: 8, fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{bumperErr}</div>
        )}
        </CardSection>

        <CardSection title="OFFLINE FALLBACK" summary="Served when no content is scheduled">


        <div style={{ marginBottom: 10 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>VIDEO (looping)</div>
          <input
            value={store.channelDraftOfflineVideoPath}
            onChange={e => store.setChannelDraft({ offline_video_path: e.target.value })}
            style={inputStyle}
            placeholder="/path/to/offline.mp4"
            spellCheck={false}
          />
        </div>

        <div style={{ marginBottom: 10 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>IMAGE</div>
          <input
            value={store.channelDraftOfflineImagePath}
            onChange={e => store.setChannelDraft({ offline_image_path: e.target.value })}
            style={inputStyle}
            placeholder="/path/to/offline.png or https://…"
            spellCheck={false}
          />
        </div>

        <div style={{ marginBottom: 10 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>AUDIO (with image)</div>
          <AudioPicker
            audioId={store.channelDraftOfflineAudioId}
            audioTitle={store.channelDraftOfflineAudioTitle}
            onSelect={(id, type, title) => store.setChannelDraft({ offline_audio_id: id, offline_audio_type: type, offline_audio_title: title })}
            onClear={() => store.setChannelDraft({ offline_audio_id: '', offline_audio_type: '', offline_audio_title: '' })}
          />
        </div>
        </CardSection>
      </div>
    </div>
  )
})

// ─── BumperContentPicker ─────────────────────────────────────────────────────

function BumperContentPicker({ contentType, onPick, currentId }: {
  contentType: BumperContentType
  onPick:      (id: string) => void
  currentId:   string
}) {
  const [q,         setQ]         = useState('')
  const [seasonFlt, setSeasonFlt] = useState('')
  const [shows,     setShows]     = useState<Show[]>([])
  const [lists,     setLists]     = useState<Playlist[]>([])
  const [eps,       setEps]       = useState<EpisodeSearchResult[]>([])
  const [open,      setOpen]      = useState(false)
  const debounceRef = useRef<ReturnType<typeof setTimeout>>()

  useEffect(() => {
    setQ(''); setSeasonFlt(''); setShows([]); setLists([]); setEps([]); setOpen(false)
  }, [contentType])

  function search(val: string, slt = seasonFlt) {
    clearTimeout(debounceRef.current)
    setQ(val)
    if (!val.trim() && contentType === 'show') { setOpen(false); return }
    debounceRef.current = setTimeout(async () => {
      try {
        if (contentType === 'show') {
          const r = await api.getShows({ limit: 40, q: val })
          setShows(r.items); setOpen(r.items.length > 0)
        } else if (contentType === 'playlist') {
          const r = await api.getPlaylists()
          const filtered = r.filter(p => !val || p.title.toLowerCase().includes(val.toLowerCase()))
          setLists(filtered); setOpen(filtered.length > 0)
        } else if (contentType === 'episode') {
          const season = slt.trim() !== '' ? parseInt(slt, 10) : undefined
          const r = await api.searchEpisodes({ q: val || undefined, season: Number.isFinite(season) ? season : undefined, limit: 40 })
          setEps(r.items); setOpen(r.items.length > 0)
        }
      } catch {}
    }, 200)
  }

  function searchSeason(slt: string) {
    setSeasonFlt(slt)
    search(q, slt)
  }

  useEffect(() => {
    if (contentType === 'playlist') search('')
    if (contentType === 'episode') search('')
  }, [contentType])

  return (
    <div style={{ position: 'relative' }}>
      <div style={{ display: 'flex', gap: 5 }}>
        <input
          value={q}
          onChange={e => search(e.target.value)}
          onFocus={() => { if ((shows.length > 0 || lists.length > 0 || eps.length > 0) && !currentId) setOpen(true) }}
          onBlur={() => setTimeout(() => setOpen(false), 150)}
          style={{ ...filterInputStyle, flex: 1 }}
          placeholder={`Search ${contentType}s…`}
          spellCheck={false}
        />
        {contentType === 'episode' && (
          <input
            type="number" min={0} value={seasonFlt}
            onChange={e => searchSeason(e.target.value)}
            onBlur={() => setTimeout(() => setOpen(false), 150)}
            placeholder="S#" title="Filter by season (0 = specials)"
            style={{ ...filterInputStyle, width: 48 }}
          />
        )}
      </div>
      {open && (
        <div style={{ position: 'absolute', top: '100%', left: 0, right: 0, zIndex: 50, background: 'var(--hds-bg-2)', border: '1px solid var(--hds-line)', borderRadius: 7, marginTop: 3, maxHeight: 180, overflow: 'auto' }} className="scrollbar-dark">
          {contentType === 'show' && shows.map(s => (
            <div key={s.show_id} onMouseDown={() => { onPick(s.show_id); setQ(s.title); setOpen(false) }} style={{ padding: '6px 10px', fontSize: 11, cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)' }}
              onMouseEnter={e => (e.currentTarget.style.background = 'var(--hds-bg-3)')}
              onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}>
              {s.title}{s.year ? <span style={{ color: 'var(--hds-txt-3)', fontSize: 9.5, marginLeft: 5 }}>({s.year})</span> : null}
            </div>
          ))}
          {contentType === 'playlist' && lists.map(p => (
            <div key={p.playlist_id} onMouseDown={() => { onPick(p.playlist_id); setQ(p.title); setOpen(false) }} style={{ padding: '6px 10px', fontSize: 11, cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)' }}
              onMouseEnter={e => (e.currentTarget.style.background = 'var(--hds-bg-3)')}
              onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}>
              {p.title}
            </div>
          ))}
          {contentType === 'episode' && eps.map(ep => (
            <div key={ep.episode_id} onMouseDown={() => { onPick(ep.episode_id); setOpen(false) }} style={{ padding: '6px 10px', fontSize: 11, cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)' }}
              onMouseEnter={e => (e.currentTarget.style.background = 'var(--hds-bg-3)')}
              onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}>
              <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)' }}>{ep.show_title} · </span>
              S{String(ep.season).padStart(2,'0')}E{String(ep.episode).padStart(2,'0')} — {ep.title}
            </div>
          ))}
        </div>
      )}
    </div>
  )
}

type AudioPickerResult = { id: string; type: 'episode' | 'movie'; title: string }

const AudioPicker = observer(function AudioPicker({ audioId, audioTitle, onSelect, onClear }: {
  audioId:    string
  audioTitle: string
  onSelect:   (id: string, type: 'episode' | 'movie', title: string) => void
  onClear:    () => void
}) {
  const [query,   setQuery]   = useState('')
  const [results, setResults] = useState<AudioPickerResult[]>([])
  const [open,    setOpen]    = useState(false)
  const debounceRef = useRef<ReturnType<typeof setTimeout>>()

  function search(q: string) {
    clearTimeout(debounceRef.current)
    setQuery(q)
    if (!q.trim()) { setResults([]); setOpen(false); return }
    debounceRef.current = setTimeout(async () => {
      try {
        const [eps, movies] = await Promise.all([
          api.searchEpisodes({ q, limit: 10 }),
          api.getMovies({ q, limit: 10 }),
        ])
        const out: AudioPickerResult[] = [
          ...eps.items.map(e => ({ id: e.episode_id, type: 'episode' as const, title: `${e.show_title} — ${e.title}` })),
          ...movies.items.map(m => ({ id: m.movie_id, type: 'movie' as const, title: m.title })),
        ]
        setResults(out)
        setOpen(out.length > 0)
      } catch {}
    }, 250)
  }

  function pick(r: AudioPickerResult) {
    onSelect(r.id, r.type, r.title)
    setQuery('')
    setResults([])
    setOpen(false)
  }

  return (
    <div style={{ position: 'relative' }}>
      {audioId && (
        <div style={{ display: 'flex', alignItems: 'center', gap: 6, marginBottom: 5 }}>
          <span style={{ fontSize: 11, color: 'var(--hds-txt-1)', flex: 1, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
            {audioTitle || audioId}
          </span>
          <button onClick={onClear} style={{ padding: '2px 7px', border: '1px solid var(--hds-line)', borderRadius: 5, background: 'transparent', color: 'var(--hds-txt-3)', fontSize: 10, cursor: 'pointer' }}>✕</button>
        </div>
      )}
      <input
        value={query}
        onChange={e => search(e.target.value)}
        onBlur={() => setTimeout(() => setOpen(false), 150)}
        onFocus={() => results.length > 0 && setOpen(true)}
        style={filterInputStyle}
        placeholder="Search episodes or movies…"
        spellCheck={false}
      />
      {open && (
        <div style={{ position: 'absolute', top: '100%', left: 0, right: 0, zIndex: 50, background: 'var(--hds-bg-2)', border: '1px solid var(--hds-line)', borderRadius: 7, marginTop: 3, maxHeight: 200, overflow: 'auto' }} className="scrollbar-dark">
          {results.map(r => (
            <div
              key={r.id}
              onMouseDown={() => pick(r)}
              style={{ padding: '6px 10px', fontSize: 11, cursor: 'pointer', borderBottom: '1px solid var(--hds-line-s)' }}
              onMouseEnter={e => (e.currentTarget.style.background = 'var(--hds-bg-3)')}
              onMouseLeave={e => (e.currentTarget.style.background = 'transparent')}
            >
              <span style={{ color: 'var(--hds-txt-3)', fontSize: 9.5, marginRight: 5 }}>{r.type}</span>
              {r.title}
            </div>
          ))}
        </div>
      )}
    </div>
  )
})

export default ChannelDefaultsPanel
