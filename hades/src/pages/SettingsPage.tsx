import { useEffect, useRef, useState } from 'react'
import { observer } from 'mobx-react-lite'
import { api } from '../api/client'
import { statusStore, helpTipsStore } from '../stores'
import { tourStore } from '../stores/TourStore'
import { TourSpotlight } from '../components/tour/TourSpotlight'
import type { ArrConfig, CastSessionInfo, ScraperSettings, ScraperStats } from '../api/types'
import { useFocusable } from '../nav/useFocusable'
import { HelpTip, HelpSection } from '../channel/HelpTip'

interface Settings {
  epg_debug:              boolean
  sync_debug:             boolean
  sync_threads:           number
  stream_buffer_size:     number
  image_cache_ttl_hours:  number
  verbose_transcode_logs: boolean
  cast_app_id:            string
}

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
      style={{
        position: 'relative', display: 'inline-flex', alignItems: 'center',
        width: 40, height: 22, borderRadius: 11, border: 'none', cursor: disabled ? 'not-allowed' : 'pointer',
        background: checked ? 'oklch(0.72 0.18 140)' : 'oklch(0.28 0.01 286)',
        transition: 'background 0.15s', flexShrink: 0, outline: 'none',
        opacity: disabled ? 0.5 : 1,
      }}
    >
      <span style={{
        position: 'absolute', left: checked ? 20 : 2, width: 18, height: 18,
        borderRadius: '50%', background: '#fff', transition: 'left 0.15s', boxShadow: '0 1px 3px rgba(0,0,0,0.4)',
      }} />
    </button>
  )
}

function SettingRow({ label, hint, children }: { label: string; hint?: string; children: React.ReactNode }) {
  return (
    // flexWrap so the label/hint stack above the control on narrow screens
    // instead of both fighting for horizontal space — several controls here
    // have hardcoded widths (e.g. 260px API key inputs) that would otherwise
    // crush the label into a sliver, or overflow outright, on a phone.
    <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', flexWrap: 'wrap', gap: '6px 24px', padding: '14px 0', borderBottom: '1px solid oklch(0.22 0.01 286)' }}>
      <div>
        <div style={{ fontSize: 13, color: 'var(--hds-txt)', fontWeight: 500 }}>{label}</div>
        {hint && <div style={{ fontSize: 11, color: 'var(--hds-txt-3)', marginTop: 3 }}>{hint}</div>}
      </div>
      <div style={{ flexShrink: 0 }}>{children}</div>
    </div>
  )
}

// Local helper — this page has several one-off action buttons (Clear All,
// Save x2, Run Match, Reset/Yes/Cancel) all needing the same D-pad wiring;
// factored out rather than repeating the useFocusable boilerplate 7 times.
function NavButton({ id, onClick, disabled, style, children }: {
  id: string; onClick: () => void; disabled?: boolean
  style: React.CSSProperties; children: React.ReactNode
}) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: `settings-btn-${id}`, onEnterPress: onClick, focusable: !disabled })
  return (
    <button ref={ref} data-tv-focused={focused} onClick={onClick} disabled={disabled} style={style}>
      {children}
    </button>
  )
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div style={{ background: 'var(--hds-bg-2)', border: '1px solid oklch(0.22 0.01 286 / 0.6)', borderRadius: 10, overflow: 'hidden' }}>
      <div style={{ padding: '10px 18px', borderBottom: '1px solid oklch(0.22 0.01 286)', background: 'oklch(0.14 0.012 286 / 0.6)' }}>
        <span style={{ fontSize: 10, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{title.toUpperCase()}</span>
      </div>
      <div style={{ padding: '0 18px' }}>{children}</div>
    </div>
  )
}

const inputStyle: React.CSSProperties = {
  padding: '4px 8px', borderRadius: 6,
  border: '1px solid oklch(0.3 0.01 286)',
  background: 'oklch(0.13 0.01 286)', color: 'var(--hds-txt)',
  fontSize: 12, fontFamily: "'JetBrains Mono', monospace",
}

