import { useState, type FormEvent } from 'react'
import { useNavigate } from 'react-router-dom'
import { useAuth } from './AuthContext'
import styles from './SetupPage.module.css'

function Field({ label, hint, children }: { label: string; hint?: string; children: React.ReactNode }) {
  return (
    <div className={styles.fieldWrap}>
      <div className={styles.fieldLabelRow}>
        <label className={styles.fieldLabel}>{label}</label>
        {hint && <span className={styles.fieldHint}>{hint}</span>}
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
    <div className={styles.page}>
      <div className={styles.card}>
        <div className={styles.brandWrap}>
          <div className={styles.brandTitle}>HADES</div>
          <div className={styles.brandSubtitle}>
            KAIROS ENGINE
          </div>
          <div className={styles.introText}>
            Create your admin account to get started.
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
          <Field label="PASSWORD" hint="min. 8 characters">
            <input
              type="password" autoComplete="new-password" required
              value={password} onChange={e => setPassword(e.target.value)}
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
            {loading ? 'CREATING ACCOUNT…' : 'CREATE ACCOUNT'}
          </button>
        </form>
      </div>
    </div>
  )
}
