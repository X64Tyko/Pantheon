import { useEffect, useRef, useState, type FormEvent } from 'react'
import { useNavigate } from 'react-router-dom'
import {api} from '../api/client'
import { useAuth } from './AuthContext'
import PinPad from './PinPad'
import {requiresPasswordForAdminSwitch} from './profileSwitchPolicy'
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
    const {
        user,
        profiles,
        switchProfile,
        confirmCurrentProfile,
        applySession,
        logout,
        login,
        resolveLandingPath
    } = useAuth()
  const navigate = useNavigate()

  const [pinFor,      setPinFor]      = useState<User | null>(null)
  const [passwordFor, setPasswordFor] = useState<User | null>(null)
  const [password,    setPassword]    = useState('')
  const [error,  setError]  = useState('')
  const [busy,   setBusy]   = useState(false)

    // "Add Guest" tile — a device that's already past a real login (that's
    // the whole precondition for reaching this picker at all) can mint a
    // brand-new guest profile right here, same idea as Netflix's own
    // "Add Profile" tile, rather than that only ever being reachable from
    // the pre-login page. Read pre-emptively (not gated behind any
    // interaction) so the tile itself only renders when actually enabled —
    // same public-settings fetch LoginPage.tsx already does for the same reason.
    const [guestProfilesEnabled, setGuestProfilesEnabled] = useState(false)
    const [addingGuest, setAddingGuest] = useState(false)
    const [guestName, setGuestName] = useState('')
    const [guestBusy, setGuestBusy] = useState(false)
    const [guestError, setGuestError] = useState('')

    // The already-OR'd-with-guest_profiles_enabled effective value (see
    // ConfigService.cpp's public-settings) — needed before ever attempting a
    // switch, so an admin tile routes straight to the password form instead
    // of showing a PIN prompt that AuthStore::switchProfile would just
    // reject anyway.
    const [requireAdminPasswordSwitch, setRequireAdminPasswordSwitch] = useState(false)

    useEffect(() => {
        api.getPublicSettings().then(s => {
            setGuestProfilesEnabled(s.guest_profiles_enabled)
            setRequireAdminPasswordSwitch(s.require_admin_password_switch)
        }).catch(() => {
        })
    }, [])

    const submitGuest = async (e: FormEvent) => {
        e.preventDefault()
        setGuestError('')
        setGuestBusy(true)
        try {
            const {token, user: guestUser} = await api.createGuest(guestName)
            // Replaces this device's active session with the freshly-created
            // guest's, exactly like picking an existing tile does via
            // switchProfile — the original session that got this device past
            // the real login isn't lost server-side, just no longer the one
            // this device is holding onto (same as switching to any other
            // existing profile from this same picker).
            await applySession(token, guestUser)
            confirmCurrentProfile()
            navigate('/guest-setup', {replace: true})
        } catch (err: any) {
            setGuestError(err.message ?? 'Failed to create guest account')
        } finally {
            setGuestBusy(false)
        }
    }

  const pick = async (u: User, pin?: string) => {
    setError('')
    // Picking whichever profile is already this session's active identity
    // (e.g. the account that just typed its password) needs no PIN and no
    // switchProfile round-trip — it already is that profile. Without this,
    // an admin profile with no PIN set yet could never get past its own
    // tile, since switchProfile always requires one for role=admin.
    if (u.user_id === user?.user_id) {
      confirmCurrentProfile()
        navigate(await resolveLandingPath(u), {replace: true})
      return
    }
    // An admin profile with no PIN configured can never be switched into
    // (AuthStore::switchProfile always denies it) — rather than a dead-end
    // error, fall back to a real password login for that specific account.
      // Same fallback when the server's hardening setting is on, even for an
      // admin that DOES have a PIN — a PIN is a meaningfully weaker credential
      // than the real password, and switchProfile rejects PIN-based admin
      // switches outright in that mode regardless of what we'd send it.
      if (requiresPasswordForAdminSwitch(u.role, u.has_pin, requireAdminPasswordSwitch)) {
          setPassword('');
          setPasswordFor(u);
          return
      }
    if (u.has_pin && pin === undefined) { setPinFor(u); return }
    setBusy(true)
    try {
        const switched = await switchProfile(u.user_id, pin)
        navigate(await resolveLandingPath(switched), {replace: true})
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
        const loggedIn = await login(passwordFor.username, password)
        navigate(await resolveLandingPath(loggedIn), {replace: true})
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
      ) : addingGuest ? (
          <form onSubmit={submitGuest} className={styles.passwordStage}>
              <div className={`${styles.avatar} ${styles.avatarAddGuest}`}>+</div>
              <div className={styles.stageNote}>
                  Pick a name — you'll be able to set a PIN and try out parental-control restrictions next.
              </div>
              <input
                  type="text" autoFocus required maxLength={40} disabled={guestBusy}
                  value={guestName} onChange={e => setGuestName(e.target.value)}
                  className={styles.passwordInput}
              />
              {guestError && (
                  <div className={styles.errorTextSmall}>
                      {guestError}
                  </div>
              )}
              <button
                  type="submit" disabled={guestBusy || !guestName}
                  className={`${styles.submitBtn} ${(guestBusy || !guestName) ? styles.submitBtnDisabled : styles.submitBtnEnabled}`}
              >
                  {guestBusy ? '…' : 'CONTINUE'}
              </button>
              <button
                  type="button" onClick={() => {
                  setAddingGuest(false);
                  setGuestName('');
                  setGuestError('')
              }}
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
            {guestProfilesEnabled && (
                <button
                    type="button"
                    onClick={() => setAddingGuest(true)}
                    disabled={busy}
                    className={`${styles.profileTile} ${busy ? styles.profileTileDisabled : styles.profileTileEnabled}`}
                >
                    <div className={`${styles.avatar} ${styles.avatarAddGuest}`}>+</div>
                    <div className={styles.profileName}>Add Guest</div>
                </button>
            )}
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
