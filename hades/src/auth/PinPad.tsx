import { useEffect, useRef, useState } from 'react'
import styles from './PinPad.module.css'

interface PinPadProps {
  onComplete: (pin: string) => void
  error?:     string
  busy?:      boolean
  autoFocus?: boolean
  confirmLabel?: string
}

// Shared numeric PIN entry — used by ProfileSelectPage to verify a profile's
// PIN and by UsersPage to set one. A single masked input rather than
// per-digit boxes since PIN length varies 4-6 per profile and there's no
// fixed count to auto-submit at; the caller decides what a completed pin
// means (verify vs. set) via onComplete.
export default function PinPad({ onComplete, error, busy, autoFocus = true, confirmLabel = 'CONFIRM' }: PinPadProps) {
  const [pin, setPin] = useState('')
  const inputRef = useRef<HTMLInputElement>(null)

  useEffect(() => { if (autoFocus) inputRef.current?.focus() }, [autoFocus])

  const canSubmit = pin.length >= 4 && pin.length <= 6 && !busy

  const submit = () => {
    if (!canSubmit) return
    onComplete(pin)
  }

  return (
    <div className={styles.wrap}>
      <input
        ref={inputRef}
        type="password" inputMode="numeric" pattern="[0-9]*" autoComplete="off"
        maxLength={6}
        value={pin}
        disabled={busy}
        onChange={e => setPin(e.target.value.replace(/\D/g, '').slice(0, 6))}
        onKeyDown={e => { if (e.key === 'Enter') submit() }}
        placeholder="····"
        className={styles.pinInput}
      />
      {error && (
        <div className={styles.errorText}>
          {error}
        </div>
      )}
      <button
        type="button" disabled={!canSubmit}
        onClick={submit}
        className={`${styles.submitBtn} ${canSubmit ? styles.submitBtnEnabled : styles.submitBtnDisabled}`}
      >
        {busy ? '…' : confirmLabel}
      </button>
    </div>
  )
}
