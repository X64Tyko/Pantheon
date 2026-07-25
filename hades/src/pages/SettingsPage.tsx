import { useEffect, useRef, useState } from 'react'
import { observer } from 'mobx-react-lite'
import { useSearchParams } from 'react-router-dom'
import { api, downloadDebugDump } from '../api/client'
import { statusStore, helpTipsStore } from '../stores'
import { tourStore } from '../stores/TourStore'
import { TourSpotlight } from '../components/tour/TourSpotlight'
import type { ArrConfig, CastSessionInfo, RokuDevice, ScraperSettings, ScraperStats } from '../api/types'

interface SmtpForm {
  host:            string
  port:            string
  username:        string
  password:        string
  from_address:    string
  public_base_url: string
}
import { useFocusable } from '../nav/useFocusable'
import { HelpTip, HelpSection } from '../channel/HelpTip'
import styles from './SettingsPage.module.css'

interface Settings {
  epg_debug:              boolean
  sync_debug:             boolean
  sync_threads:           number
  stream_buffer_size:     number
  image_cache_ttl_hours:  number
  verbose_transcode_logs: boolean
  verbose_gateway_logs:   boolean
  hades_debug:            boolean
  cast_app_id:            string
    default_landing_page: string
    internal_token: string
}

type Tab = 'general' | 'scrapers' | 'integrations' | 'devices' | 'diagnostics'
const TABS: { key: Tab; label: string }[] = [
  { key: 'general',      label: 'General' },
  { key: 'scrapers',     label: 'Scrapers' },
  { key: 'integrations', label: 'Integrations' },
  { key: 'devices',      label: 'Devices' },
  { key: 'diagnostics',  label: 'Diagnostics' },
]

function Toggle({ id, checked, onChange, disabled }: { id: string; checked: boolean; onChange: (v: boolean) => void; disabled?: boolean }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({
    focusKey: `settings-toggle-${id}`,
    onEnterPress: () => onChange(!checked),
    focusable: !disabled,
  })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      role="switch"
      aria-checked={checked}
      disabled={disabled}
      onClick={() => onChange(!checked)}
      className={`${styles.toggle} ${checked ? styles.toggleOn : styles.toggleOff} ${disabled ? styles.toggleDisabled : ''}`}
    >
      <span className={`${styles.toggleKnob} ${checked ? styles.toggleKnobOn : styles.toggleKnobOff}`} />
    </button>
  )
}

function SettingRow({ label, hint, children }: { label: string; hint?: string; children: React.ReactNode }) {
  return (
    // flexWrap so the label/hint stack above the control on narrow screens
    // instead of both fighting for horizontal space — several controls here
    // have hardcoded widths (e.g. 260px API key inputs) that would otherwise
    // crush the label into a sliver, or overflow outright, on a phone.
    <div className={styles.settingRow}>
      <div>
        <div className={styles.settingRowLabel}>{label}</div>
        {hint && <div className={styles.settingRowHint}>{hint}</div>}
      </div>
      <div className={styles.settingRowControl}>{children}</div>
    </div>
  )
}

// Local helper — this page has several one-off action buttons (Clear All,
// Save x2, Run Match, Reset/Yes/Cancel) all needing the same D-pad wiring;
// factored out rather than repeating the useFocusable boilerplate 7 times.
function NavButton({ id, onClick, disabled, className, title, children }: {
  id: string; onClick: () => void; disabled?: boolean; title?: string
  className: string; children: React.ReactNode
}) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: `settings-btn-${id}`, onEnterPress: onClick, focusable: !disabled })
  return (
    <button ref={ref} data-tv-focused={focused} onClick={onClick} disabled={disabled} title={title} className={className}>
      {children}
    </button>
  )
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className={styles.section}>
      <div className={styles.sectionHeader}>
        <span className={styles.sectionHeaderText}>{title.toUpperCase()}</span>
      </div>
      <div className={styles.sectionBody}>{children}</div>
    </div>
  )
}

