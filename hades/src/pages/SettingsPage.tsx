import { useEffect, useState } from 'react'
import { api } from '../api/client'
import type { ArrConfig } from '../api/types'

interface Settings {
  epg_debug:    boolean
  sync_threads: number
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

export default function SettingsPage() {
  const [settings, setSettings]   = useState<Settings | null>(null)
  const [saving,   setSaving]     = useState(false)
  const [clearing, setClearing]   = useState(false)
  const [clearMsg, setClearMsg]   = useState<string | null>(null)
  const [error,    setError]      = useState<string | null>(null)
  const [threads,  setThreads]    = useState('')

  const [arr,     setArr]     = useState<ArrConfig>({ sonarr_url: '', sonarr_api_key: '', radarr_url: '', radarr_api_key: '' })
  const [arrSave, setArrSave] = useState<'idle'|'saving'|'ok'|'err'>('idle')

  useEffect(() => {
    api.getSettings().then(s => {
      setSettings(s)
      setThreads(String(s.sync_threads))
    }).catch(() => setError('Failed to load settings'))
    api.getArrConfig().then(setArr).catch(() => {})
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
                width: 60, padding: '4px 8px', borderRadius: 6, border: '1px solid oklch(0.3 0.01 286)',
                background: 'oklch(0.13 0.01 286)', color: 'var(--hds-txt)', fontSize: 13,
                fontFamily: "'JetBrains Mono', monospace", textAlign: 'center',
              }}
            />
          </div>
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

      {saving && (
        <div style={{ fontSize: 11, color: 'var(--hds-txt-3)' }}>Saving…</div>
      )}
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
