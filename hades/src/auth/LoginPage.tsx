import {useEffect, useState, type FormEvent} from 'react'
import {useNavigate, useLocation, Link} from 'react-router-dom'
import {api} from '../api/client'
import { useAuth } from './AuthContext'
import styles from './LoginPage.module.css'

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className={styles.fieldWrap}>
      <label className={styles.fieldLabel}>{label}</label>
      {children}
    </div>
  )
}

export default function LoginPage() {
    const {login, resolveLandingPath, applySession, confirmCurrentProfile} = useAuth()
  const navigate   = useNavigate()
  const location   = useLocation()
    // A real deep link (redirected here by ProtectedRoute) always wins; only
    // when there wasn't one do we fall back to the resolved default landing
    // page instead of a hardcoded path.
    const from = (location.state as any)?.from?.pathname as string | undefined

  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [error,    setError]    = useState('')
  const [loading,  setLoading]  = useState(false)

    // Read pre-login (no session exists yet) — same public-settings route
    // CastProvider already fetches unauthenticated. Off unless the admin has
    // explicitly turned it on (SettingsPage's Guest Access section).
    const [guestProfilesEnabled, setGuestProfilesEnabled] = useState(false)
    const [showGuestForm, setShowGuestForm] = useState(false)
    const [guestName, setGuestName] = useState('')
    const [guestError, setGuestError] = useState('')
    const [guestLoading, setGuestLoading] = useState(false)

    useEffect(() => {
        api.getPublicSettings().then(s => setGuestProfilesEnabled(s.guest_profiles_enabled)).catch(() => {
        })
    }, [])

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setError('')
    setLoading(true)
    try {
        const user = await login(username, password)
        navigate(from ?? await resolveLandingPath(user), {replace: true})
    } catch (err: any) {
      setError(err.message ?? 'Login failed')
    } finally {
      setLoading(false)
    }
  }

    const submitGuest = async (e: FormEvent) => {
        e.preventDefault()
        setGuestError('')
        setGuestLoading(true)
        try {
            const {token, user} = await api.createGuest(guestName)
            await applySession(token, user)
            // A freshly-created guest session IS the profile just picked —
            // there's no "who's watching?" ambiguity to resolve, so skip
            // straight past ProtectedRoute's picker redirect the way
            // ProfileSelectPage does for the tile matching the active
            // session (see confirmCurrentProfile's own doc).
            confirmCurrentProfile()
            navigate('/guest-setup', {replace: true})
        } catch (err: any) {
            setGuestError(err.message ?? 'Failed to create guest account')
        } finally {
            setGuestLoading(false)
        }
    }

  return (
    <div className={styles.page}>
      <div className={styles.card}>
        <div className={styles.brandWrap}>
          <div className={styles.brandTitle}>HADES</div>
          <div className={styles.brandSubtitle}>
            KAIROS ENGINE
          </div>
        </div>

        <form onSubmit={submit} className={styles.form}>
          <Field label="USERNAME">
            <input
              type="text" autoComplete="username" autoFocus required
              value={username} onChange={e => setUsername(e.target.value)}
              className={styles.textInput}
            />
          </Field>
          <Field label="PASSWORD">
            <input
              type="password" autoComplete="current-password" required
              value={password} onChange={e => setPassword(e.target.value)}
              className={styles.textInput}
            />
          </Field>

          {error && (
            <div className={styles.errorBox}>
              {error}
            </div>
          )}

          <button
            type="submit" disabled={loading}
            className={`${styles.submitBtn} ${loading ? styles.submitBtnDisabled : styles.submitBtnEnabled}`}
          >
            {loading ? 'SIGNING IN…' : 'SIGN IN'}
          </button>
        </form>

          {guestProfilesEnabled && !showGuestForm && (
              <button
                  type="button"
                  onClick={() => setShowGuestForm(true)}
                  className={styles.guestLink}
              >
                  Continue as Guest
              </button>
          )}

          {guestProfilesEnabled && showGuestForm && (
              <form onSubmit={submitGuest} className={styles.form}>
                  <Field label="YOUR NAME">
                      <input
                          type="text" autoFocus required maxLength={40}
                          value={guestName} onChange={e => setGuestName(e.target.value)}
                          className={styles.textInput}
                      />
                  </Field>

                  {guestError && (
                      <div className={styles.errorBox}>
                          {guestError}
                      </div>
                  )}

                  <button
                      type="submit" disabled={guestLoading}
                      className={`${styles.submitBtn} ${guestLoading ? styles.submitBtnDisabled : styles.submitBtnEnabled}`}
                  >
                      {guestLoading ? 'CREATING GUEST ACCOUNT…' : 'CONTINUE AS GUEST'}
                  </button>
              </form>
          )}

          <Link to="/about" className={styles.guestLink}>About Pantheon</Link>
      </div>
    </div>
  )
}