export default observer(function SettingsPage() {
  const [searchParams] = useSearchParams()
  const initialTab = TABS.find(t => t.key === searchParams.get('tab'))?.key ?? 'general'
  const [tab, setTab] = useState<Tab>(initialTab)

  const [settings, setSettings]   = useState<Settings | null>(null)
  const [saving,   setSaving]     = useState(false)
  const [clearing, setClearing]   = useState(false)
  const [clearMsg, setClearMsg]   = useState<string | null>(null)
  const [error,    setError]      = useState<string | null>(null)
  const [threads,  setThreads]    = useState('')
  const [bufferSize, setBufferSize] = useState('')
  const [castAppId, setCastAppId] = useState('')
    const [internalToken, setInternalToken] = useState('')
    const [regeneratingToken, setRegeneratingToken] = useState(false)
  const [castSessions, setCastSessions] = useState<CastSessionInfo[] | null>(null)
  const [revokingCast, setRevokingCast] = useState<string | null>(null)

  const [rokuDevices, setRokuDevices]   = useState<RokuDevice[] | null>(null)
  const [rokuName,    setRokuName]      = useState('')
  const [rokuIp,       setRokuIp]        = useState('')
  const [addingRoku,  setAddingRoku]    = useState(false)
  const [rokuError,   setRokuError]     = useState<string | null>(null)
  const [removingRoku, setRemovingRoku] = useState<string | null>(null)
  const rokuPollRef = useRef<ReturnType<typeof setTimeout> | null>(null)


  const [resetConfirm,  setResetConfirm]  = useState(false)
  const [resetting,     setResetting]     = useState(false)
  const [resetMsg,      setResetMsg]      = useState<string | null>(null)

  const [dumping, setDumping]   = useState(false)
  const [dumpMsg, setDumpMsg]   = useState<string | null>(null)

  const [arr,     setArr]     = useState<ArrConfig>({ sonarr_url: '', sonarr_api_key: '', radarr_url: '', radarr_api_key: '' })
  const [arrSave, setArrSave] = useState<'idle'|'saving'|'ok'|'err'>('idle')

  // Password starts blank regardless of whether one is already stored —
  // GET /api/config/smtp never returns it (same don't-leak-secrets shape as
  // source credentials elsewhere); has_password just tells the field what
  // "leave blank to keep" means here. A save only overwrites it when non-empty.
  const [smtp,            setSmtp]            = useState<SmtpForm>({ host: '', port: '587', username: '', password: '', from_address: '', public_base_url: '' })
  const [smtpHasPassword, setSmtpHasPassword] = useState(false)
  const [smtpSave,        setSmtpSave]        = useState<'idle'|'saving'|'ok'|'err'>('idle')
  const [testEmailTo,     setTestEmailTo]     = useState('')
  const [testSend,        setTestSend]        = useState<'idle'|'sending'|'ok'|'err'>('idle')
  const [testError,       setTestError]       = useState('')

  const [scraperSettings, setScraperSettings] = useState<ScraperSettings | null>(null)
  const [scraperStats,    setScraperStats]    = useState<ScraperStats | null>(null)
  const [scraperDirty,    setScraperDirty]    = useState(false)
  const [scraperSaving,   setScraperSaving]   = useState(false)
  const [scraperSaved,    setScraperSaved]    = useState(false)
  const [matchRunning,    setMatchRunning]    = useState(false)
  const matchPollRef = useRef<ReturnType<typeof setTimeout> | null>(null)

  const [refreshingAll, setRefreshingAll] = useState(false)
  const [refreshAllProgress, setRefreshAllProgress] = useState<{ total: number; processed: number; refreshed: number; failed: number } | null>(null)
  const refreshAllPollRef = useRef<ReturnType<typeof setTimeout> | null>(null)

  const [confirmAllPending, setConfirmAllPending] = useState(false)
  const [confirmingAll,     setConfirmingAll]     = useState(false)
  const [confirmAllMsg,     setConfirmAllMsg]     = useState<string | null>(null)

  const loadCastSessions = () => api.getCastSessions().then(setCastSessions).catch(() => setCastSessions([]))

  // Self-terminating poll chain, not a standing interval — only ever ticks
  // while a match pass is actually running, instead of hitting the server
  // every 3s for the entire time Settings happens to be open (which is what
  // was flooding the log with requests whether or not anything was going on).
  const wasMatchRunningRef = useRef(false)
  const pollMatchStatus = () => {
    api.getMatchStatus().then(s => {
      setMatchRunning(s.running)
      if (s.running) {
        matchPollRef.current = setTimeout(pollMatchStatus, 3000)
      } else if (wasMatchRunningRef.current) {
        api.getScraperStats().then(setScraperStats).catch(() => {})
      }
      wasMatchRunningRef.current = s.running
    }).catch(() => {})
  }

  // Same self-terminating pattern as pollMatchStatus, tracking the separate
  // "refresh all metadata" background job instead of the matcher.
  const wasRefreshingAllRef = useRef(false)
  const pollRefreshAllStatus = () => {
    api.getRefreshAllStatus().then(s => {
      setRefreshingAll(s.running)
      setRefreshAllProgress({ total: s.total, processed: s.processed, refreshed: s.refreshed, failed: s.failed })
      if (s.running) {
        refreshAllPollRef.current = setTimeout(pollRefreshAllStatus, 3000)
      } else if (wasRefreshingAllRef.current) {
        api.getScraperStats().then(setScraperStats).catch(() => {})
      }
      wasRefreshingAllRef.current = s.running
    }).catch(() => {})
  }

  const refreshAll = async () => {
    setRefreshingAll(true)
    setRefreshAllProgress(null)
    wasRefreshingAllRef.current = true
    await api.triggerRefreshAll()
    if (refreshAllPollRef.current) clearTimeout(refreshAllPollRef.current)
    pollRefreshAllStatus()
  }

  // Synchronous (a plain DB flip, no network fetch per item — see
  // ScraperManager::confirmAllMatches()), so unlike refreshAll/runMatch this
  // needs no background-job polling: the await just resolves once it's done.
  const confirmAllMatches = async () => {
    setConfirmingAll(true)
    setConfirmAllMsg(null)
    try {
      const { confirmed } = await api.confirmAllMatches()
      setConfirmAllMsg(`Confirmed ${confirmed} match${confirmed !== 1 ? 'es' : ''}.`)
      setConfirmAllPending(false)
      api.getScraperStats().then(setScraperStats).catch(() => {})
    } catch (e: any) {
      setConfirmAllMsg(`Error: ${e.message ?? 'Unknown error'}`)
    } finally {
      setConfirmingAll(false)
    }
  }

  const revokeCastSession = async (sessionId: string) => {
    setRevokingCast(sessionId)
    try { await api.revokeCastSession(sessionId); await loadCastSessions() }
    finally { setRevokingCast(null) }
  }

  // Self-terminating poll (same pattern as pollMatchStatus below) — only
  // ticks while at least one device is still mid-pairing (the channel
  // hasn't confirmed via its ECP-delivered pairingId/pairingToken yet).
  const pollRokuDevices = () => {
    api.getRokuDevices().then(devices => {
      setRokuDevices(devices)
      if (devices.some(d => !d.paired)) rokuPollRef.current = setTimeout(pollRokuDevices, 3000)
    }).catch(() => setRokuDevices([]))
  }

  const addRokuDevice = async () => {
    if (!rokuName.trim() || !rokuIp.trim()) return
    setAddingRoku(true)
    setRokuError(null)
    try {
      await api.addRokuDevice(rokuName.trim(), rokuIp.trim())
      setRokuName('')
      setRokuIp('')
      if (rokuPollRef.current) clearTimeout(rokuPollRef.current)
      pollRokuDevices()
    } catch (err: any) {
      setRokuError(err?.message ?? 'Failed to add device')
    } finally {
      setAddingRoku(false)
    }
  }

  const removeRokuDevice = async (id: string) => {
    setRemovingRoku(id)
    try { await api.deleteRokuDevice(id); await api.getRokuDevices().then(setRokuDevices) }
    finally { setRemovingRoku(null) }
  }

  useEffect(() => {
    api.getSettings().then(s => {
      setSettings(s)
      setThreads(String(s.sync_threads))
      setBufferSize(String(s.stream_buffer_size))
      setCastAppId(s.cast_app_id)
        setInternalToken(s.internal_token)
    }).catch(() => setError('Failed to load settings'))
    loadCastSessions()
    pollRokuDevices()
    api.getArrConfig().then(setArr).catch(() => {})
    api.getSmtpConfig().then(c => {
      setSmtp({ host: c.host, port: c.port || '587', username: c.username, password: '', from_address: c.from_address, public_base_url: c.public_base_url })
      setSmtpHasPassword(c.has_password)
    }).catch(() => {})
    api.getScraperSettings().then(setScraperSettings).catch(() => {})
    api.getScraperStats().then(setScraperStats).catch(() => {})
    // One-time check in case a match/refresh-all kicked off elsewhere (or
    // before this mount) is still running — the poll chains take over from
    // here on their own if so, and stay quiet otherwise.
    pollMatchStatus()
    pollRefreshAllStatus()
    return () => {
      if (matchPollRef.current)      clearTimeout(matchPollRef.current)
      if (rokuPollRef.current)       clearTimeout(rokuPollRef.current)
      if (refreshAllPollRef.current) clearTimeout(refreshAllPollRef.current)
    }
  }, [])

  // The guided setup tour's scraper step targets an input that now lives
  // behind the Scrapers tab — force that tab open while the step is active
  // so TourSpotlight's data-tour querySelector can actually find it.
  const tourRoute = tourStore.currentStep?.route
  useEffect(() => {
    if (tourRoute === '/settings') setTab('scrapers')
  }, [tourRoute])

  const patch = async (update: Partial<Settings>) => {
    setSaving(true)
    setError(null)
    try {
      const next = await api.updateSettings(update)
      setSettings(next)
      setThreads(String(next.sync_threads))
      setBufferSize(String(next.stream_buffer_size))
      setCastAppId(next.cast_app_id)
        setInternalToken(next.internal_token)
      // Keep statusStore in sync immediately so the debug banner reflects the change.
      if ('sync_debug'  in update) statusStore.syncDebug  = next.sync_debug
      if ('epg_debug'   in update) statusStore.epgDebug   = next.epg_debug
      // remoteLog.ts reads this directly to decide whether to forward
      // console.error calls — update it now rather than waiting for the
      // next statusStore poll.
      if ('hades_debug' in update) statusStore.hadesDebug = next.hades_debug
    } catch (e: any) {
      setError(e.message ?? 'Save failed')
    } finally {
      setSaving(false)
    }
  }

  const applyThreads = () => {
    const n = parseInt(threads, 10)
    if (!isNaN(n) && n >= 1 && n <= 32) patch({ sync_threads: n })
    else setThreads(settings ? String(settings.sync_threads) : '6')
  }

const applyBuffer = () => {
    const n = parseInt(bufferSize, 10)
    if (!isNaN(n) && n >= 1024) patch({ stream_buffer_size: n })
    else setBufferSize(settings ? String(settings.stream_buffer_size) : '1024')
}

  const applyCastAppId = () => {
    if (castAppId !== (settings?.cast_app_id ?? '')) patch({ cast_app_id: castAppId.trim() })
  }

    const applyInternalToken = () => {
        const v = internalToken.trim()
        if (v && v !== (settings?.internal_token ?? '')) patch({internal_token: v})
        else if (!v) setInternalToken(settings?.internal_token ?? '')
    }

    const regenerateInternalToken = async () => {
        setRegeneratingToken(true)
        try {
            const bytes = new Uint8Array(32)
            crypto.getRandomValues(bytes)
            const v = Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('')
            await patch({internal_token: v})
        } finally {
            setRegeneratingToken(false)
        }
    }

  const resetLibrary = async () => {
    setResetting(true)
    setResetMsg(null)
    try {
      await api.resetLibrary()
      setResetMsg('Library index cleared. Trigger a sync to repopulate.')
      setResetConfirm(false)
    } catch (e: any) {
      setResetMsg(`Error: ${e.message ?? 'Unknown error'}`)
    } finally {
      setResetting(false)
    }
  }

  const dumpDebugDb = async () => {
    setDumping(true)
    setDumpMsg(null)
    try {
      await downloadDebugDump()
    } catch (e: any) {
      setDumpMsg(`Error: ${e.message ?? 'Unknown error'}`)
    } finally {
      setDumping(false)
    }
  }

  const clearAllEpg = async () => {
    setClearing(true)
    setClearMsg(null)
    try {
      const r = await api.clearAllEpg()
      setClearMsg(`Cleared ${r.cleared} row${r.cleared !== 1 ? 's' : ''} — EPG will regenerate on next request.`)
    } catch (e: any) {
      setClearMsg(`Error: ${e.message ?? 'Unknown error'}`)
    } finally {
      setClearing(false)
    }
  }

  const updateScraperConfig = (source: 'tmdb' | 'tvdb' | 'anidb' | 'tvmaze' | 'trakt' | 'anilist' | 'wikidata', field: string, value: string | boolean | number) => {
    if (!scraperSettings) return
    setScraperSettings(prev => prev ? {
      ...prev,
      configs: prev.configs.map(c => c.source === source ? { ...c, [field]: value } : c),
    } : prev)
    setScraperDirty(true)
  }

  const updateThreshold = (v: number) => {
    if (!scraperSettings) return
    setScraperSettings(prev => prev ? { ...prev, match_threshold: v } : prev)
    setScraperDirty(true)
  }

  const updateAnidbDownloadPosters = (v: boolean) => {
    if (!scraperSettings) return
    setScraperSettings(prev => prev ? { ...prev, anidb_download_posters: v } : prev)
    setScraperDirty(true)
  }

  const saveScraperSettings = async () => {
    if (!scraperSettings) return
    setScraperSaving(true)
    try {
      await api.patchScraperSettings(scraperSettings)
      setScraperDirty(false)
      setScraperSaved(true)
      setTimeout(() => setScraperSaved(false), 2000)
      api.getScraperStats().then(setScraperStats).catch(() => {})
    } finally {
      setScraperSaving(false)
    }
  }

  const runMatch = async () => {
    setMatchRunning(true)
    wasMatchRunningRef.current = true
    await api.triggerMatch()
    if (matchPollRef.current) clearTimeout(matchPollRef.current)
    pollMatchStatus()
  }

  const tmdb   = scraperSettings?.configs.find(c => c.source === 'tmdb')
  const tvdb   = scraperSettings?.configs.find(c => c.source === 'tvdb')
  const anidb  = scraperSettings?.configs.find(c => c.source === 'anidb')
  const tvmaze = scraperSettings?.configs.find(c => c.source === 'tvmaze')
  const trakt   = scraperSettings?.configs.find(c => c.source === 'trakt')
  const anilist  = scraperSettings?.configs.find(c => c.source === 'anilist')
  const wikidata = scraperSettings?.configs.find(c => c.source === 'wikidata')

  return (
    <div className={styles.page}>
      {tourStore.currentStep?.route === '/settings' && <TourSpotlight step={tourStore.currentStep} />}

      <div className={styles.titleRow}>
        <h1 className={styles.title}>Settings</h1>
        {saving && <span className={styles.savingText}>Saving…</span>}
      </div>

      {error && (
        <div className={styles.errorBanner}>
          {error}
        </div>
      )}

      {/* ── Tab bar ─────────────────────────────────────────────────────────── */}
      <div className={styles.tabBar}>
        {TABS.map(({ key, label }) => (
          <button
            key={key}
            onClick={() => setTab(key)}
            className={`${styles.tabBtn} ${tab === key ? styles.tabBtnActive : ''}`}
          >
            {label.toUpperCase()}
          </button>
        ))}
      </div>

      {/* ── General ─────────────────────────────────────────────────────────── */}
      {tab === 'general' && (
        <>
          <Section title="Interface">
            <SettingRow
              label="Help & Tips"
              hint={'The small "?" help buttons throughout the app (channel builder, etc.) and their explanation popups. Off removes them entirely, everywhere.'}
            >
              <Toggle
                id="help_tips_enabled"
                checked={helpTipsStore.enabled}
                onChange={v => helpTipsStore.setEnabled(v)}
              />
            </SettingRow>
              <SettingRow
                  label="Default Landing Page"
                  hint="Which page the app opens to after logging in or switching profiles. Any user can override this for their own account on their Account page."
              >
                  <select
                      value={settings?.default_landing_page ?? 'home'}
                      disabled={!settings || saving}
                      onChange={e => patch({default_landing_page: e.target.value})}
                      className={`${styles.input} ${styles.w120} ${styles.inputCursorPointer}`}
                  >
                      <option value="home">Home</option>
                      <option value="guide">Guide</option>
                  </select>
            </SettingRow>
          </Section>

          <Section title="Performance">
            <SettingRow
              label="Sync Worker Threads"
              hint="Parallel connections used when fetching episode metadata from media servers. Range: 1–32."
            >
              <div className={styles.inlineRow}>
                <input
                  type="number" min={1} max={32}
                  value={threads}
                  onChange={e => setThreads(e.target.value)}
                  onBlur={applyThreads}
                  onKeyDown={e => e.key === 'Enter' && applyThreads()}
                  disabled={!settings || saving}
                  className={`${styles.input} ${styles.w60} ${styles.inputCenter}`}
                />
              </div>
            </SettingRow>
            <SettingRow
              label="Image Cache TTL"
              hint="How long poster and backdrop images are cached on disk before re-fetching from the source."
            >
              <select
                value={settings?.image_cache_ttl_hours ?? 2}
                disabled={!settings || saving}
                onChange={e => patch({ image_cache_ttl_hours: parseInt(e.target.value, 10) })}
                className={`${styles.input} ${styles.w120} ${styles.inputCursorPointer}`}
              >
                {([
                  [1,   '1 hour'],
                  [2,   '2 hours'],
                  [6,   '6 hours'],
                  [12,  '12 hours'],
                  [24,  '1 day'],
                  [48,  '2 days'],
                  [168, '7 days'],
                ] as [number, string][]).map(([v, label]) => (
                  <option key={v} value={v}>{label}</option>
                ))}
              </select>
            </SettingRow>
          </Section>

          <Section title="Stream Settings">
            <SettingRow
              label="Stream Buffer Size (KB)"
              hint="Size of the stream buffer while watching streaming channels. (Requires Restart)"
            >
              <div className={styles.inlineRow}>
                <input
                  type="number" min={1024}
                  value={bufferSize}
                  onChange={e => setBufferSize(e.target.value)}
                  onBlur={applyBuffer}
                  onKeyDown={e => e.key === 'Enter' && applyBuffer()}
                  disabled={!settings || saving}
                  className={`${styles.input} ${styles.w120} ${styles.inputRight}`}
                />
                <span className={styles.unitLabel}>KB</span>
              </div>
            </SettingRow>
          </Section>
        </>
      )}

      {/* ── Scrapers ────────────────────────────────────────────────────────── */}
      {tab === 'scrapers' && (
        <>
          <Section title="TMDB — The Movie Database">
            <SettingRow label="Enabled" hint="Primary metadata source for movies and shows.">
              <Toggle
                id="tmdb_enabled"
                checked={tmdb?.enabled ?? false}
                disabled={!scraperSettings}
                onChange={v => updateScraperConfig('tmdb', 'enabled', v)}
              />
            </SettingRow>
            <SettingRow label="API Key (v3)">
              <input
                data-tour="tmdb-api-key-input"
                className={`${styles.input} ${styles.w260}`}
                type="password"
                placeholder="xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                value={tmdb?.api_key ?? ''}
                onChange={e => updateScraperConfig('tmdb', 'api_key', e.target.value)}
              />
            </SettingRow>
            <SettingRow label="Language" hint="e.g. en-US">
              <input
                className={`${styles.input} ${styles.w80}`}
                placeholder="en-US"
                value={tmdb?.language ?? 'en-US'}
                onChange={e => updateScraperConfig('tmdb', 'language', e.target.value)}
              />
            </SettingRow>
            <SettingRow label="Language Weight" hint="Bonus added to score (0.0 to 1.0) if scraper language matches library preference.">
              <input
                type="number" step={0.05} min={0} max={1}
                className={`${styles.input} ${styles.w80}`}
                value={tmdb?.language_weight ?? 0.1}
                onChange={e => updateScraperConfig('tmdb', 'language_weight', parseFloat(e.target.value))}
              />
            </SettingRow>
          </Section>

          <Section title="TVDB — TheTVDB">
            <SettingRow label="Enabled" hint="Secondary source; provides TVDB IDs and series data.">
              <Toggle
                id="tvdb_enabled"
                checked={tvdb?.enabled ?? false}
                disabled={!scraperSettings}
                onChange={v => updateScraperConfig('tvdb', 'enabled', v)}
              />
            </SettingRow>
            <SettingRow label="API Key (v4 project key)">
              <input
                className={`${styles.input} ${styles.w260}`}
                type="password"
                placeholder="xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
                value={tvdb?.api_key ?? ''}
                onChange={e => updateScraperConfig('tvdb', 'api_key', e.target.value)}
              />
            </SettingRow>
            <SettingRow label="Subscriber PIN" hint="Optional; required for some TVDB accounts.">
              <input
                className={`${styles.input} ${styles.w140}`}
                type="password"
                placeholder="optional"
                value={tvdb?.pin ?? ''}
                onChange={e => updateScraperConfig('tvdb', 'pin', e.target.value)}
              />
            </SettingRow>
            <SettingRow label="Language" hint="e.g. eng">
              <input
                className={`${styles.input} ${styles.w80}`}
                placeholder="eng"
                value={tvdb?.language ?? 'eng'}
                onChange={e => updateScraperConfig('tvdb', 'language', e.target.value)}
              />
            </SettingRow>
            <SettingRow label="Language Weight" hint="Bonus added to score (0.0 to 1.0) if scraper language matches library preference.">
              <input
                type="number" step={0.05} min={0} max={1}
                className={`${styles.input} ${styles.w80}`}
                value={tvdb?.language_weight ?? 0.1}
                onChange={e => updateScraperConfig('tvdb', 'language_weight', parseFloat(e.target.value))}
              />
            </SettingRow>
          </Section>

          <Section title="TVMaze">
            <SettingRow label="Enabled" hint="Free, unauthenticated TV show database — no API key required. Shows only; TVMaze doesn't catalogue movies.">
              <Toggle
                id="tvmaze_enabled"
                checked={tvmaze?.enabled ?? false}
                disabled={!scraperSettings}
                onChange={v => updateScraperConfig('tvmaze', 'enabled', v)}
              />
            </SettingRow>
            <SettingRow label="Language Weight" hint="Bonus added to score (0.0 to 1.0) if scraper language matches library preference. TVMaze's data is predominantly English.">
              <input
                type="number" step={0.05} min={0} max={1}
                className={`${styles.input} ${styles.w80}`}
                value={tvmaze?.language_weight ?? 0.1}
                onChange={e => updateScraperConfig('tvmaze', 'language_weight', parseFloat(e.target.value))}
              />
            </SettingRow>
          </Section>

          <Section title="Trakt">
            <SettingRow label="Enabled" hint="Metadata source for both movies and shows, keyed by Trakt's own IDs plus cross-references to TMDB/IMDB.">
              <Toggle
                id="trakt_enabled"
                checked={trakt?.enabled ?? false}
                disabled={!scraperSettings}
                onChange={v => updateScraperConfig('trakt', 'enabled', v)}
              />
            </SettingRow>
            <SettingRow label="Client ID" hint="Your registered Trakt app's Client ID. Register at trakt.tv/oauth/applications.">
              <input
                className={`${styles.input} ${styles.w280}`}
                type="password"
                placeholder="xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                value={trakt?.api_key ?? ''}
                onChange={e => updateScraperConfig('trakt', 'api_key', e.target.value)}
              />
            </SettingRow>
            <SettingRow label="Language Weight" hint="Bonus added to score (0.0 to 1.0) if scraper language matches library preference.">
              <input
                type="number" step={0.05} min={0} max={1}
                className={`${styles.input} ${styles.w80}`}
                value={trakt?.language_weight ?? 0.1}
                onChange={e => updateScraperConfig('trakt', 'language_weight', parseFloat(e.target.value))}
              />
            </SettingRow>
          </Section>

          <Section title="AniDB">
            <SettingRow label="Enabled" hint="Anime metadata source for shows and movies via the AniDB HTTP API.">
              <Toggle
                id="anidb_enabled"
                checked={anidb?.enabled ?? false}
                disabled={!scraperSettings}
                onChange={v => updateScraperConfig('anidb', 'enabled', v)}
              />
            </SettingRow>
            <SettingRow label="Client Name" hint="Your registered AniDB HTTP API client name. Register at anidb.net/software/add.">
              <input
                className={`${styles.input} ${styles.w200}`}
                placeholder="myclientname"
                value={anidb?.api_key ?? ''}
                onChange={e => updateScraperConfig('anidb', 'api_key', e.target.value)}
              />
            </SettingRow>
            <SettingRow label="Language Weight" hint="Bonus added to score (0.0 to 1.0) if scraper language matches library preference.">
              <input
                type="number" step={0.05} min={0} max={1}
                className={`${styles.input} ${styles.w80}`}
                value={anidb?.language_weight ?? 0.1}
                onChange={e => updateScraperConfig('anidb', 'language_weight', parseFloat(e.target.value))}
              />
            </SettingRow>
            <SettingRow
              label="Download & Cache Posters Locally"
              hint="Saves a confirmed match's poster to disk (next to the media, as aniThumb.jpg) the first time it's matched, so it loads instantly afterward with no AniDB CDN dependency. Tradeoff: AniDB rate-limits image fetches to ~1 every 2.1s, so turning this on adds a one-time ~2s delay per newly-matched or refreshed item, and matching/refreshing many anime at once will serialize slowly. Off by default — posters still work without this, just fetched live (and cached briefly) on each request instead of saved permanently."
            >
              <Toggle
                id="anidb_download_posters"
                checked={scraperSettings?.anidb_download_posters ?? false}
                disabled={!scraperSettings}
                onChange={v => updateAnidbDownloadPosters(v)}
              />
            </SettingRow>
          </Section>

          <Section title="AniList">
            <SettingRow label="Enabled" hint="Anime/manga metadata source via AniList's free public GraphQL API — no API key required. Unlike AniDB, there's no separate 'include' checkbox: once enabled here, add &quot;anilist&quot; to a library's scraper priority order (Sources page) to actually query it for that library.">
              <Toggle
                id="anilist_enabled"
                checked={anilist?.enabled ?? false}
                disabled={!scraperSettings}
                onChange={v => updateScraperConfig('anilist', 'enabled', v)}
              />
            </SettingRow>
            <SettingRow label="Language Weight" hint="Bonus added to score (0.0 to 1.0) if scraper language matches library preference.">
              <input
                type="number" step={0.05} min={0} max={1}
                className={`${styles.input} ${styles.w80}`}
                value={anilist?.language_weight ?? 0.1}
                onChange={e => updateScraperConfig('anilist', 'language_weight', parseFloat(e.target.value))}
              />
            </SettingRow>
          </Section>

          <Section title="Wikidata">
            <SettingRow label="Enabled" hint="Broadest coverage of any source here (Wikidata's structured data + a Wikipedia summary for overview text) — no API key required. No per-episode data and thinner fields than dedicated media databases, so it's best kept as a low-priority fallback for obscure, regional, or web-only titles the other sources don't have.">
              <Toggle
                id="wikidata_enabled"
                checked={wikidata?.enabled ?? false}
                disabled={!scraperSettings}
                onChange={v => updateScraperConfig('wikidata', 'enabled', v)}
              />
            </SettingRow>
            <SettingRow label="Language Weight" hint="Bonus added to score (0.0 to 1.0) if scraper language matches library preference.">
              <input
                type="number" step={0.05} min={0} max={1}
                className={`${styles.input} ${styles.w80}`}
                value={wikidata?.language_weight ?? 0.1}
                onChange={e => updateScraperConfig('wikidata', 'language_weight', parseFloat(e.target.value))}
              />
            </SettingRow>
          </Section>

          <Section title="Matching">
            <SettingRow
              label="Confidence Threshold"
              hint="Items below this score go to the Review Queue. 100% = only exact matches are auto-accepted."
            >
              <div className={styles.inlineRowSm}>
                <input
                  type="range" min={0} max={1} step={0.05}
                  value={scraperSettings?.match_threshold ?? 0.8}
                  onChange={e => updateThreshold(parseFloat(e.target.value))}
                  disabled={!scraperSettings}
                  className={styles.thresholdSlider}
                />
                <span className={styles.thresholdValue}>
                  {Math.round((scraperSettings?.match_threshold ?? 0.8) * 100)}%
                </span>
              </div>
            </SettingRow>

            {scraperStats && (
              <div className={styles.statsRow}>
                {([
                  { label: 'Total',     value: scraperStats.total,     color: 'var(--hds-txt-2)' },
                  { label: 'Matched',   value: scraperStats.matched,   color: 'var(--hds-match-green)' },
                  { label: 'Uncertain', value: scraperStats.uncertain, color: 'var(--hds-match-amber)' },
                  { label: 'Unmatched', value: scraperStats.unmatched, color: 'var(--hds-match-red)' },
                  { label: 'Unscraped', value: scraperStats.unscraped, color: 'var(--hds-txt-3)' },
                  { label: 'Skipped',   value: scraperStats.skipped,   color: 'var(--hds-txt-3)' },
                ] as const).map(({ label, value, color }) => (
                  <div key={label} className={styles.statCard}>
                    <div className={styles.statValue} style={{ color }}>{value}</div>
                    <div className={styles.statLabel}>{label.toUpperCase()}</div>
                  </div>
                ))}
              </div>
            )}

            <div className={styles.actionsRow}>
              <NavButton
                id="run-match"
                onClick={runMatch}
                disabled={matchRunning}
                className={`${styles.navBtn} ${styles.navBtnNeutral16} ${matchRunning ? styles.navBtnNeutral16TxtB : styles.navBtnNeutral16TxtA} ${matchRunning ? styles.navBtnCursorNotAllowed : styles.navBtnCursorPointer} ${matchRunning ? styles.navBtnFaded6 : styles.navBtnOpaque}`}
              >
                {matchRunning ? '● Running…' : 'Run Match Pass'}
              </NavButton>
              <NavButton
                id="refresh-all"
                onClick={refreshAll}
                disabled={refreshingAll}
                title="Re-pulls title, overview, posters, ratings, etc. from each already-linked source for every matched show/movie"
                className={`${styles.navBtn} ${styles.navBtnNeutral16} ${refreshingAll ? styles.navBtnNeutral16TxtB : styles.navBtnNeutral16TxtA} ${refreshingAll ? styles.navBtnCursorNotAllowed : styles.navBtnCursorPointer} ${refreshingAll ? styles.navBtnFaded6 : styles.navBtnOpaque}`}
              >
                {refreshingAll ? '● Refreshing metadata…' : 'Refresh All Metadata'}
              </NavButton>
            </div>

            {/* Real progress instead of a static "Refreshing metadata…" button
                label — a library-wide refresh iterates every matched show/
                movie against rate-limited scraper APIs and can run for many
                minutes, so a bar + counts is the difference between "working"
                and "looks stuck." Stays visible with its final tally after
                completion (running flips false, total/processed don't reset)
                until the next run clears it back to null. */}
            {refreshAllProgress && refreshAllProgress.total > 0 && (
              <div className={styles.refreshProgressWrap}>
                <div className={styles.refreshProgressTrack}>
                  <div
                    className={styles.refreshProgressFill}
                    style={{ width: `${Math.min(100, (refreshAllProgress.processed / refreshAllProgress.total) * 100)}%` }}
                  />
                </div>
                <div className={styles.refreshProgressLabel}>
                  {refreshingAll ? 'Refreshing' : 'Finished'} {refreshAllProgress.processed} / {refreshAllProgress.total}
                  {' — '}{refreshAllProgress.refreshed} refreshed
                  {refreshAllProgress.failed > 0 && `, ${refreshAllProgress.failed} failed`}
                </div>
              </div>
            )}

            <SettingRow
              label="Confirm All Matches"
              hint="Marks every currently auto-matched, unconfirmed item as human-confirmed — the same effect as clicking Confirm Match on each one individually. Unlocks Push to Sources / Refresh Metadata for all of them at once. Does not re-fetch metadata (already pulled at match time — use Refresh All Metadata for that)."
            >
              {!confirmAllPending ? (
                <NavButton
                  id="confirm-all-matches"
                  onClick={() => { setConfirmAllPending(true); setConfirmAllMsg(null) }}
                  className={`${styles.navBtn} ${styles.navBtnNeutral16} ${styles.navBtnNeutral16TxtA} ${styles.navBtnCursorPointer}`}
                >
                  Confirm All Matches
                </NavButton>
              ) : (
                <div className={styles.inlineRow}>
                  <span className={styles.confirmText}>
                    This will confirm all active unconfirmed matches in the library. Sure?
                  </span>
                  <NavButton
                    id="confirm-all-matches-confirm"
                    onClick={confirmAllMatches}
                    disabled={confirmingAll}
                    className={`${styles.navBtn} ${styles.navBtnGreen14} ${confirmingAll ? styles.navBtnCursorNotAllowed : styles.navBtnCursorPointer} ${confirmingAll ? styles.navBtnFaded6 : styles.navBtnOpaque}`}
                  >
                    {confirmingAll ? 'Confirming…' : 'Yes, confirm all'}
                  </NavButton>
                  <NavButton
                    id="confirm-all-matches-cancel"
                    onClick={() => setConfirmAllPending(false)}
                    disabled={confirmingAll}
                    className={`${styles.navBtn} ${styles.navBtnCancel10} ${styles.navBtnCursorPointer}`}
                  >
                    Cancel
                  </NavButton>
                </div>
              )}
            </SettingRow>
            {confirmAllMsg && (
              <div className={`${styles.msgRow} ${confirmAllMsg.startsWith('Error') ? styles.msgRowError : ''}`}>
                {confirmAllMsg}
              </div>
            )}
          </Section>

          <div className={styles.inlineRowGap12}>
            <NavButton
              id="save-scraper"
              onClick={saveScraperSettings}
              disabled={!scraperDirty || scraperSaving}
              className={`${styles.navBtn} ${styles.navBtnGold18} ${(!scraperDirty || scraperSaving) ? styles.navBtnCursorNotAllowed : styles.navBtnCursorPointer} ${(!scraperDirty || scraperSaving) ? styles.navBtnFaded45 : styles.navBtnOpaque}`}
            >
              {scraperSaving ? 'Saving…' : scraperSaved ? '✓ Saved' : 'Save Scraper Settings'}
            </NavButton>
          </div>
        </>
      )}

      {/* ── Integrations ────────────────────────────────────────────────────── */}
      {tab === 'integrations' && (
        <>
          <Section title="Sonarr">
            <ArrField label="URL" hint="e.g. http://sonarr:8989" value={arr.sonarr_url}
              onChange={v => setArr(a => ({ ...a, sonarr_url: v }))} />
            <ArrField label="API Key" value={arr.sonarr_api_key} password
              onChange={v => setArr(a => ({ ...a, sonarr_api_key: v }))} />
          </Section>

          <Section title="Radarr">
            <ArrField label="URL" hint="e.g. http://radarr:7878" value={arr.radarr_url}
              onChange={v => setArr(a => ({ ...a, radarr_url: v }))} />
            <ArrField label="API Key" value={arr.radarr_api_key} password
              onChange={v => setArr(a => ({ ...a, radarr_api_key: v }))} />
          </Section>

          <div className={styles.inlineRowGap12}>
            <NavButton
              id="save-arr"
              onClick={async () => {
                setArrSave('saving')
                try { await api.patchArrConfig(arr); setArrSave('ok') }
                catch { setArrSave('err') }
                setTimeout(() => setArrSave('idle'), 2000)
              }}
              disabled={arrSave === 'saving'}
              className={`${styles.navBtn} ${styles.navBtnGold18} ${arrSave === 'saving' ? styles.navBtnCursorNotAllowed : styles.navBtnCursorPointer} ${arrSave === 'saving' ? styles.navBtnFaded6 : styles.navBtnOpaque}`}
            >
              {arrSave === 'saving' ? 'Saving…' : arrSave === 'ok' ? 'Saved' : arrSave === 'err' ? 'Error' : 'Save Settings'}
            </NavButton>
            <span className={styles.captionText}>
              Used when adding missing media from the import preview.
            </span>
          </div>

          <Section title="Email / SMTP">
            <ArrField label="Host" hint="e.g. smtp.gmail.com" value={smtp.host}
              onChange={v => setSmtp(s => ({ ...s, host: v }))} />
            <ArrField label="Port" hint="STARTTLS only — 587 is the common case" value={smtp.port}
              onChange={v => setSmtp(s => ({ ...s, port: v }))} />
            <ArrField label="Username" value={smtp.username}
              onChange={v => setSmtp(s => ({ ...s, username: v }))} />
            <SettingRow label="Password" hint={smtpHasPassword ? 'Leave blank to keep the current one' : undefined}>
              <input
                type="password"
                placeholder={smtpHasPassword ? '••••••••  (unchanged)' : ''}
                value={smtp.password}
                onChange={e => setSmtp(s => ({ ...s, password: e.target.value }))}
                className={`${styles.input} ${styles.w240}`}
              />
            </SettingRow>
            <ArrField label="From address" hint="What recipients see as the sender" value={smtp.from_address}
              onChange={v => setSmtp(s => ({ ...s, from_address: v }))} />
            <ArrField label="Public base URL" hint="e.g. https://pantheon.example.com — used to build invite links" value={smtp.public_base_url}
              onChange={v => setSmtp(s => ({ ...s, public_base_url: v }))} />
          </Section>

          <div className={styles.inlineRowGap12Wrap}>
            <NavButton
              id="save-smtp"
              onClick={async () => {
                setSmtpSave('saving')
                try {
                  const body: Partial<Record<keyof SmtpForm, string>> = {
                    host: smtp.host, port: smtp.port, username: smtp.username,
                    from_address: smtp.from_address, public_base_url: smtp.public_base_url,
                  }
                  if (smtp.password) body.password = smtp.password
                  await api.setSmtpConfig(body)
                  setSmtpSave('ok')
                  if (smtp.password) { setSmtpHasPassword(true); setSmtp(s => ({ ...s, password: '' })) }
                } catch { setSmtpSave('err') }
                setTimeout(() => setSmtpSave('idle'), 2000)
              }}
              disabled={smtpSave === 'saving'}
              className={`${styles.navBtn} ${styles.navBtnGold18} ${smtpSave === 'saving' ? styles.navBtnCursorNotAllowed : styles.navBtnCursorPointer} ${smtpSave === 'saving' ? styles.navBtnFaded6 : styles.navBtnOpaque}`}
            >
              {smtpSave === 'saving' ? 'Saving…' : smtpSave === 'ok' ? 'Saved' : smtpSave === 'err' ? 'Error' : 'Save Settings'}
            </NavButton>

            <input
              placeholder="you@example.com"
              value={testEmailTo}
              onChange={e => setTestEmailTo(e.target.value)}
              className={`${styles.input} ${styles.w200}`}
            />
            <NavButton
              id="test-smtp"
              onClick={async () => {
                if (!testEmailTo) return
                setTestSend('sending'); setTestError('')
                try { await api.testSmtp(testEmailTo); setTestSend('ok') }
                catch (e: any) { setTestSend('err'); setTestError(e.message ?? 'Send failed') }
                setTimeout(() => setTestSend('idle'), 4000)
              }}
              disabled={testSend === 'sending' || !testEmailTo}
              className={`${styles.navBtn} ${styles.navBtnLine18} ${(testSend === 'sending' || !testEmailTo) ? styles.navBtnCursorNotAllowed : styles.navBtnCursorPointer} ${(testSend === 'sending' || !testEmailTo) ? styles.navBtnFaded5 : styles.navBtnOpaque}`}
            >
              {testSend === 'sending' ? 'Sending…' : testSend === 'ok' ? 'Sent ✓' : testSend === 'err' ? 'Failed' : 'Send Test Email'}
            </NavButton>
            {testSend === 'err' && testError && (
              <span className={styles.captionTextDanger}>{testError}</span>
            )}
          </div>
        </>
      )}

      {/* ── Devices ─────────────────────────────────────────────────────────── */}
      {tab === 'devices' && (
        <>
          <Section title="Chromecast">
            <SettingRow
              label="Receiver Application ID"
              hint="From the Google Cast SDK Developer Console, registered against the Pantheon custom receiver's hosted URL. Leave blank to disable the Cast button."
            >
              <input
                className={`${styles.input} ${styles.w200}`}
                placeholder="XXXXXXXX"
                value={castAppId}
                onChange={e => setCastAppId(e.target.value)}
                onBlur={applyCastAppId}
                onKeyDown={e => e.key === 'Enter' && applyCastAppId()}
                disabled={!settings || saving}
              />
            </SettingRow>
            <SettingRow
              label="Remote / HTTPS Access"
              hint="Chrome only enables the Cast button on a secure origin (HTTPS, or localhost) — a plain http://<lan-ip> address never satisfies this. Cloudflare Tunnel is one free way to get a real HTTPS URL for Pantheon without port-forwarding."
            >
              <HelpTip title="Cloudflare Tunnel Setup" label="Setup Guide">
                <HelpSection title="Why">
                  Google Chrome disables the Chromecast Sender API entirely on insecure origins — casting only works from an <code>https://</code> URL, or from <code>http://localhost</code> on the machine Pantheon itself runs on. A LAN address like <code>http://192.168.1.20:8000</code> will never show a Cast button, no matter what's configured elsewhere in this app.
                </HelpSection>
                <HelpSection title="What Cloudflare Tunnel does">
                  Gives Pantheon a real <code>https://</code> hostname (e.g. <code>pantheon.yourdomain.com</code>) that reaches your server without opening any inbound ports on your router — the tunnel is outbound-only from your network to Cloudflare's edge.
                </HelpSection>
                <HelpSection title="Setup">
                  <ol className={styles.helpList}>
                    <li className={styles.helpListItemGap}>You need a domain added to Cloudflare (DNS managed by Cloudflare — free tier is fine).</li>
                    <li className={styles.helpListItemGap}>Cloudflare Zero Trust dashboard → <b className={styles.helpTextBold}>Networks → Tunnels → Create a tunnel</b> → connector type <b className={styles.helpTextBold}>Docker</b> → copy the token it gives you.</li>
                    <li className={styles.helpListItemGap}>In the same wizard, add a <b className={styles.helpTextBold}>Public Hostname</b>: the hostname you want (e.g. <code>pantheon.yourdomain.com</code>) → service type <b className={styles.helpTextBold}>HTTP</b> → URL <code>hermes:8000</code>. This is also where the hostname itself is configured — nothing in Pantheon's own files needs your hostname hardcoded.</li>
                    <li className={styles.helpListItemGap}>Put that token in a <code>.env</code> file next to Pantheon's <code>docker-compose.yml</code>:<br /><code>CLOUDFLARE_TUNNEL_TOKEN=eyJ...</code></li>
                    <li>Start/restart the stack however you normally do (plain <code>docker compose up -d</code>, or the Unraid Compose Manager UI) — no extra flags needed. Leaving the token blank keeps the container quietly stopped and doesn't affect anything else.</li>
                  </ol>
                </HelpSection>
                <HelpSection title="Important">
                  <p className={styles.helpParaFlat}>
                    The <code>cloudflared</code> container has to actually be running — the tunnel
                    only exists while it's up. Registering a tunnel and hostname in the Cloudflare
                    dashboard doesn't do anything on its own if <code>CLOUDFLARE_TUNNEL_TOKEN</code>{' '}
                    isn't set, or the container ever gets stopped — the hostname will simply fail to
                    connect until it's running again. Check it with{' '}
                    <code>docker compose ps cloudflared</code>.
                  </p>
                </HelpSection>
                <HelpSection title="Security note">
                  This makes Pantheon reachable from the public internet at whatever hostname you choose, not just your LAN. Kairos's own login screen still gates access — if you want an extra layer, Cloudflare Zero Trust Access can require an email/SSO check before a request ever reaches Pantheon, configured separately in the same dashboard.
                </HelpSection>
              </HelpTip>
            </SettingRow>
          </Section>

          <Section title="Cast Devices">
            {castSessions === null ? (
              <div className={styles.listMsg}>Loading…</div>
            ) : castSessions.length === 0 ? (
              <div className={styles.listMsg}>
                No devices paired yet — casting to a Chromecast or Google TV for the first time will add one here.
              </div>
            ) : (
              castSessions.map(s => (
                <SettingRow
                  key={s.session_id}
                  label={`Paired ${new Date(s.created_at * 1000).toLocaleDateString()}`}
                  hint={`Last used ${new Date(s.last_seen * 1000).toLocaleString()}`}
                >
                  <NavButton
                    id={`cast-revoke-${s.session_id}`}
                    onClick={() => revokeCastSession(s.session_id)}
                    disabled={revokingCast === s.session_id}
                    className={`${styles.navBtn} ${styles.navBtnRevoke12} ${styles.navBtnCursorPointer} ${revokingCast === s.session_id ? styles.navBtnFaded5 : styles.navBtnOpaque}`}
                  >
                    {revokingCast === s.session_id ? 'Revoking…' : 'Revoke'}
                  </NavButton>
                </SettingRow>
              ))
            )}
          </Section>

          <Section title="Roku Devices">
            <SettingRow
              label="Add a Roku device"
              hint="Enter the IP address shown on the Roku itself (Settings → Network → About) — the Pantheon channel must already be open and signed in on it once before pairing can complete."
            >
              <div className={styles.rokuAddRow}>
                <input
                  type="text" placeholder="Living Room TV" value={rokuName}
                  onChange={e => setRokuName(e.target.value)}
                  className={`${styles.rokuField} ${styles.w140}`}
                />
                <input
                  type="text" placeholder="192.168.1.50" value={rokuIp}
                  onChange={e => setRokuIp(e.target.value)}
                  className={`${styles.rokuField} ${styles.w120}`}
                />
                <NavButton
                  id="roku-add" onClick={addRokuDevice} disabled={addingRoku || !rokuName.trim() || !rokuIp.trim()}
                  className={`${styles.navBtn} ${styles.navBtnViolet14} ${styles.navBtnCursorPointer} ${addingRoku ? styles.navBtnFaded5 : styles.navBtnOpaque}`}
                >
                  {addingRoku ? 'Adding…' : 'Add'}
                </NavButton>
              </div>
            </SettingRow>
            {rokuError && <div className={styles.rokuErrorMsg}>{rokuError}</div>}

            {rokuDevices === null ? (
              <div className={styles.listMsg}>Loading…</div>
            ) : rokuDevices.length === 0 ? (
              <div className={styles.listMsg}>No Roku devices added yet.</div>
            ) : (
              rokuDevices.map(d => (
                <SettingRow
                  key={d.id}
                  label={d.name}
                  hint={d.paired ? d.ip_address : `${d.ip_address} — waiting for the channel to confirm pairing…`}
                >
                  <NavButton
                    id={`roku-remove-${d.id}`}
                    onClick={() => removeRokuDevice(d.id)}
                    disabled={removingRoku === d.id}
                    className={`${styles.navBtn} ${styles.navBtnRevoke12} ${styles.navBtnCursorPointer} ${removingRoku === d.id ? styles.navBtnFaded5 : styles.navBtnOpaque}`}
                  >
                    {removingRoku === d.id ? 'Removing…' : 'Remove'}
                  </NavButton>
                </SettingRow>
              ))
            )}
          </Section>
        </>
      )}

      {/* ── Diagnostics ─────────────────────────────────────────────────────── */}
      {tab === 'diagnostics' && (
        <>
          <Section title="Diagnostics">
            <SettingRow
              label="Sync Debug Logging"
              hint="Verbose output for every sync, ffprobe call, and scraper query — phase timings, per-episode path mapping, and chapter probe results. Disable when not actively diagnosing."
            >
              <Toggle
                id="sync_debug"
                checked={settings?.sync_debug ?? false}
                disabled={!settings || saving}
                onChange={v => patch({ sync_debug: v })}
              />
            </SettingRow>
            <SettingRow
              label="EPG Debug Logging"
              hint="Emits verbose [epg] lines to stdout during schedule projection. Visible in engine logs and docker logs."
            >
              <Toggle
                id="epg_debug"
                checked={settings?.epg_debug ?? false}
                disabled={!settings || saving}
                onChange={v => patch({ epg_debug: v })}
              />
            </SettingRow>
            <SettingRow
              label="Verbose Transcode Logging"
              hint="Logs full ffmpeg command lines and -v verbose output for every spawned transcode (live channels, VOD, previews) — noisy, enable only when debugging hardware acceleration issues. Applies to new streams within ~15s, no restart needed."
            >
              <Toggle
                id="verbose_transcode_logs"
                checked={settings?.verbose_transcode_logs ?? false}
                disabled={!settings || saving}
                onChange={v => patch({ verbose_transcode_logs: v })}
              />
            </SettingRow>
            <SettingRow
              label="Verbose Gateway Logging"
              hint="Shows Hermes's access-error log ([hermes] 4XX/5XX responses) and device-unreachable warnings ([roku-ecp]) in the Activity page — noisy on a busy or public-facing instance, enable only when diagnosing gateway/device issues. Always written to hermes.log either way. Applies within ~15s, no restart needed."
            >
              <Toggle
                id="verbose_gateway_logs"
                checked={settings?.verbose_gateway_logs ?? false}
                disabled={!settings || saving}
                onChange={v => patch({ verbose_gateway_logs: v })}
              />
            </SettingRow>
            <SettingRow
              label="Hades Console Error Logging"
              hint="Forwards this browser's console.error() calls to the server log (as [hades] lines) in addition to uncaught errors, which always forward. Enable when reproducing a UI bug you need in the server-side log alongside backend activity."
            >
              <Toggle
                id="hades_debug"
                checked={settings?.hades_debug ?? false}
                disabled={!settings || saving}
                onChange={v => patch({ hades_debug: v })}
              />
            </SettingRow>
              <SettingRow
                  label="Internal Service Token"
                  hint="Shared secret Hephaestus sends when reporting live-channel playback progress back to Kairos — auto-generated on first run, rotating it here takes effect on the next report with no restart needed. Only change this if you suspect it's been exposed."
              >
                  <div className={styles.inlineRow}>
                      <input
                          className={`${styles.input} ${styles.w200}`}
                          value={internalToken}
                          onChange={e => setInternalToken(e.target.value)}
                          onBlur={applyInternalToken}
                          onKeyDown={e => e.key === 'Enter' && applyInternalToken()}
                          disabled={!settings || saving}
                          spellCheck={false}
                      />
                      <NavButton
                          id="regenerate-internal-token"
                          onClick={regenerateInternalToken}
                          disabled={!settings || saving || regeneratingToken}
                          className={`${styles.navBtn} ${styles.navBtnNeutral14} ${(!settings || saving || regeneratingToken) ? styles.navBtnCursorNotAllowed : styles.navBtnCursorPointer} ${(!settings || saving || regeneratingToken) ? styles.navBtnFaded6 : styles.navBtnOpaque}`}
                      >
                          {regeneratingToken ? 'Generating…' : 'Regenerate'}
                      </NavButton>
                  </div>
            </SettingRow>
            <SettingRow
              label="Download DB Snapshot"
              hint="Downloads a sqlite file with just the library/matching tables (shows, movies, episodes, source mappings, scraper/review-queue data) — no login or session data. For sharing with support or digging into library-count/matching discrepancies offline."
            >
              <NavButton
                id="debug-dump"
                onClick={dumpDebugDb}
                disabled={dumping}
                className={`${styles.navBtn} ${styles.navBtnNeutral14} ${dumping ? styles.navBtnCursorNotAllowed : styles.navBtnCursorPointer} ${dumping ? styles.navBtnFaded6 : styles.navBtnOpaque}`}
              >
                {dumping ? 'Preparing…' : 'Download'}
              </NavButton>
            </SettingRow>
            {dumpMsg && (
              <div className={`${styles.msgRow} ${styles.msgRowError}`}>{dumpMsg}</div>
            )}
          </Section>

          <Section title="EPG Cache">
            <SettingRow
              label="Clear All EPG Caches"
              hint="Deletes all scheduled program rows across every channel. The guide will regenerate on next request."
            >
              <NavButton
                id="clear-epg"
                onClick={clearAllEpg}
                disabled={clearing}
                className={`${styles.navBtn} ${styles.navBtnDangerSoft14} ${clearing ? styles.navBtnCursorNotAllowed : styles.navBtnCursorPointer} ${clearing ? styles.navBtnFaded6 : styles.navBtnOpaque}`}
              >
                {clearing ? 'Clearing…' : 'Clear All'}
              </NavButton>
            </SettingRow>
            {clearMsg && (
              <div className={styles.msgRow}>{clearMsg}</div>
            )}
          </Section>

          <Section title="Danger Zone">
            <SettingRow
              label="Reset Library Index"
              hint="Wipes all shows, episodes, movies, and source mappings. Source/library config, channels, and users are kept. The next sync rebuilds everything from scratch."
            >
              {!resetConfirm ? (
                <NavButton
                  id="reset-library"
                  onClick={() => { setResetConfirm(true); setResetMsg(null) }}
                  className={`${styles.navBtn} ${styles.navBtnDangerSoft14} ${styles.navBtnCursorPointer} ${styles.navBtnOpaque}`}
                >
                  Reset
                </NavButton>
              ) : (
                <div className={styles.inlineRow}>
                  <span className={styles.confirmTextDanger}>Sure?</span>
                  <NavButton
                    id="reset-library-confirm"
                    onClick={resetLibrary}
                    disabled={resetting}
                    className={`${styles.navBtn} ${styles.navBtnDangerStrong14} ${resetting ? styles.navBtnCursorNotAllowed : styles.navBtnCursorPointer} ${resetting ? styles.navBtnFaded6 : styles.navBtnOpaque}`}
                  >
                    {resetting ? 'Resetting…' : 'Yes, wipe it'}
                  </NavButton>
                  <NavButton
                    id="reset-library-cancel"
                    onClick={() => setResetConfirm(false)}
                    className={`${styles.navBtn} ${styles.navBtnCancel10} ${styles.navBtnCursorPointer}`}
                  >
                    Cancel
                  </NavButton>
                </div>
              )}
            </SettingRow>
            {resetMsg && (
              <div className={`${styles.msgRow} ${resetMsg.startsWith('Error') ? styles.msgRowError : ''}`}>
                {resetMsg}
              </div>
            )}
          </Section>
        </>
      )}
    </div>
  )
})

function ArrField({ label, hint, value, onChange, password }: {
  label: string; hint?: string; value: string; onChange: (v: string) => void; password?: boolean
}) {
  return (
    <SettingRow label={label} hint={hint}>
      <input
        type={password ? 'password' : 'text'}
        value={value}
        onChange={e => onChange(e.target.value)}
        className={`${styles.input} ${styles.w240}`}
      />
    </SettingRow>
  )
}
