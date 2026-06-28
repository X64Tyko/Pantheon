import { useEffect, useRef, useState } from 'react'
import { api } from '../api/client'
import type { ArrConfig, ScraperSettings, ScraperStats } from '../api/types'

interface Settings {
  epg_debug:             boolean
  sync_threads:          number
  image_cache_ttl_hours: number
}

function Toggle({ checked, onChange, disabled }: { checked: boolean; onChange: (v: boolean) => void; disabled?: boolean }) {
  return (
    <button
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
    <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 24, padding: '14px 0', borderBottom: '1px solid oklch(0.22 0.01 286)' }}>
      <div>
        <div style={{ fontSize: 13, color: 'var(--hds-txt)', fontWeight: 500 }}>{label}</div>
        {hint && <div style={{ fontSize: 11, color: 'var(--hds-txt-3)', marginTop: 3 }}>{hint}</div>}
      </div>
      <div style={{ flexShrink: 0 }}>{children}</div>
    </div>
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

export default function SettingsPage() {
  const [settings, setSettings]   = useState<Settings | null>(null)
  const [saving,   setSaving]     = useState(false)
  const [clearing, setClearing]   = useState(false)
  const [clearMsg, setClearMsg]   = useState<string | null>(null)
  const [error,    setError]      = useState<string | null>(null)
  const [threads,  setThreads]    = useState('')

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
  const matchPollRef = useRef<ReturnType<typeof setInterval> | null>(null)

  useEffect(() => {
    api.getSettings().then(s => {
      setSettings(s)
      setThreads(String(s.sync_threads))
    }).catch(() => setError('Failed to load settings'))
    api.getArrConfig().then(setArr).catch(() => {})
    api.getScraperSettings().then(setScraperSettings).catch(() => {})
    api.getScraperStats().then(setScraperStats).catch(() => {})
    matchPollRef.current = setInterval(() => {
      api.getMatchStatus().then(s => setMatchRunning(s.running)).catch(() => {})
    }, 3000)
    return () => { if (matchPollRef.current) clearInterval(matchPollRef.current) }
  }, [])

  const patch = async (update: Partial<Settings>) => {
    setSaving(true)
    setError(null)
    try {
      const next = await api.updateSettings(update)
      setSettings(next)
      setThreads(String(next.sync_threads))
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

  const updateScraperConfig = (source: 'tmdb' | 'tvdb', field: string, value: string | boolean) => {
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
    await api.triggerMatch()
    setTimeout(() => {
      setMatchRunning(false)
      api.getScraperStats().then(setScraperStats).catch(() => {})
    }, 4000)
  }

  const tmdb = scraperSettings?.configs.find(c => c.source === 'tmdb')
  const tvdb = scraperSettings?.configs.find(c => c.source === 'tvdb')

  return (
    <div style={{ maxWidth: 640, display: 'flex', flexDirection: 'column', gap: 24 }}>
      <h1 style={{ fontSize: 20, fontWeight: 600, color: 'var(--hds-txt)', margin: 0 }}>Settings</h1>

      {error && (
        <div style={{ padding: '10px 14px', borderRadius: 8, background: 'oklch(0.18 0.06 22 / 0.5)', border: '1px solid oklch(0.4 0.1 22 / 0.4)', fontSize: 12, color: 'oklch(0.75 0.15 22)' }}>
          {error}
        </div>
      )}

      <Section title="Diagnostics">
        <SettingRow
          label="EPG Debug Logging"
          hint="Emits verbose [epg] lines to stdout during schedule projection. Visible in engine logs and docker logs."
        >
          <Toggle
            checked={settings?.epg_debug ?? false}
            disabled={!settings || saving}
            onChange={v => patch({ epg_debug: v })}
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
          <button
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
          </button>
        </SettingRow>
        {clearMsg && (
          <div style={{ padding: '10px 0 14px', fontSize: 11, color: 'var(--hds-txt-3)' }}>{clearMsg}</div>
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
        <button
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
          {arrSave === 'saving' ? 'Saving…' : arrSave === 'ok' ? 'Saved' : arrSave === 'err' ? 'Error' : 'Save Arr Settings'}
        </button>
        <span style={{ fontSize: 11, color: 'var(--hds-txt-3)' }}>
          Used when adding missing media from the import preview.
        </span>
      </div>

      {/* ── Metadata scrapers ───────────────────────────────────────────────── */}

      <Section title="TMDB — The Movie Database">
        <SettingRow label="Enabled" hint="Primary metadata source for movies and shows.">
          <Toggle
            checked={tmdb?.enabled ?? false}
            disabled={!scraperSettings}
            onChange={v => updateScraperConfig('tmdb', 'enabled', v)}
          />
        </SettingRow>
        <SettingRow label="API Key (v3)">
          <input
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
      </Section>

      <Section title="TVDB — TheTVDB">
        <SettingRow label="Enabled" hint="Secondary source; provides TVDB IDs and series data.">
          <Toggle
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
      </Section>

      <Section title="Matching">
        <SettingRow
          label="Confidence Threshold"
          hint="Items below this score go to the Review Queue. 100% = only exact matches are auto-accepted."
        >
          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <input
              type="range" min={0} max={1} step={0.05}
              value={scraperSettings?.match_threshold ?? 1}
              onChange={e => updateThreshold(parseFloat(e.target.value))}
              disabled={!scraperSettings}
              style={{ width: 110, accentColor: 'var(--hds-violet)' }}
            />
            <span style={{
              fontFamily: "'JetBrains Mono', monospace", fontSize: 13, fontWeight: 700,
              color: 'var(--hds-violet)', minWidth: 38, textAlign: 'right',
            }}>
              {Math.round((scraperSettings?.match_threshold ?? 1) * 100)}%
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
          <button
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
          </button>
        </div>
      </Section>

      <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
        <button
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
        </button>
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
            <button
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
            </button>
          ) : (
            <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
              <span style={{ fontSize: 11, color: 'oklch(0.75 0.15 22)' }}>Sure?</span>
              <button
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
              </button>
              <button
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
              </button>
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
}

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
