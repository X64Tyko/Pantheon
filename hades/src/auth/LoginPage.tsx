import { useState, type FormEvent } from 'react'
import { useNavigate, useLocation } from 'react-router-dom'
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
  const { login }  = useAuth()
  const navigate   = useNavigate()
  const location   = useLocation()
  const from       = (location.state as any)?.from?.pathname ?? '/sources'

  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [error,    setError]    = useState('')
  const [loading,  setLoading]  = useState(false)

  const submit = async (e: FormEvent) => {
    e.preventDefault()
    setError('')
    setLoading(true)
    try {
      await login(username, password)
      navigate(from, { replace: true })
    } catch (err: any) {
      setError(err.message ?? 'Login failed')
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
      </div>
    </div>
  )
}
