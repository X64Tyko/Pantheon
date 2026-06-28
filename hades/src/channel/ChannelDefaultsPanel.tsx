import { useState, useRef, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import type { AdvanceMode, Channel, EpisodeSearchResult } from '../api/types'
import { inputStyle, filterInputStyle } from './styles'
import { CardSection, LauncherRow } from './sections'
import type { ChannelDetailStore } from './store'
import { api } from '../api/client'

const ChannelDefaultsPanel = observer(function ChannelDefaultsPanel({ channel, channelId, store }: {
  channel:   Channel | undefined
  channelId: string
  store:     ChannelDetailStore
}) {
  const entries        = channel?.default_filler_entries ?? []
  const selectionMode  = channel?.default_filler_selection ?? 'round_robin'

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
        <LauncherRow
          icon="◈"
          title="MANAGE FILLER"
          summary={entries.length > 0 ? `${entries.length} source${entries.length !== 1 ? 's' : ''} · ${selectionMode}` : 'No default filler configured'}
          onClick={() => { store.channelFillerOverlayOpen = true }}
        />
        </CardSection>

        <CardSection title="CHANNEL BUMPERS" summary="Between / filler mode injection">
        <LauncherRow
          icon="⊕"
          title="MANAGE BUMPERS"
          summary="Configure bumper content injected during playback"
          onClick={() => { store.channelBumperOverlayOpen = true }}
        />
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

// ─── Audio picker ─────────────────────────────────────────────────────────────

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
