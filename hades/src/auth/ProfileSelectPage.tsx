import { useEffect, useRef, useState, type FormEvent } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAuth } from './AuthContext'
import PinPad from './PinPad'
import type { User } from '../api/types'
import styles from './ProfileSelectPage.module.css'

function Avatar({ u }: { u: User }) {
  return (
    <div className={`${styles.avatar} ${u.role === 'admin' ? styles.avatarAdmin : styles.avatarViewer}`}>
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
    <div className={styles.page}>
      <div className={styles.heading}>
        Who's watching?
      </div>

      {pinFor ? (
        <div className={styles.pinStage}>
          <Avatar u={pinFor} />
          <div className={styles.stageUsername}>{pinFor.username}</div>
          <PinPad
            busy={busy}
            error={error}
            onComplete={pin => pick(pinFor, pin)}
          />
          <button
            type="button" onClick={() => { setPinFor(null); setError('') }}
            className={styles.backLink}
          >
            ← back
          </button>
        </div>
      ) : passwordFor ? (
        <form onSubmit={submitPassword} className={styles.passwordStage}>
          <Avatar u={passwordFor} />
          <div className={styles.stageUsername}>{passwordFor.username}</div>
          <div className={styles.stageNote}>
            Admin profiles need a PIN before they can be switched into — sign in with the password instead.
          </div>
          <input
            ref={passwordInputRef}
            type="password" autoComplete="current-password" disabled={busy}
            value={password} onChange={e => setPassword(e.target.value)}
            className={styles.passwordInput}
          />
          {error && (
            <div className={styles.errorTextSmall}>
              {error}
            </div>
          )}
          <button
            type="submit" disabled={busy || !password}
            className={`${styles.submitBtn} ${(busy || !password) ? styles.submitBtnDisabled : styles.submitBtnEnabled}`}
          >
            {busy ? '…' : 'SIGN IN'}
          </button>
          <button
            type="button" onClick={() => { setPasswordFor(null); setPassword(''); setError('') }}
            className={styles.backLink}
          >
            ← back
          </button>
        </form>
      ) : (
        <div className={styles.profileGrid}>
          {profiles.map(u => (
            <button
              key={u.user_id}
              type="button"
              onClick={() => pick(u)}
              disabled={busy}
              className={`${styles.profileTile} ${busy ? styles.profileTileDisabled : styles.profileTileEnabled}`}
            >
              <div className={styles.avatarWrap}>
                <Avatar u={u} />
                {u.has_pin && (
                  <div className={styles.pinBadge}>
                    🔒
                  </div>
                )}
              </div>
              <div className={styles.profileName}>
                {u.username}
                {u.user_id === user?.user_id && <span className={styles.youTag}>(you)</span>}
              </div>
              {u.restricted && (
                <span className={styles.restrictedTag}>
                  RESTRICTED
                </span>
              )}
            </button>
          ))}
        </div>
      )}

      {error && !pinFor && !passwordFor && (
        <div className={styles.pageError}>
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
          className={styles.signOutBtn}
        >
          Sign out completely
        </button>
      )}
    </div>
  )
}
