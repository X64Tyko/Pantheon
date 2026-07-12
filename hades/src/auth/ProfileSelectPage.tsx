import { useEffect, useRef, useState, type FormEvent } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAuth } from './AuthContext'
import PinPad from './PinPad'
import type { User } from '../api/types'

const AVATAR_COLORS: Record<string, string> = {
  admin:  'oklch(0.83 0.13 84)',   // gold, matches ADMIN badge elsewhere
  viewer: 'oklch(0.7 0.13 287)',   // violet
}

function Avatar({ u }: { u: User }) {
  return (
    <div style={{
      width: 96, height: 96, borderRadius: '50%',
      background: `color-mix(in oklch, ${AVATAR_COLORS[u.role]} 22%, var(--hds-bg-3))`,
      border: `2px solid color-mix(in oklch, ${AVATAR_COLORS[u.role]} 55%, transparent)`,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
      fontSize: 34, fontWeight: 700, color: AVATAR_COLORS[u.role],
      fontFamily: "'Chakra Petch', sans-serif",
    }}>
      {u.username.slice(0, 1).toUpperCase()}
    </div>
  )
}

export default function ProfileSelectPage() {
  const { user, profiles, switchProfile, confirmCurrentProfile, logout, login } = useAuth()
  const navigate = useNavigate()

  const [pinFor,      setPinFor]      = useState<User | null>(null)
  const [passwordFor, setPasswordFor] = useState<User | null>(null)
  const [password,    setPassword]    = useState('')
  const [error,  setError]  = useState('')
  const [busy,   setBusy]   = useState(false)

  const pick = async (u: User, pin?: string) => {
    setError('')
    // Picking whichever profile is already this session's active identity
    // (e.g. the account that just typed its password) needs no PIN and no
    // switchProfile round-trip — it already is that profile. Without this,
    // an admin profile with no PIN set yet could never get past its own
    // tile, since switchProfile always requires one for role=admin.
    if (u.user_id === user?.user_id) {
      confirmCurrentProfile()
      navigate('/', { replace: true })
      return
    }
    // An admin profile with no PIN configured can never be switched into
    // (AuthStore::switchProfile always denies it) — rather than a dead-end
    // error, fall back to a real password login for that specific account.
    if (u.role === 'admin' && !u.has_pin) { setPassword(''); setPasswordFor(u); return }
    if (u.has_pin && pin === undefined) { setPinFor(u); return }
    setBusy(true)
    try {
      await switchProfile(u.user_id, pin)
      navigate('/', { replace: true })
    } catch (err: any) {
      setError(err.message ?? 'Failed to switch profile')
    } finally {
      setBusy(false)
    }
  }

  const submitPassword = async (e: FormEvent) => {
    e.preventDefault()
    if (!passwordFor) return
    setError(''); setBusy(true)
    try {
      await login(passwordFor.username, password)
      navigate('/', { replace: true })
    } catch (err: any) {
      setError(err.message ?? 'Login failed')
    } finally {
      setBusy(false)
    }
  }

  const passwordInputRef = useRef<HTMLInputElement>(null)
  useEffect(() => { if (passwordFor) passwordInputRef.current?.focus() }, [passwordFor])

  return (
    <div style={{
      display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center',
      minHeight: '100vh', background: 'var(--hds-bg)', gap: 40,
      fontFamily: "'JetBrains Mono', monospace",
    }}>
      <div style={{
        fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700,
        fontSize: 20, letterSpacing: '0.08em', color: 'var(--hds-txt)',
      }}>
        Who's watching?
      </div>

      {pinFor ? (
        <div style={{
          display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 18,
          animation: 'hds-in 0.15s ease both',
        }}>
          <Avatar u={pinFor} />
          <div style={{ fontSize: 13, color: 'var(--hds-txt-2)' }}>{pinFor.username}</div>
          <PinPad
            busy={busy}
            error={error}
            onComplete={pin => pick(pinFor, pin)}
          />
          <button
            type="button" onClick={() => { setPinFor(null); setError('') }}
            style={{
              background: 'none', border: 'none', color: 'var(--hds-txt-3)',
              fontSize: 11, cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace",
            }}
          >
            ← back
          </button>
        </div>
      ) : passwordFor ? (
        <form onSubmit={submitPassword} style={{
          display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 18,
          animation: 'hds-in 0.15s ease both',
        }}>
          <Avatar u={passwordFor} />
          <div style={{ fontSize: 13, color: 'var(--hds-txt-2)' }}>{passwordFor.username}</div>
          <div style={{ fontSize: 11, color: 'var(--hds-txt-3)', maxWidth: 240, textAlign: 'center' }}>
            Admin profiles need a PIN before they can be switched into — sign in with the password instead.
          </div>
          <input
            ref={passwordInputRef}
            type="password" autoComplete="current-password" disabled={busy}
            value={password} onChange={e => setPassword(e.target.value)}
            style={{
              width: 220, padding: '9px 12px', boxSizing: 'border-box',
              background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line)',
              borderRadius: 8, color: 'var(--hds-txt)', fontSize: 13, textAlign: 'center',
              fontFamily: "'JetBrains Mono', monospace", outline: 'none',
            }}
          />
          {error && (
            <div style={{ fontSize: 11, color: 'oklch(0.72 0.18 22)', textAlign: 'center', maxWidth: 240 }}>
              {error}
            </div>
          )}
          <button
            type="submit" disabled={busy || !password}
            style={{
              padding: '8px 20px', borderRadius: 7, fontSize: 11, fontWeight: 600,
              cursor: (busy || !password) ? 'not-allowed' : 'pointer',
              fontFamily: "'JetBrains Mono', monospace", letterSpacing: '0.08em',
              background: 'oklch(0.83 0.13 84 / 0.15)', border: '1px solid oklch(0.83 0.13 84 / 0.4)',
              color: 'var(--hds-gold)', opacity: (busy || !password) ? 0.5 : 1,
            }}
          >
            {busy ? '…' : 'SIGN IN'}
          </button>
          <button
            type="button" onClick={() => { setPasswordFor(null); setPassword(''); setError('') }}
            style={{
              background: 'none', border: 'none', color: 'var(--hds-txt-3)',
              fontSize: 11, cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace",
            }}
          >
            ← back
          </button>
        </form>
      ) : (
        <div style={{ display: 'flex', gap: 28, flexWrap: 'wrap', justifyContent: 'center', maxWidth: 720 }}>
          {profiles.map(u => (
            <button
              key={u.user_id}
              type="button"
              onClick={() => pick(u)}
              disabled={busy}
              style={{
                display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 10,
                background: 'none', border: 'none', cursor: busy ? 'not-allowed' : 'pointer',
                padding: 8, borderRadius: 10, fontFamily: "'JetBrains Mono', monospace",
              }}
            >
              <div style={{ position: 'relative' }}>
                <Avatar u={u} />
                {u.has_pin && (
                  <div style={{
                    position: 'absolute', bottom: -2, right: -2,
                    width: 26, height: 26, borderRadius: '50%',
                    background: 'var(--hds-bg-2)', border: '1px solid var(--hds-line)',
                    display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 12,
                  }}>
                    🔒
                  </div>
                )}
              </div>
              <div style={{ fontSize: 13, color: 'var(--hds-txt)' }}>
                {u.username}
                {u.user_id === user?.user_id && <span style={{ marginLeft: 5, fontSize: 9, color: 'var(--hds-txt-3)' }}>(you)</span>}
              </div>
              {u.restricted && (
                <span style={{
                  fontSize: 9, letterSpacing: '0.1em', fontWeight: 700, padding: '2px 7px',
                  borderRadius: 4, background: 'oklch(0.6 0.2 22 / 0.12)', color: 'oklch(0.75 0.18 22)',
                  border: '1px solid oklch(0.6 0.2 22 / 0.3)',
                }}>
                  RESTRICTED
                </span>
              )}
            </button>
          ))}
        </div>
      )}

      {error && !pinFor && !passwordFor && (
        <div style={{
          fontSize: 11, color: 'oklch(0.72 0.18 22)', padding: '8px 12px',
          background: 'oklch(0.55 0.22 22 / 0.12)', borderRadius: 7,
          border: '1px solid oklch(0.55 0.22 22 / 0.3)',
        }}>
          {error}
        </div>
      )}

      {/* Full sign-out lives here, not in the main app shell — "exit" from
          inside the app only ever returns to this picker (see Layout.tsx).
          Ending the device's session entirely is a deliberate, separate step. */}
      {!pinFor && !passwordFor && (
        <button
          type="button"
          onClick={() => { logout().then(() => navigate('/login')) }}
          style={{
            background: 'none', border: 'none', color: 'var(--hds-txt-3)',
            fontSize: 11, cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace",
            letterSpacing: '0.05em', opacity: 0.7,
          }}
        >
          Sign out completely
        </button>
      )}
    </div>
  )
}
