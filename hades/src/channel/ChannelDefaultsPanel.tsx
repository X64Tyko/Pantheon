import { useState, useRef, useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import type { AdvanceMode, Channel, EpisodeSearchResult } from '../api/types'
import { CardSection, LauncherRow } from './sections'
import {TIMEZONE_SUGGESTIONS} from './constants'
import type { ChannelDetailStore } from './store'
import { api } from '../api/client'
import { CHANNEL_RATINGS } from '../api/ratingScales'
import { LangSelect } from '../components/media/LangSelect'
import shared from './sharedStyles.module.css'
import styles from './ChannelDefaultsPanel.module.css'

const ChannelDefaultsPanel = observer(function ChannelDefaultsPanel({ channel, channelId, store }: {
  channel:   Channel | undefined
  channelId: string
  store:     ChannelDetailStore
}) {
  const entries        = channel?.default_filler_entries ?? []
  const selectionMode  = channel?.default_filler_selection ?? 'round_robin'

  const [mediaLangs, setMediaLangs] = useState<{ audio: string[], subtitle: string[] } | null>(null)

  useEffect(() => {
    api.getMediaLanguages().then(setMediaLangs).catch(() => {})
  }, [])

  return (
    <div className={styles.panelRoot}>
      <div className={styles.panelHeader}>
        <span className={styles.panelTitle}>Channel Settings</span>
      </div>

      <div className={`${styles.panelBody} scrollbar-dark`}>
        <div className={styles.introHelp}>
          Select a block to edit it, or press <span className={styles.goldInline}>Add Block</span> to create one.
        </div>

        <CardSection title="CHANNEL">
        <div className={styles.fieldRow}>
          <div className={styles.fieldCol}>
            <div className={styles.fieldLabel}>NAME</div>
            <input value={store.channelDraft.name} onChange={e => store.setChannelDraft({ name: e.target.value })} className={shared.input} />
          </div>
          <div className={styles.fieldColNarrow}>
            <div className={styles.fieldLabel}>CH #</div>
            <input type="number" min={1} value={store.channelDraft.number} onChange={e => store.setChannelDraft({ number: Math.max(1, +e.target.value || 1) })} className={shared.input} />
          </div>
        </div>
        <div className={styles.fieldGroup8}>
          <div className={styles.fieldLabel}>TIMEZONE</div>
          <input
            list="tz-suggestions"
            value={store.channelDraft.timezone}
            onChange={e => store.setChannelDraft({ timezone: e.target.value })}
            className={shared.input}
            placeholder="America/Denver"
            spellCheck={false}
          />
          <datalist id="tz-suggestions">
              {TIMEZONE_SUGGESTIONS.map(tz => <option key={tz} value={tz}/>)}
          </datalist>
          <div className={styles.fieldHint}>
            IANA format — e.g. <span className={styles.fieldHintMono}>America/Denver</span>
          </div>
        </div>
        <div className={styles.fieldGroup8}>
          <div className={styles.fieldLabel}>EPG SEED</div>
          <div className={styles.seedRow}>
            <input type="number" min={0} value={store.channelDraft.seed} onChange={e => store.setChannelDraft({ seed: Math.max(0, +e.target.value || 0) })} className={`${shared.input} ${styles.seedInputFlex}`} />
            <button
              onClick={() => store.setChannelDraft({ seed: Math.floor(Math.random() * 99999) + 1 })}
              title="Randomize seed"
              className={styles.seedRandomBtn}
            >⚄</button>
          </div>
          <div className={`${styles.fieldHint} ${styles.fieldHintTight}`}>
            Controls the starting position for EPG simulation when no live schedule exists. Change to get a different ordering.
          </div>
        </div>
        <div className={styles.fieldGroup8}>
          <div className={styles.fieldLabel}>PRE-SEED WEEKS</div>
          <input
            type="number" min={0} max={52}
            value={store.channelDraft.pre_seed_weeks}
            onChange={e => store.setChannelDraft({ pre_seed_weeks: Math.max(0, Math.min(52, +e.target.value || 0)) })}
            className={shared.input}
            placeholder="0 = disabled"
          />
          <div className={`${styles.fieldHint} ${styles.fieldHintTight}`}>
            Number of weeks to simulate in the past at launch so rerun shows start mid-rotation instead of all at episode 1. Applied automatically on new channels; use <span className={styles.goldInline}>Apply Pre-seed</span> to re-run on this channel (resets all cursor positions).
          </div>
          {channel && store.channelDraft.pre_seed_weeks > 0 && (
            <div className={styles.preSeedRow}>
              <button
                onClick={() => store.applyPreSeed(channelId, store.channelDraft.pre_seed_weeks)}
                disabled={store.preSeedApplying}
                className={styles.preSeedBtn}
                title="Hard-reset cursor positions and re-run the virtual pre-seed projection"
              >
                {store.preSeedApplying ? 'Applying…' : 'Apply Pre-seed'}
              </button>
              {store.preSeedErr && <span className={styles.preSeedErr}>{store.preSeedErr}</span>}
            </div>
          )}
        </div>
        <div className={styles.fieldGroup14}>
          <div className={styles.fieldLabel}>CONTENT TAG</div>
          <select
            value={store.channelDraft.content_tag}
            onChange={e => store.setChannelDraft({ content_tag: e.target.value })}
            className={shared.input}
          >
            <option value="">Unrated</option>
            {CHANNEL_RATINGS.map(r => <option key={r} value={r}>{r}</option>)}
          </select>
          <div className={`${styles.fieldHint} ${styles.fieldHintTight}`}>
            Ceiling for this channel's own content, evaluated the same way as a show's rating. Restricted accounts can't see or play this channel above their max channel rating — "Unrated" fails closed.
          </div>
        </div>

        {store.channelSaveErr && (
          <div className={styles.errorBanner}>
            {store.channelSaveErr}
          </div>
        )}

        <button
          onClick={() => store.saveChannel(channelId)}
          disabled={store.channelSaving || !store.isDirty}
          className={[
            styles.saveBtn,
            store.isDirty ? styles.saveBtnDirty : '',
            store.channelSaving ? styles.saveBtnSaving : '',
          ].filter(Boolean).join(' ')}
        >
          {store.channelSaving ? 'Saving…' : 'Save Channel'}
        </button>
        </CardSection>

        <CardSection title="LOGO">
        <div className={styles.fieldGroup8}>
          <div className={styles.fieldLabel}>LOGO PATH</div>
          <input
            value={store.channelDraft.logo_path}
            onChange={e => store.setChannelDraft({ logo_path: e.target.value })}
            className={shared.input}
            placeholder="/path/to/logo.png or https://…"
            spellCheck={false}
          />
          <div className={styles.fieldHint}>
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

        <CardSection title="STREAMING" summary="Hephaestus audio and subtitle preferences">
        <div className={styles.fieldGroup8}>
          <div className={styles.fieldLabel}>AUDIO LANGUAGE</div>
          <LangSelect
            value={store.channelDraft.audio_lang}
            onChange={v => store.setChannelDraft({ audio_lang: v })}
            langs={mediaLangs?.audio ?? []}
            emptyLabel="Global default"
            className={shared.input}
          />
          <div className={styles.fieldHint}>
            Overrides the server-wide default for this channel only.
          </div>
        </div>
        <div className={styles.fieldGroup10}>
          <div className={styles.fieldLabel}>SUBTITLE LANGUAGE</div>
          <LangSelect
            value={store.channelDraft.subtitle_lang}
            onChange={v => store.setChannelDraft({ subtitle_lang: v })}
            langs={mediaLangs?.subtitle ?? []}
            emptyLabel="Disabled (no subtitles)"
            className={shared.input}
          />
          <div className={styles.fieldHint}>
            When set, Hephaestus maps a matching subtitle track into the stream.
          </div>
        </div>
        </CardSection>

        <CardSection title="TRANSCODE QUALITY" summary="Per-channel resolution and bitrate limits">
            <div className={styles.fieldGroup10}>
                <label className={styles.checkboxLabel}>
                    <input type="checkbox"
                           checked={store.channelDraft.force_transcode ?? false}
                           onChange={e => store.setChannelDraft({force_transcode: e.target.checked})}/>
                    <span className={styles.checkboxLabelText}>Always transcode (disable direct stream)</span>
                </label>
                <div className={styles.fieldHint}>
                    Direct-stream viewers skip re-encoding video, but can only correct schedule drift by skipping ahead
                    in small steps rather than smoothly. Enable this to force every viewer through the transcode path
                    instead, trading away direct-stream's CPU/GPU savings for smooth, continuous drift correction.
                </div>
            </div>
        <div className={styles.fieldGroup8}>
          <div className={styles.fieldLabel}>MAX RESOLUTION</div>
          <select
            value={store.channelDraft.stream_resolution ?? 'source'}
            onChange={e => store.setChannelDraft({ stream_resolution: e.target.value as 'source' | '1080p' | '720p' | '480p' })}
            className={`${shared.input} ${styles.selectPointer}`}
          >
            <option value="source">Source (no scaling)</option>
            <option value="1080p">1080p</option>
            <option value="720p">720p</option>
            <option value="480p">480p</option>
          </select>
          <div className={styles.fieldHint}>
            Scales down only — never upscales. Useful for SD-only channels.
          </div>
        </div>
        <div className={styles.fieldGroup8}>
          <div className={styles.fieldLabel}>VIDEO BITRATE CAP (kbps)</div>
          <input
            type="number"
            min={0}
            step={100}
            value={store.channelDraft.stream_video_bitrate ?? 0}
            onChange={e => store.setChannelDraft({ stream_video_bitrate: Math.max(0, +e.target.value || 0) })}
            className={shared.input}
            placeholder="0 = CRF auto (no cap)"
          />
          <div className={styles.fieldHint}>
            0 = quality-based (CRF). Set a value to cap bandwidth, e.g. 3000 for ~3 Mbps.
          </div>
        </div>
        <div className={styles.fieldGroup10}>
          <div className={styles.fieldLabel}>AUDIO BITRATE (kbps)</div>
          <input
            type="number"
            min={64}
            max={320}
            step={32}
            value={store.channelDraft.stream_audio_bitrate ?? 192}
            onChange={e => store.setChannelDraft({ stream_audio_bitrate: Math.max(64, Math.min(320, +e.target.value || 192)) })}
            className={shared.input}
          />
        </div>
        </CardSection>

        <CardSection title="OFFLINE FALLBACK" summary="Served when no content is scheduled">
        <div className={styles.fieldGroup10}>
          <div className={styles.fieldLabel}>VIDEO (looping)</div>
          <input
            value={store.channelDraft.offline_video_path}
            onChange={e => store.setChannelDraft({ offline_video_path: e.target.value })}
            className={shared.input}
            placeholder="/path/to/offline.mp4"
            spellCheck={false}
          />
        </div>

        <div className={styles.fieldGroup10}>
          <div className={styles.fieldLabel}>IMAGE</div>
          <input
            value={store.channelDraft.offline_image_path}
            onChange={e => store.setChannelDraft({ offline_image_path: e.target.value })}
            className={shared.input}
            placeholder="/path/to/offline.png or https://…"
            spellCheck={false}
          />
        </div>

        <div className={styles.fieldGroup10}>
          <div className={styles.fieldLabel}>AUDIO (with image)</div>
          <AudioPicker
            audioId={store.channelDraft.offline_audio_id}
            audioTitle={store.channelDraft.offline_audio_title}
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
    <div className={styles.audioPickerRoot}>
      {audioId && (
        <div className={styles.audioSelectedRow}>
          <span className={styles.audioSelectedText}>
            {audioTitle || audioId}
          </span>
          <button onClick={onClear} className={styles.audioClearBtn}>✕</button>
        </div>
      )}
      <input
        value={query}
        onChange={e => search(e.target.value)}
        onBlur={() => setTimeout(() => setOpen(false), 150)}
        onFocus={() => results.length > 0 && setOpen(true)}
        className={shared.filterInput}
        placeholder="Search episodes or movies…"
        spellCheck={false}
      />
      {open && (
        <div className={`${styles.audioResultsDropdown} scrollbar-dark`}>
          {results.map(r => (
            <div
              key={r.id}
              onMouseDown={() => pick(r)}
              className={styles.audioResultItem}
            >
              <span className={styles.audioResultType}>{r.type}</span>
              {r.title}
            </div>
          ))}
        </div>
      )}
    </div>
  )
})

export default ChannelDefaultsPanel
