import { useState, type FormEvent } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAuth } from './AuthContext'
import styles from './SetPasswordPage.module.css'

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

// Mandatory gate for invite-created accounts (temp-password path) — reached
// via ProtectedRoute's must_change_password check, not a normal nav target.
export default function SetPasswordPage() {
  const { user, setPassword, logout } = useAuth()
  const navigate = useNavigate()

  const [password, setPasswordField] = useState('')
  const [confirm,  setConfirm]       = useState('')
  const [error,    setError]         = useState('')
  const [loading,  setLoading]       = useState(false)

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    if (password !== confirm) { setError('Passwords do not match'); return }
    if (password.length < 8)  { setError('Password must be at least 8 characters'); return }
    setError('')
    setLoading(true)
    try {
      await setPassword(password)
      navigate('/', { replace: true })
    } catch (err: any) {
      setError(err.message ?? 'Failed to set password')
    } finally {
      setLoading(false)
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
          <div className={styles.introText}>
            {user ? `Welcome, ${user.username}. ` : ''}Set a password for your account before continuing.
          </div>
        </div>

        <form onSubmit={submit} className={styles.form}>
          <Field label="NEW PASSWORD" hint="min. 8 characters">
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
            {loading ? 'SAVING…' : 'SET PASSWORD'}
          </button>

          <button
            type="button"
            onClick={() => logout()}
            className={styles.signOutBtn}
          >
            Not you? Sign out
          </button>
        </form>
      </div>
    </div>
  )
}
