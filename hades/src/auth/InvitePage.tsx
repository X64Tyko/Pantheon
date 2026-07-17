import { useEffect, useState, type FormEvent } from 'react'
import { useNavigate, useParams } from 'react-router-dom'
import { api } from '../api/client'
import { useAuth } from './AuthContext'
import styles from './InvitePage.module.css'

function Field({ label, hint, children }: { label: string; hint?: string; children: React.ReactNode }) {
  return (
    <div className={styles.fieldWrap}>
      <div>
        <label className={styles.fieldLabel}>{label}</label>
        {hint && <span className={styles.fieldHint}>{hint}</span>}
      </div>
      {children}
    </div>
  )
}

// Unauthenticated claim page for an email-delivered invite link — the only
// way in is knowing the unguessable token in the URL (see
// AuthStore::claimInvite / IMediaSource discovery → SourceService import).
export default function InvitePage() {
  const { token } = useParams<{ token: string }>()
  const { applySession } = useAuth()
  const navigate = useNavigate()

  const [status,   setStatus]   = useState<'loading' | 'valid' | 'invalid'>('loading')
  const [username, setUsername] = useState('')
  const [password, setPasswordField] = useState('')
  const [confirm,  setConfirm]  = useState('')
  const [error,    setError]    = useState('')
  const [loading,  setLoading]  = useState(false)

  useEffect(() => {
    if (!token) { setStatus('invalid'); return }
    api.getInvite(token)
      .then(r => { setUsername(r.username); setStatus('valid') })
      .catch(() => setStatus('invalid'))
  }, [token])

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    if (!token) return
    if (password !== confirm) { setError('Passwords do not match'); return }
    if (password.length < 8)  { setError('Password must be at least 8 characters'); return }
    setError('')
    setLoading(true)
    try {
      const { token: sessionToken, user } = await api.claimInvite(token, password)
      await applySession(sessionToken, user)
      navigate('/', { replace: true })
    } catch (err: any) {
      setError(err.message ?? 'Failed to claim invite')
    } finally {
      setLoading(false)
    }
  }

  if (status === 'loading') {
    return <div className={styles.page} />
  }

  if (status === 'invalid') {
    return (
      <div className={styles.page}>
        <div className={styles.card}>
          <div className={styles.invalidCentered}>
            <div className={styles.brandTitle}>HADES</div>
            <div className={styles.invalidBox}>
              This invite link is invalid, expired, or has already been used. Ask an admin to send a new one.
            </div>
          </div>
        </div>
      </div>
    )
  }

  return (
    <div className={styles.page}>
      <div className={styles.card}>
        <div className={styles.brandWrap}>
          <div className={styles.brandTitle}>HADES</div>
          <div className={styles.brandSubtitle}>
            KAIROS ENGINE
          </div>
          <div className={styles.introText}>
            Welcome, <strong className={styles.introTextStrong}>{username}</strong>. Choose a password to activate your account.
          </div>
        </div>

        <form onSubmit={submit} className={styles.form}>
          <Field label="PASSWORD" hint="min. 8 characters">
            <input
              type="password" autoComplete="new-password" autoFocus required
              value={password} onChange={e => setPasswordField(e.target.value)}
              className={styles.textInput}
            />
          </Field>
          <Field label="CONFIRM PASSWORD">
            <input
              type="password" autoComplete="new-password" required
              value={confirm} onChange={e => setConfirm(e.target.value)}
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
            {loading ? 'ACTIVATING…' : 'ACTIVATE ACCOUNT'}
          </button>
        </form>
      </div>
    </div>
  )
}
