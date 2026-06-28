import { useEffect, useState } from 'react'
import { api } from '../api/client'
import type { ScraperSettings, ScraperStats } from '../api/types'
import { goldBtnStyle, inputStyle } from '../channel/styles'

const sectionLabel: React.CSSProperties = {
  fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, fontWeight: 700,
  letterSpacing: '0.18em', color: 'var(--hds-txt-3)', textTransform: 'uppercase',
  marginBottom: 12,
}

const fieldLabel: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
  color: 'var(--hds-txt-2)', marginBottom: 5, display: 'block',
}

const row: React.CSSProperties = {
  display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 16,
  padding: '14px 0', borderBottom: '1px solid var(--hds-line-s)',
}

const statCard: React.CSSProperties = {
  flex: 1, padding: '14px 16px', borderRadius: 10, border: '1px solid var(--hds-line)',
  background: 'var(--hds-bg-2)', display: 'flex', flexDirection: 'column', gap: 4,
}

export default function ScrapersPage() {
  const [settings, setSettings] = useState<ScraperSettings | null>(null)
  const [stats,    setStats]    = useState<ScraperStats | null>(null)
  const [dirty,    setDirty]    = useState(false)
  const [saving,   setSaving]   = useState(false)
  const [running,  setRunning]  = useState(false)
  const [saved,    setSaved]    = useState(false)

  useEffect(() => {
    api.getScraperSettings().then(setSettings)
    api.getScraperStats().then(setStats)
    const poll = setInterval(() => api.getMatchStatus().then(s => setRunning(s.running)), 3000)
    return () => clearInterval(poll)
  }, [])

  const updateConfig = (source: 'tmdb' | 'tvdb', field: string, value: string | boolean) => {
    if (!settings) return
    setSettings(prev => {
      if (!prev) return prev
      return {
        ...prev,
        configs: prev.configs.map(c => c.source === source ? { ...c, [field]: value } : c),
      }
    })
    setDirty(true)
  }

  const updateThreshold = (v: number) => {
    if (!settings) return
    setSettings(prev => prev ? { ...prev, match_threshold: v } : prev)
    setDirty(true)
  }

  const save = async () => {
    if (!settings) return
    setSaving(true)
    await api.patchScraperSettings(settings)
    setSaving(false)
    setDirty(false)
    setSaved(true)
    setTimeout(() => setSaved(false), 2000)
    api.getScraperStats().then(setStats)
  }

  const runMatch = async () => {
    setRunning(true)
    await api.triggerMatch()
    setTimeout(() => {
      setRunning(false)
      api.getScraperStats().then(setStats)
    }, 4000)
  }

  const tmdb = settings?.configs.find(c => c.source === 'tmdb')
  const tvdb = settings?.configs.find(c => c.source === 'tvdb')

  if (!settings) {
    return (
      <div style={{ padding: 32 }}>
        <div className="hds-skeleton" style={{ height: 200, borderRadius: 10 }} />
      </div>
    )
  }

  return (
    <div style={{ maxWidth: 680, padding: '28px 32px' }}>
      <h1 style={{
        fontFamily: "'Chakra Petch', sans-serif", fontSize: 20, fontWeight: 700,
        color: 'var(--hds-txt)', margin: '0 0 6px',
      }}>Scrapers</h1>
      <p style={{
        fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
        color: 'var(--hds-txt-3)', margin: '0 0 28px', lineHeight: 1.6,
      }}>
        Configure metadata sources and automatic match confidence settings.
        Items below the threshold are pushed to the Review Queue for manual approval.
      </p>

      {/* Stats row */}
      {stats && (
        <div style={{ display: 'flex', gap: 10, marginBottom: 28 }}>
          {([
            { label: 'Total',     value: stats.total,     color: 'var(--hds-txt-2)' },
            { label: 'Matched',   value: stats.matched,   color: 'var(--hds-match-green)' },
            { label: 'Uncertain', value: stats.uncertain, color: 'var(--hds-match-amber)' },
            { label: 'Unmatched', value: stats.unmatched, color: 'var(--hds-match-red)' },
            { label: 'Unscraped', value: stats.unscraped, color: 'var(--hds-txt-3)' },
          ] as const).map(({ label, value, color }) => (
            <div key={label} style={statCard}>
              <div style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 18, fontWeight: 700, color,
              }}>{value}</div>
              <div style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
                color: 'var(--hds-txt-3)', letterSpacing: '0.1em',
              }}>{label.toUpperCase()}</div>
            </div>
          ))}
        </div>
      )}

      {/* Match threshold */}
      <div style={{ marginBottom: 28 }}>
        <div style={sectionLabel}>Match Threshold</div>
        <div style={{
          padding: '16px 20px', borderRadius: 10, border: '1px solid var(--hds-line)',
          background: 'var(--hds-bg-2)',
        }}>
          <div style={row}>
            <div>
              <span style={{ ...fieldLabel, marginBottom: 2 }}>Confidence threshold</span>
              <div style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)',
              }}>
                Items below this score are held in the Review Queue.
                100% = everything needs verification except trusted source IDs.
              </div>
            </div>
            <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexShrink: 0 }}>
              <input
                type="range" min={0} max={1} step={0.05}
                value={settings.match_threshold}
                onChange={e => updateThreshold(parseFloat(e.target.value))}
                style={{ width: 110, accentColor: 'var(--hds-violet)' }}
              />
              <span style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 13, fontWeight: 700,
                color: 'var(--hds-violet)', minWidth: 38, textAlign: 'right',
              }}>
                {Math.round(settings.match_threshold * 100)}%
              </span>
            </div>
          </div>
        </div>
      </div>

      {/* TMDB */}
      <div style={{ marginBottom: 28 }}>
        <div style={sectionLabel}>TMDB — The Movie Database</div>
        <div style={{
          padding: '16px 20px', borderRadius: 10, border: '1px solid var(--hds-line)',
          background: 'var(--hds-bg-2)', display: 'flex', flexDirection: 'column',
        }}>
          <div style={row}>
            <span style={fieldLabel}>Enabled</span>
            <Toggle
              checked={tmdb?.enabled ?? false}
              onChange={v => updateConfig('tmdb', 'enabled', v)}
            />
          </div>
          <div style={{ paddingTop: 12 }}>
            <label style={fieldLabel}>API Key (v3)</label>
            <input
              style={{ ...inputStyle, width: '100%', boxSizing: 'border-box' }}
              type="password"
              placeholder="xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
              value={tmdb?.api_key ?? ''}
              onChange={e => updateConfig('tmdb', 'api_key', e.target.value)}
            />
          </div>
          <div style={{ paddingTop: 12 }}>
            <label style={fieldLabel}>Language</label>
            <input
              style={{ ...inputStyle, width: 100 }}
              placeholder="en-US"
              value={tmdb?.language ?? 'en-US'}
              onChange={e => updateConfig('tmdb', 'language', e.target.value)}
            />
          </div>
        </div>
      </div>

      {/* TVDB */}
      <div style={{ marginBottom: 28 }}>
        <div style={sectionLabel}>TVDB — TheTVDB</div>
        <div style={{
          padding: '16px 20px', borderRadius: 10, border: '1px solid var(--hds-line)',
          background: 'var(--hds-bg-2)', display: 'flex', flexDirection: 'column',
        }}>
          <div style={row}>
            <span style={fieldLabel}>Enabled</span>
            <Toggle
              checked={tvdb?.enabled ?? false}
              onChange={v => updateConfig('tvdb', 'enabled', v)}
            />
          </div>
          <div style={{ paddingTop: 12 }}>
            <label style={fieldLabel}>API Key (v4 project key)</label>
            <input
              style={{ ...inputStyle, width: '100%', boxSizing: 'border-box' }}
              type="password"
              placeholder="xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
              value={tvdb?.api_key ?? ''}
              onChange={e => updateConfig('tvdb', 'api_key', e.target.value)}
            />
          </div>
          <div style={{ paddingTop: 12 }}>
            <label style={fieldLabel}>PIN (optional subscriber PIN)</label>
            <input
              style={{ ...inputStyle, width: 200 }}
              type="password"
              placeholder="optional"
              value={tvdb?.pin ?? ''}
              onChange={e => updateConfig('tvdb', 'pin', e.target.value)}
            />
          </div>
          <div style={{ paddingTop: 12 }}>
            <label style={fieldLabel}>Language</label>
            <input
              style={{ ...inputStyle, width: 100 }}
              placeholder="eng"
              value={tvdb?.language ?? 'eng'}
              onChange={e => updateConfig('tvdb', 'language', e.target.value)}
            />
          </div>
        </div>
      </div>

      {/* Actions */}
      <div style={{ display: 'flex', gap: 10, alignItems: 'center' }}>
        <button
          onClick={save}
          disabled={!dirty || saving}
          style={{
            ...goldBtnStyle,
            opacity: !dirty || saving ? 0.45 : 1,
            cursor: !dirty || saving ? 'not-allowed' : 'pointer',
          }}
        >
          {saving ? 'Saving…' : saved ? '✓ Saved' : 'Save Settings'}
        </button>
        <button
          onClick={runMatch}
          disabled={running}
          style={{
            padding: '8px 18px', borderRadius: 8, cursor: running ? 'not-allowed' : 'pointer',
            border: '1px solid var(--hds-line)',
            background: 'transparent',
            color: running ? 'var(--hds-txt-3)' : 'var(--hds-txt-2)',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
            opacity: running ? 0.6 : 1,
          }}
        >
          {running ? '● Running match…' : 'Run Match Pass'}
        </button>
      </div>
    </div>
  )
}

function Toggle({ checked, onChange }: { checked: boolean; onChange: (v: boolean) => void }) {
  return (
    <button
      onClick={() => onChange(!checked)}
      style={{
        position: 'relative', width: 38, height: 22, borderRadius: 11,
        border: 'none', cursor: 'pointer', padding: 0, flexShrink: 0,
        background: checked ? 'var(--hds-violet)' : 'var(--hds-bg-3)',
        transition: 'background .15s',
      }}
    >
      <span style={{
        position: 'absolute', top: 3, left: checked ? 19 : 3,
        width: 16, height: 16, borderRadius: '50%', background: '#fff',
        transition: 'left .15s',
      }} />
    </button>
  )
}
