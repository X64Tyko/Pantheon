import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import type { MediaLanguages } from '../../api/types'
import { LangSelect } from './LangSelect'
import styles from './PlaybackPreferenceSelector.module.css'

// Sticky per-show/movie audio/subtitle language, settable here — from the
// detail page, before ever pressing play — as well as by switching tracks
// mid-playback (PlayerPage.tsx's saveTrackPreference, which writes the same
// underlying row). GET /api/playback/:content_type/:id falls back to the
// viewer's own library-wide default (see Settings) whenever neither side is
// set here. Auto-saves on change — two fields don't need a separate Save
// button, and the whole point is "set it and go press play."
export function PlaybackPreferenceSelector({ contentType, id, languages }: {
  contentType: 'show' | 'movie'
  id:          string
  languages:   MediaLanguages | null
}) {
  const [audioLang,    setAudioLang]    = useState('')
  const [subtitleLang, setSubtitleLang] = useState('')
  const [loaded,  setLoaded]  = useState(false)
  const [error,   setError]   = useState<string | null>(null)

  useEffect(() => {
    setLoaded(false)
    setError(null)
    const get = contentType === 'show' ? api.getShowTrackPreference : api.getMovieTrackPreference
    get(id)
      .then(pref => { setAudioLang(pref.audio_lang); setSubtitleLang(pref.subtitle_lang) })
      .catch(() => {})
      .finally(() => setLoaded(true))
  }, [contentType, id])

  const save = (b: { audio_lang?: string; subtitle_lang?: string }) => {
    setError(null)
    const set = contentType === 'show' ? api.setShowTrackPreference : api.setMovieTrackPreference
    set(id, b).catch(e => setError(e instanceof Error ? e.message : 'Failed to save'))
  }

  if (!loaded) return null

  return (
    <div className={styles.root}>
      <div className={styles.field}>
        <span className={styles.label}>Audio</span>
        <LangSelect
          value={audioLang}
          onChange={v => { setAudioLang(v); save({ audio_lang: v }) }}
          langs={languages?.audio ?? []}
          emptyLabel="No preference"
          className={styles.select}
        />
      </div>
      <div className={styles.field}>
        <span className={styles.label}>Subtitles</span>
        <LangSelect
          value={subtitleLang}
          onChange={v => { setSubtitleLang(v); save({ subtitle_lang: v }) }}
          langs={languages?.subtitle ?? []}
          emptyLabel="No preference"
          className={styles.select}
        />
      </div>
      {error && <span className={styles.error}>{error}</span>}
    </div>
  )
}
