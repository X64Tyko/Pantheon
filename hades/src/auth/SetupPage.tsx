import { useState, type FormEvent } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAuth } from './AuthContext'

const inputStyle: React.CSSProperties = {
  width: '100%', padding: '9px 12px', boxSizing: 'border-box',
  background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line)',
  borderRadius: 8, color: 'var(--hds-txt)', fontSize: 13,
  fontFamily: "'JetBrains Mono', monospace", outline: 'none',
}

function Field({ label, hint, children }: { label: string; hint?: string; children: React.ReactNode }) {
  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
      <div>
        <label style={{ fontSize: 11, color: 'var(--hds-txt-3)', letterSpacing: '0.06em' }}>{label}</label>
        {hint && <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginLeft: 8, opacity: 0.7 }}>{hint}</span>}
      </div>
      {children}
    </div>
  )
}

export default function SetupPage() {
  const { completeSetup } = useAuth()
  const navigate          = useNavigate()

  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [confirm,  setConfirm]  = useState('')
  const [error,    setError]    = useState('')
  const [loading,  setLoading]  = useState(false)

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    if (password !== confirm) { setError('Passwords do not match'); return }
    if (password.length < 8)  { setError('Password must be at least 8 characters'); return }
    setError('')
    setLoading(true)
    try {
      await completeSetup(username, password)
      navigate('/sources', { replace: true })
    } catch (err: any) {
      setError(err.message ?? 'Setup failed')
    } finally {
      setLoading(false)
    }
  }

  return (
    <div style={{
      display: 'flex', alignItems: 'center', justifyContent: 'center',
      minHeight: '100vh', background: 'var(--hds-bg)',
      fontFamily: "'JetBrains Mono', monospace",
    }}>
      <div style={{
        width: 380, padding: 32,
        background: 'var(--hds-bg-2)',
        border: '1px solid var(--hds-line-s)',
        borderRadius: 14,
        boxShadow: '0 16px 64px -12px rgba(0,0,0,0.7)',
        animation: 'hds-in 0.2s ease both',
      }}>
        <div style={{ marginBottom: 24, textAlign: 'center' }}>
          <div style={{
            fontFamily: "'Chakra Petch', sans-serif", fontWeight: 800,
            fontSize: 28, letterSpacing: '0.32em', color: 'var(--hds-gold)',
            textShadow: '0 0 22px oklch(0.83 0.13 84 / 0.35)',
          }}>HADES</div>
          <div style={{ fontSize: 9.5, letterSpacing: '0.42em', color: 'var(--hds-violet)', marginTop: 5, opacity: 0.85 }}>
            KAIROS ENGINE
          </div>
          <div style={{ marginTop: 18, fontSize: 12, color: 'var(--hds-txt-2)', lineHeight: 1.5 }}>
            Create your admin account to get started.
          </div>
        </div>

        <form onSubmit={submit} style={{ display: 'flex', flexDirection: 'column', gap: 14 }}>
          <Field label="USERNAME">
            <input
              type="text" autoComplete="username" autoFocus required
              value={username} onChange={e => setUsername(e.target.value)}
              style={inputStyle}
            />
          </Field>
          <Field label="PASSWORD" hint="min. 8 characters">
            <input
              type="password" autoComplete="new-password" required
              value={password} onChange={e => setPassword(e.target.value)}
              style={inputStyle}
            />
          </Field>
          <Field label="CONFIRM PASSWORD">
            <input
              type="password" autoComplete="new-password" required
              value={confirm} onChange={e => setConfirm(e.target.value)}
              style={inputStyle}
            />
          </Field>

          {error && (
            <div style={{
              fontSize: 11, color: 'oklch(0.72 0.18 22)', padding: '8px 10px',
              background: 'oklch(0.55 0.22 22 / 0.12)', borderRadius: 7,
              border: '1px solid oklch(0.55 0.22 22 / 0.3)',
            }}>
              {error}
            </div>
          )}

          <button
            type="submit" disabled={loading}
            style={{
              marginTop: 6, padding: '10px 0',
              background: loading ? 'var(--hds-bg-4)' : 'oklch(0.83 0.13 84 / 0.15)',
              border: '1px solid oklch(0.83 0.13 84 / 0.4)',
              borderRadius: 8, color: 'var(--hds-gold)',
              fontSize: 12, fontWeight: 600, cursor: loading ? 'not-allowed' : 'pointer',
              fontFamily: "'JetBrains Mono', monospace",
              letterSpacing: '0.1em', transition: 'background .12s',
              opacity: loading ? 0.6 : 1,
            }}
          >
            {loading ? 'CREATING ACCOUNT…' : 'CREATE ACCOUNT'}
          </button>
        </form>
      </div>
    </div>
  )
}