export default observer(function SettingsPage() {
  const [settings, setSettings]   = useState<Settings | null>(null)
  const [saving,   setSaving]     = useState(false)
  const [clearing, setClearing]   = useState(false)
  const [clearMsg, setClearMsg]   = useState<string | null>(null)
  const [error,    setError]      = useState<string | null>(null)
  const [threads,  setThreads]    = useState('')
  const [bufferSize, setBufferSize] = useState('')
  const [castAppId, setCastAppId] = useState('')
  const [castSessions, setCastSessions] = useState<CastSessionInfo[] | null>(null)
  const [revokingCast, setRevokingCast] = useState<string | null>(null)


  const [resetConfirm,  setResetConfirm]  = useState(false)
  const [resetting,     setResetting]     = useState(false)
  const [resetMsg,      setResetMsg]      = useState<string | null>(null)

  const [arr,     setArr]     = useState<ArrConfig>({ sonarr_url: '', sonarr_api_key: '', radarr_url: '', radarr_api_key: '' })
  const [arrSave, setArrSave] = useState<'idle'|'saving'|'ok'|'err'>('idle')

  const [scraperSettings, setScraperSettings] = useState<ScraperSettings | null>(null)
  const [scraperStats,    setScraperStats]    = useState<ScraperStats | null>(null)
  const [scraperDirty,    setScraperDirty]    = useState(false)
  const [scraperSaving,   setScraperSaving]   = useState(false)
  const [scraperSaved,    setScraperSaved]    = useState(false)
  const [matchRunning,    setMatchRunning]    = useState(false)
  const matchPollRef = useRef<ReturnType<typeof setTimeout> | null>(null)

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

  const revokeCastSession = async (sessionId: string) => {
    setRevokingCast(sessionId)
    try { await api.revokeCastSession(sessionId); await loadCastSessions() }
    finally { setRevokingCast(null) }
  }

  useEffect(() => {
    api.getSettings().then(s => {
      setSettings(s)
      setThreads(String(s.sync_threads))
      setBufferSize(String(s.stream_buffer_size))
      setCastAppId(s.cast_app_id)
    }).catch(() => setError('Failed to load settings'))
    loadCastSessions()
    api.getArrConfig().then(setArr).catch(() => {})
    api.getScraperSettings().then(setScraperSettings).catch(() => {})
    api.getScraperStats().then(setScraperStats).catch(() => {})
    // One-time check in case a match kicked off elsewhere (or before this
    // mount) is still running — pollMatchStatus takes over from here on its
    // own if so, and stays quiet otherwise.
    pollMatchStatus()
    return () => { if (matchPollRef.current) clearTimeout(matchPollRef.current) }
  }, [])

  const patch = async (update: Partial<Settings>) => {
    setSaving(true)
    setError(null)
    try {
      const next = await api.updateSettings(update)
      setSettings(next)
      setThreads(String(next.sync_threads))
      setBufferSize(String(next.stream_buffer_size))
      setCastAppId(next.cast_app_id)
      // Keep statusStore in sync immediately so the debug banner reflects the change.
      if ('sync_debug' in update) statusStore.syncDebug = next.sync_debug
      if ('epg_debug'  in update) statusStore.epgDebug  = next.epg_debug
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

  const updateScraperConfig = (source: 'tmdb' | 'tvdb' | 'anidb', field: string, value: string | boolean | number) => {
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

  const tmdb  = scraperSettings?.configs.find(c => c.source === 'tmdb')
  const tvdb  = scraperSettings?.configs.find(c => c.source === 'tvdb')
  const anidb = scraperSettings?.configs.find(c => c.source === 'anidb')

  return (
    <div style={{ maxWidth: 800, display: 'flex', flexDirection: 'column', gap: 24 }}>
      {tourStore.currentStep?.route === '/settings' && <TourSpotlight step={tourStore.currentStep} />}
      <h1 style={{ fontSize: 20, fontWeight: 600, color: 'var(--hds-txt)', margin: 0 }}>Settings</h1>

      {error && (
        <div style={{ padding: '10px 14px', borderRadius: 8, background: 'oklch(0.18 0.06 22 / 0.5)', border: '1px solid oklch(0.4 0.1 22 / 0.4)', fontSize: 12, color: 'oklch(0.75 0.15 22)' }}>
          {error}
        </div>
      )}

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
      </Section>

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
      </Section>

      <Section title="Performance">
        <SettingRow
          label="Sync Worker Threads"
          hint="Parallel connections used when fetching episode metadata from media servers. Range: 1–32."
        >
          <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
            <input
              type="number" min={1} max={32}
              value={threads}
              onChange={e => setThreads(e.target.value)}
              onBlur={applyThreads}
              onKeyDown={e => e.key === 'Enter' && applyThreads()}
              disabled={!settings || saving}
              style={{
                ...inputStyle, width: 60, textAlign: 'center',
              }}
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
            style={{ ...inputStyle, width: 120, cursor: 'pointer' }}
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

      <Section title="EPG Cache">
        <SettingRow
          label="Clear All EPG Caches"
          hint="Deletes all scheduled program rows across every channel. The guide will regenerate on next request."
        >
          <NavButton
            id="clear-epg"
            onClick={clearAllEpg}
            disabled={clearing}
            style={{
              padding: '5px 14px', borderRadius: 6, border: '1px solid oklch(0.4 0.1 22 / 0.6)',
              background: 'oklch(0.18 0.06 22 / 0.4)', color: 'oklch(0.75 0.15 22)',
              fontSize: 12, cursor: clearing ? 'not-allowed' : 'pointer',
              fontFamily: "'JetBrains Mono', monospace", opacity: clearing ? 0.6 : 1,
            }}
          >
            {clearing ? 'Clearing…' : 'Clear All'}
          </NavButton>
        </SettingRow>
        {clearMsg && (
          <div style={{ padding: '10px 0 14px', fontSize: 11, color: 'var(--hds-txt-3)' }}>{clearMsg}</div>
        )}
      </Section>

        <Section title="Stream Settings">
            <SettingRow
                label="Stream Buffer Size (KB)"
                hint="Size of the stream buffer while watching streaming channels. (Requires Restart)"
            >
                <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                    <input
                        type="number" min={1024}
                        value={bufferSize}
                        onChange={e => setBufferSize(e.target.value)}
                        onBlur={applyBuffer}
                        onKeyDown={e => e.key === 'Enter' && applyBuffer()}
                        disabled={!settings || saving}
                        style={{
                            ...inputStyle, width: 120, textAlign: 'right',
                        }}
                    />
                    <span style={{ fontSize: 12, color: 'var(--hds-txt-3)' }}>KB</span>
                </div>
            </SettingRow>
        </Section>

      <Section title="Chromecast">
        <SettingRow
          label="Receiver Application ID"
          hint="From the Google Cast SDK Developer Console, registered against the Pantheon custom receiver's hosted URL. Leave blank to disable the Cast button."
        >
          <input
            style={{ ...inputStyle, width: 200 }}
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
              <ol style={{ margin: 0, paddingLeft: 18 }}>
                <li style={{ marginBottom: 8 }}>You need a domain added to Cloudflare (DNS managed by Cloudflare — free tier is fine).</li>
                <li style={{ marginBottom: 8 }}>Cloudflare Zero Trust dashboard → <b style={{ color: 'var(--hds-txt)' }}>Networks → Tunnels → Create a tunnel</b> → connector type <b style={{ color: 'var(--hds-txt)' }}>Docker</b> → copy the token it gives you.</li>
                <li style={{ marginBottom: 8 }}>In the same wizard, add a <b style={{ color: 'var(--hds-txt)' }}>Public Hostname</b>: the hostname you want (e.g. <code>pantheon.yourdomain.com</code>) → service type <b style={{ color: 'var(--hds-txt)' }}>HTTP</b> → URL <code>hermes:8000</code>. This is also where the hostname itself is configured — nothing in Pantheon's own files needs your hostname hardcoded.</li>
                <li style={{ marginBottom: 8 }}>Put that token in a <code>.env</code> file next to Pantheon's <code>docker-compose.yml</code>:<br /><code>CLOUDFLARE_TUNNEL_TOKEN=eyJ...</code></li>
                <li>Start/restart the stack however you normally do (plain <code>docker compose up -d</code>, or the Unraid Compose Manager UI) — no extra flags needed. Leaving the token blank keeps the container quietly stopped and doesn't affect anything else.</li>
              </ol>
            </HelpSection>
            <HelpSection title="Important">
              <p style={{ margin: 0 }}>
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
          <div style={{ padding: '14px 0', fontSize: 12, color: 'var(--hds-txt-3)' }}>Loading…</div>
        ) : castSessions.length === 0 ? (
          <div style={{ padding: '14px 0', fontSize: 12, color: 'var(--hds-txt-3)' }}>
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
                style={{
                  padding: '5px 12px', borderRadius: 6, cursor: 'pointer',
                  border: '1px solid oklch(0.4 0.15 25)', background: 'transparent',
                  color: 'oklch(0.65 0.2 25)', fontSize: 11, fontFamily: "'JetBrains Mono', monospace",
                  opacity: revokingCast === s.session_id ? 0.5 : 1,
                }}
              >
                {revokingCast === s.session_id ? 'Revoking…' : 'Revoke'}
              </NavButton>
            </SettingRow>
          ))
        )}
      </Section>

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

      <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
        <NavButton
          id="save-arr"
          onClick={async () => {
            setArrSave('saving')
            try { await api.patchArrConfig(arr); setArrSave('ok') }
            catch { setArrSave('err') }
            setTimeout(() => setArrSave('idle'), 2000)
          }}
          disabled={arrSave === 'saving'}
          style={{
            padding: '6px 18px', borderRadius: 6,
            border: '1px solid oklch(0.72 0.18 84 / 0.6)',
            background: 'oklch(0.18 0.06 84 / 0.3)', color: 'oklch(0.88 0.14 84)',
            fontSize: 12, cursor: arrSave === 'saving' ? 'not-allowed' : 'pointer',
            fontFamily: "'JetBrains Mono', monospace", opacity: arrSave === 'saving' ? 0.6 : 1,
          }}
        >
          {arrSave === 'saving' ? 'Saving…' : arrSave === 'ok' ? 'Saved' : arrSave === 'err' ? 'Error' : 'Save Settings'}
        </NavButton>
        <span style={{ fontSize: 11, color: 'var(--hds-txt-3)' }}>
          Used when adding missing media from the import preview.
        </span>
      </div>

      {/* ── Metadata scrapers ───────────────────────────────────────────────── */}

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
            style={{ ...inputStyle, width: 260 }}
            type="password"
            placeholder="xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
            value={tmdb?.api_key ?? ''}
            onChange={e => updateScraperConfig('tmdb', 'api_key', e.target.value)}
          />
        </SettingRow>
        <SettingRow label="Language" hint="e.g. en-US">
          <input
            style={{ ...inputStyle, width: 80 }}
            placeholder="en-US"
            value={tmdb?.language ?? 'en-US'}
            onChange={e => updateScraperConfig('tmdb', 'language', e.target.value)}
          />
        </SettingRow>
        <SettingRow label="Language Weight" hint="Bonus added to score (0.0 to 1.0) if scraper language matches library preference.">
          <input
            type="number" step={0.05} min={0} max={1}
            style={{ ...inputStyle, width: 80 }}
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
            style={{ ...inputStyle, width: 260 }}
            type="password"
            placeholder="xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
            value={tvdb?.api_key ?? ''}
            onChange={e => updateScraperConfig('tvdb', 'api_key', e.target.value)}
          />
        </SettingRow>
        <SettingRow label="Subscriber PIN" hint="Optional; required for some TVDB accounts.">
          <input
            style={{ ...inputStyle, width: 140 }}
            type="password"
            placeholder="optional"
            value={tvdb?.pin ?? ''}
            onChange={e => updateScraperConfig('tvdb', 'pin', e.target.value)}
          />
        </SettingRow>
        <SettingRow label="Language" hint="e.g. eng">
          <input
            style={{ ...inputStyle, width: 80 }}
            placeholder="eng"
            value={tvdb?.language ?? 'eng'}
            onChange={e => updateScraperConfig('tvdb', 'language', e.target.value)}
          />
        </SettingRow>
        <SettingRow label="Language Weight" hint="Bonus added to score (0.0 to 1.0) if scraper language matches library preference.">
          <input
            type="number" step={0.05} min={0} max={1}
            style={{ ...inputStyle, width: 80 }}
            value={tvdb?.language_weight ?? 0.1}
            onChange={e => updateScraperConfig('tvdb', 'language_weight', parseFloat(e.target.value))}
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
            style={{ ...inputStyle, width: 200 }}
            placeholder="myclientname"
            value={anidb?.api_key ?? ''}
            onChange={e => updateScraperConfig('anidb', 'api_key', e.target.value)}
          />
        </SettingRow>
        <SettingRow label="Language Weight" hint="Bonus added to score (0.0 to 1.0) if scraper language matches library preference.">
          <input
            type="number" step={0.05} min={0} max={1}
            style={{ ...inputStyle, width: 80 }}
            value={anidb?.language_weight ?? 0.1}
            onChange={e => updateScraperConfig('anidb', 'language_weight', parseFloat(e.target.value))}
          />
        </SettingRow>
      </Section>

      <Section title="Matching">
        <SettingRow
          label="Confidence Threshold"
          hint="Items below this score go to the Review Queue. 100% = only exact matches are auto-accepted."
        >
          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <input
              type="range" min={0} max={1} step={0.05}
              value={scraperSettings?.match_threshold ?? 0.8}
              onChange={e => updateThreshold(parseFloat(e.target.value))}
              disabled={!scraperSettings}
              style={{ width: 110, accentColor: 'var(--hds-violet)' }}
            />
            <span style={{
              fontFamily: "'JetBrains Mono', monospace", fontSize: 13, fontWeight: 700,
              color: 'var(--hds-violet)', minWidth: 38, textAlign: 'right',
            }}>
              {Math.round((scraperSettings?.match_threshold ?? 0.8) * 100)}%
            </span>
          </div>
        </SettingRow>

        {scraperStats && (
          <div style={{ padding: '14px 0', borderBottom: '1px solid oklch(0.22 0.01 286)', display: 'flex', gap: 10 }}>
            {([
              { label: 'Total',     value: scraperStats.total,     color: 'var(--hds-txt-2)' },
              { label: 'Matched',   value: scraperStats.matched,   color: 'var(--hds-match-green)' },
              { label: 'Uncertain', value: scraperStats.uncertain, color: 'var(--hds-match-amber)' },
              { label: 'Unmatched', value: scraperStats.unmatched, color: 'var(--hds-match-red)' },
              { label: 'Unscraped', value: scraperStats.unscraped, color: 'var(--hds-txt-3)' },
            ] as const).map(({ label, value, color }) => (
              <div key={label} style={{
                flex: 1, padding: '10px 12px', borderRadius: 8, border: '1px solid oklch(0.22 0.01 286)',
                background: 'oklch(0.13 0.01 286)',
              }}>
                <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 16, fontWeight: 700, color }}>{value}</div>
                <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)', letterSpacing: '0.1em', marginTop: 3 }}>{label.toUpperCase()}</div>
              </div>
            ))}
          </div>
        )}

        <div style={{ padding: '14px 0', display: 'flex', gap: 10, alignItems: 'center' }}>
          <NavButton
            id="run-match"
            onClick={runMatch}
            disabled={matchRunning}
            style={{
              padding: '6px 16px', borderRadius: 6, cursor: matchRunning ? 'not-allowed' : 'pointer',
              border: '1px solid oklch(0.3 0.01 286)',
              background: 'transparent',
              color: matchRunning ? 'var(--hds-txt-3)' : 'var(--hds-txt-2)',
              fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
              opacity: matchRunning ? 0.6 : 1,
            }}
          >
            {matchRunning ? '● Running…' : 'Run Match Pass'}
          </NavButton>
        </div>
      </Section>

      <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
        <NavButton
          id="save-scraper"
          onClick={saveScraperSettings}
          disabled={!scraperDirty || scraperSaving}
          style={{
            padding: '6px 18px', borderRadius: 6,
            border: '1px solid oklch(0.72 0.18 84 / 0.6)',
            background: 'oklch(0.18 0.06 84 / 0.3)', color: 'oklch(0.88 0.14 84)',
            fontSize: 12,
            cursor: (!scraperDirty || scraperSaving) ? 'not-allowed' : 'pointer',
            fontFamily: "'JetBrains Mono', monospace",
            opacity: (!scraperDirty || scraperSaving) ? 0.45 : 1,
          }}
        >
          {scraperSaving ? 'Saving…' : scraperSaved ? '✓ Saved' : 'Save Scraper Settings'}
        </NavButton>
      </div>

      {saving && (
        <div style={{ fontSize: 11, color: 'var(--hds-txt-3)' }}>Saving…</div>
      )}

      <Section title="Danger Zone">
        <SettingRow
          label="Reset Library Index"
          hint="Wipes all shows, episodes, movies, and source mappings. Source/library config, channels, and users are kept. The next sync rebuilds everything from scratch."
        >
          {!resetConfirm ? (
            <NavButton
              id="reset-library"
              onClick={() => { setResetConfirm(true); setResetMsg(null) }}
              style={{
                padding: '5px 14px', borderRadius: 6,
                border: '1px solid oklch(0.4 0.1 22 / 0.6)',
                background: 'oklch(0.18 0.06 22 / 0.4)', color: 'oklch(0.75 0.15 22)',
                fontSize: 12, cursor: 'pointer',
                fontFamily: "'JetBrains Mono', monospace",
              }}
            >
              Reset
            </NavButton>
          ) : (
            <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
              <span style={{ fontSize: 11, color: 'oklch(0.75 0.15 22)' }}>Sure?</span>
              <NavButton
                id="reset-library-confirm"
                onClick={resetLibrary}
                disabled={resetting}
                style={{
                  padding: '5px 14px', borderRadius: 6,
                  border: '1px solid oklch(0.55 0.2 22 / 0.8)',
                  background: 'oklch(0.35 0.12 22 / 0.6)', color: 'oklch(0.88 0.12 22)',
                  fontSize: 12, cursor: resetting ? 'not-allowed' : 'pointer',
                  fontFamily: "'JetBrains Mono', monospace",
                  fontWeight: 600, opacity: resetting ? 0.6 : 1,
                }}
              >
                {resetting ? 'Resetting…' : 'Yes, wipe it'}
              </NavButton>
              <NavButton
                id="reset-library-cancel"
                onClick={() => setResetConfirm(false)}
                style={{
                  padding: '5px 10px', borderRadius: 6,
                  border: '1px solid oklch(0.3 0.01 286)',
                  background: 'transparent', color: 'var(--hds-txt-3)',
                  fontSize: 12, cursor: 'pointer',
                  fontFamily: "'JetBrains Mono', monospace",
                }}
              >
                Cancel
              </NavButton>
            </div>
          )}
        </SettingRow>
        {resetMsg && (
          <div style={{ padding: '10px 0 14px', fontSize: 11, color: resetMsg.startsWith('Error') ? 'oklch(0.72 0.18 22)' : 'var(--hds-txt-3)' }}>
            {resetMsg}
          </div>
        )}
      </Section>
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
        style={{
          width: 240, padding: '4px 8px', borderRadius: 6,
          border: '1px solid oklch(0.3 0.01 286)',
          background: 'oklch(0.13 0.01 286)', color: 'var(--hds-txt)',
          fontSize: 12, fontFamily: "'JetBrains Mono', monospace",
        }}
      />
    </SettingRow>
  )
}
