import { useEffect, useState } from 'react'
import { api } from '../api/client'
import { useAuth } from '../auth/AuthContext'
import type { MediaLanguages } from '../api/types'
import { LangSelect } from '../components/media/LangSelect'
import settingsStyles from './SettingsPage.module.css'

// Self-service, reachable by any logged-in user (unlike SettingsPage, which
// is admin-only — see App.tsx's <AdminRoute /> wrapper) — the whole reason
// this page exists rather than folding "my defaults" into Settings. Reuses
// SettingsPage's own page-shell classes (.page/.section/.settingRow/...)
// rather than duplicating that CSS for a one-section page.
export default function AccountPage() {
    const {user, updateTrackPreference, updateDefaultLandingPage} = useAuth()
  const [mediaLangs, setMediaLangs] = useState<MediaLanguages | null>(null)
  const [audioLang,    setAudioLang]    = useState(user?.default_audio_lang ?? '')
  const [subtitleLang, setSubtitleLang] = useState(user?.default_subtitle_lang ?? '')
    const [landingPage, setLandingPage] = useState(user?.default_landing_page ?? '')
  const [error, setError] = useState<string | null>(null)

  useEffect(() => {
    api.getMediaLanguages().then(setMediaLangs).catch(() => {})
  }, [])

  const save = async (b: { audio_lang?: string; subtitle_lang?: string }) => {
    setError(null)
    try {
      await updateTrackPreference(b)
    } catch (e: any) {
      setError(e.message ?? 'Failed to save')
    }
  }

  if (!user) return null

  return (
    <div className={settingsStyles.page}>
      <div className={settingsStyles.titleRow}>
        <h1 className={settingsStyles.title}>My Account</h1>
      </div>

      {error && <div className={settingsStyles.errorBanner}>{error}</div>}

      <div className={settingsStyles.section}>
        <div className={settingsStyles.sectionHeader}>
          <span className={settingsStyles.sectionHeaderText}>ACCOUNT</span>
        </div>
        <div className={settingsStyles.sectionBody}>
          <div className={settingsStyles.settingRow}>
            <div>
              <div className={settingsStyles.settingRowLabel}>Username</div>
            </div>
            <div className={settingsStyles.settingRowControl}>{user.username}</div>
          </div>
        </div>
      </div>

      <div className={settingsStyles.section}>
          <div className={settingsStyles.sectionHeader}>
              <span className={settingsStyles.sectionHeaderText}>GENERAL</span>
          </div>
          <div className={settingsStyles.sectionBody}>
              <div className={settingsStyles.settingRow}>
                  <div>
                      <div className={settingsStyles.settingRowLabel}>Default Landing Page</div>
                      <div className={settingsStyles.settingRowHint}>
                          Which page you land on after logging in or switching profiles.
                      </div>
                  </div>
                  <div className={settingsStyles.settingRowControl}>
                      <select
                          value={landingPage}
                          onChange={e => {
                              const v = e.target.value as '' | 'home' | 'guide'
                              setLandingPage(v)
                              updateDefaultLandingPage(v).catch(err => setError(err.message ?? 'Failed to save'))
                          }}
                          className={`${settingsStyles.input} ${settingsStyles.inputCursorPointer}`}
                      >
                          <option value="">Use server default</option>
                          <option value="home">Home</option>
                          <option value="guide">Guide</option>
                      </select>
                  </div>
              </div>
          </div>
      </div>

        <div className={settingsStyles.section}>
        <div className={settingsStyles.sectionHeader}>
          <span className={settingsStyles.sectionHeaderText}>PLAYBACK DEFAULTS</span>
        </div>
        <div className={settingsStyles.sectionBody}>
          <div className={settingsStyles.settingRow}>
            <div>
              <div className={settingsStyles.settingRowLabel}>Audio Language</div>
              <div className={settingsStyles.settingRowHint}>
                Used whenever a show or movie doesn't have its own audio preference set.
              </div>
            </div>
            <div className={settingsStyles.settingRowControl}>
              <LangSelect
                value={audioLang}
                onChange={v => { setAudioLang(v); save({ audio_lang: v }) }}
                langs={mediaLangs?.audio ?? []}
                emptyLabel="No preference"
                className={`${settingsStyles.input} ${settingsStyles.inputCursorPointer}`}
              />
            </div>
          </div>
          <div className={settingsStyles.settingRow}>
            <div>
              <div className={settingsStyles.settingRowLabel}>Subtitle Language</div>
              <div className={settingsStyles.settingRowHint}>
                Used whenever a show or movie doesn't have its own subtitle preference set.
              </div>
            </div>
            <div className={settingsStyles.settingRowControl}>
              <LangSelect
                value={subtitleLang}
                onChange={v => { setSubtitleLang(v); save({ subtitle_lang: v }) }}
                langs={mediaLangs?.subtitle ?? []}
                emptyLabel="No preference"
                className={`${settingsStyles.input} ${settingsStyles.inputCursorPointer}`}
              />
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
