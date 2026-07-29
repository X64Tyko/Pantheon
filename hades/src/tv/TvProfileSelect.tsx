import { useEffect, useRef, useState, type FormEvent } from 'react'
import { useLocation, useNavigate } from 'react-router-dom'
import { FocusContext } from '@noriginmedia/norigin-spatial-navigation'
import {api} from '../api/client'
import { useAuth } from '../auth/AuthContext'
import PinPad from '../auth/PinPad'
import {requiresPasswordForAdminSwitch} from '../auth/profileSwitchPolicy'
import type { User } from '../api/types'
import { useFocusable } from '../nav/useFocusable'
import sharedStyles from '../channel/sharedStyles.module.css'
import styles from './TvProfileSelect.module.css'

// D-pad-navigable counterpart of auth/ProfileSelectPage.tsx — same
// switchProfile/confirmCurrentProfile/login logic exactly, reached instead
// of the desktop picker whenever ProtectedRoute's !profileChosen redirect
// fires from inside the /tv route tree (see that file's comment — this is
// what makes a Cast handoff arriving on /tv with no profile chosen land
// somewhere D-pad-usable instead of an unstyled desktop page). Rendered
// inside TvShell — full-screen, no admin chrome, same as TvHome/TvLibrary.
export function TvProfileSelect() {
  const { user, profiles, switchProfile, confirmCurrentProfile, login, logout } = useAuth()
  const navigate = useNavigate()
  const location = useLocation()

  // Wherever ProtectedRoute's redirect actually interrupted — e.g. a Cast
  // LOAD interceptor already mid-navigation to /player/episode/xyz, not
  // just plain /tv. Falls back to /tv for any other way of landing here.
  const returnTo = (location.state as { from?: { pathname: string; search: string } } | null)?.from
  const returnPath = returnTo ? `${returnTo.pathname}${returnTo.search}` : '/tv'

  const [pinFor,      setPinFor]      = useState<User | null>(null)
  const [passwordFor, setPasswordFor] = useState<User | null>(null)
  const [password,    setPassword]    = useState('')
  const [error, setError] = useState('')
  const [busy,  setBusy]  = useState(false)

    // Same effective (already OR'd with guest_profiles_enabled) value
    // auth/ProfileSelectPage.tsx reads — the server enforces this regardless
    // of what this client sends, but without checking it here first, an
    // admin-with-pin tile would show a PIN prompt that always gets rejected
    // instead of routing straight to the password form.
    const [requireAdminPasswordSwitch, setRequireAdminPasswordSwitch] = useState(false)
    useEffect(() => {
        api.getPublicSettings().then(s => setRequireAdminPasswordSwitch(s.require_admin_password_switch)).catch(() => {
        })
    }, [])

  const { ref: gridRef, focusKey: gridFocusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: 'tv-profiles-grid', trackChildren: true, isFocusBoundary: true,
  })

  const pick = async (u: User, pin?: string) => {
    setError('')
    if (u.user_id === user?.user_id) {
      confirmCurrentProfile()
      navigate(returnPath, { replace: true })
      return
    }
      if (requiresPasswordForAdminSwitch(u.role, u.has_pin, requireAdminPasswordSwitch)) {
          setPassword('');
          setPasswordFor(u);
          return
      }
    if (u.has_pin && pin === undefined) { setPinFor(u); return }
    setBusy(true)
    try {
      await switchProfile(u.user_id, pin)
      navigate(returnPath, { replace: true })
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
      navigate(returnPath, { replace: true })
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
      <div className={styles.heading}>Who's watching?</div>

      {pinFor ? (
        <div className={styles.stage}>
          <TvAvatar u={pinFor} />
          <div className={styles.stageUsername}>{pinFor.username}</div>
          <PinPad busy={busy} error={error} onComplete={pin => pick(pinFor, pin)} />
          <TvBackLink onClick={() => { setPinFor(null); setError('') }} />
        </div>
      ) : passwordFor ? (
        <form onSubmit={submitPassword} className={styles.stage}>
          <TvAvatar u={passwordFor} />
          <div className={styles.stageUsername}>{passwordFor.username}</div>
          <div className={styles.stageNote}>
            Admin profiles need a PIN before they can be switched into — sign in with the password instead.
          </div>
          <input
            ref={passwordInputRef}
            type="password" autoComplete="current-password" disabled={busy}
            value={password} onChange={e => setPassword(e.target.value)}
            className={sharedStyles.input}
          />
          {error && <div className={styles.errorText}>{error}</div>}
          <button
            type="submit" disabled={busy || !password}
            className={sharedStyles.goldBtn}
          >{busy ? '…' : 'Sign in'}</button>
          <TvBackLink onClick={() => { setPasswordFor(null); setPassword(''); setError('') }} />
        </form>
      ) : (
        <FocusContext.Provider value={gridFocusKey}>
        <div ref={gridRef} className={styles.profileGrid}>
          {profiles.map(u => (
            <TvProfileTile key={u.user_id} u={u} currentUserId={user?.user_id} busy={busy} onClick={() => pick(u)} />
          ))}
        </div>
        </FocusContext.Provider>
      )}

      {error && !pinFor && !passwordFor && <div className={styles.pageError}>{error}</div>}

      {!pinFor && !passwordFor && (
        <TvSignOutButton onClick={() => { logout().then(() => navigate('/login')) }} />
      )}
    </div>
  )
}

function TvAvatar({ u }: { u: User }) {
  return (
    <div className={`${styles.avatar} ${u.role === 'admin' ? styles.avatarAdmin : styles.avatarViewer}`}>
      {u.username.slice(0, 1).toUpperCase()}
    </div>
  )
}

function TvProfileTile({ u, currentUserId, busy, onClick }: { u: User; currentUserId?: string; busy: boolean; onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({
    focusKey: `tv-profile-${u.user_id}`, onEnterPress: onClick, forceFocus: u.user_id === currentUserId,
  })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      type="button" onClick={onClick} disabled={busy}
      className={`${styles.profileTile} ${focused ? styles.profileTileFocused : ''}`}
    >
      <div className={styles.avatarWrap}>
        <TvAvatar u={u} />
        {u.has_pin && <div className={styles.pinBadge}>🔒</div>}
      </div>
      <div className={styles.profileName}>
        {u.username}
        {u.user_id === currentUserId && <span className={styles.youTag}>(you)</span>}
      </div>
      {u.restricted && <span className={styles.restrictedTag}>RESTRICTED</span>}
    </button>
  )
}

function TvBackLink({ onClick }: { onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-profiles-back-link', onEnterPress: onClick })
  return (
    <button ref={ref} data-tv-focused={focused} type="button" onClick={onClick} className={styles.backLink}>
      ← back
    </button>
  )
}

function TvSignOutButton({ onClick }: { onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-profiles-sign-out', onEnterPress: onClick })
  return (
    <button ref={ref} data-tv-focused={focused} type="button" onClick={onClick} className={styles.signOutBtn}>
      Sign out completely
    </button>
  )
}
