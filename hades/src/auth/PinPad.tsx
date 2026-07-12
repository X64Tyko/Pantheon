import { useEffect, useRef, useState } from 'react'

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
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10, alignItems: 'center' }}>
      <input
        ref={inputRef}
        type="password" inputMode="numeric" pattern="[0-9]*" autoComplete="off"
        maxLength={6}
        value={pin}
        disabled={busy}
        onChange={e => setPin(e.target.value.replace(/\D/g, '').slice(0, 6))}
        onKeyDown={e => { if (e.key === 'Enter') submit() }}
        placeholder="····"
        style={{
          width: 160, padding: '10px 14px', textAlign: 'center', letterSpacing: '0.5em',
          fontSize: 20, boxSizing: 'border-box',
          background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line)',
          borderRadius: 8, color: 'var(--hds-txt)',
          fontFamily: "'JetBrains Mono', monospace", outline: 'none',
        }}
      />
      {error && (
        <div style={{
          fontSize: 11, color: 'oklch(0.72 0.18 22)', textAlign: 'center', maxWidth: 220,
        }}>
          {error}
        </div>
      )}
      <button
        type="button" disabled={!canSubmit}
        onClick={submit}
        style={{
          padding: '8px 20px', borderRadius: 7, fontSize: 11, fontWeight: 600,
          cursor: canSubmit ? 'pointer' : 'not-allowed',
          fontFamily: "'JetBrains Mono', monospace", letterSpacing: '0.08em',
          background: 'oklch(0.83 0.13 84 / 0.15)', border: '1px solid oklch(0.83 0.13 84 / 0.4)',
          color: 'var(--hds-gold)', opacity: canSubmit ? 1 : 0.5,
        }}
      >
        {busy ? '…' : confirmLabel}
      </button>
    </div>
  )
}
