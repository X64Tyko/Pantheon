import { useEffect, useRef } from 'react'
import { navigateByDirection } from '@noriginmedia/norigin-spatial-navigation'
import { triggerBack } from './back'

const INITIAL_REPEAT_DELAY_MS = 500
const REPEAT_INTERVAL_MS      = 150

// Standard Gamepad mapping (not fully uniform across browsers/controllers,
// but reliable enough for `mapping === 'standard'`, which most modern
// controllers report in Chromium).
const DPAD_BUTTONS: Record<number, string> = { 12: 'up', 13: 'down', 14: 'left', 15: 'right' }
const ACTIVATE_BUTTON = 0 // A / Cross
const BACK_BUTTON     = 1 // B / Circle

// The library (norigin-spatial-navigation) only listens for real keyboard
// events — it has no native Gamepad API support despite handling TV remotes
// (which arrive as keyboard events) — so directional moves are delegated to
// its own navigateByDirection(), and "activate" is delegated by dispatching
// a synthetic Enter keydown, since that's the only integration point the
// library exposes for triggering the currently-focused node's onEnterPress.
//
// The Gamepad API itself has no press events — must poll inside
// requestAnimationFrame and hand-roll auto-repeat ourselves (every tick
// while a button is held reports pressed:true, so naively firing per-tick
// would be dozens of moves/second). Mainly serves "PC + USB/Bluetooth
// controller" use; real TV remotes arrive as keyboard events instead.
export function useGamepadNav() {
  const rafRef          = useRef<number | null>(null)
  const pressedAtRef     = useRef<Partial<Record<number, number>>>({})
  const lastRepeatRef     = useRef<Partial<Record<number, number>>>({})
  const prevPressedRef     = useRef<Set<number>>(new Set())

  useEffect(() => {
    const fireButton = (idx: number) => {
      const dir = DPAD_BUTTONS[idx]
      if (dir) { navigateByDirection(dir); return }
      if (idx === ACTIVATE_BUTTON) {
        window.dispatchEvent(new KeyboardEvent('keydown', { key: 'Enter', code: 'Enter', keyCode: 13, which: 13, bubbles: true }))
        return
      }
      if (idx === BACK_BUTTON) { triggerBack(); return }
    }

    const poll = () => {
      const pads = navigator.getGamepads ? navigator.getGamepads() : []
      const now = performance.now()

      for (const pad of pads) {
        if (!pad) continue
        const pressedNow = new Set<number>()

        pad.buttons.forEach((btn, idx) => {
          if (!btn.pressed) return
          pressedNow.add(idx)

          if (!prevPressedRef.current.has(idx)) {
            pressedAtRef.current[idx] = now
            lastRepeatRef.current[idx] = now
            fireButton(idx)
          } else {
            const heldFor    = now - (pressedAtRef.current[idx] ?? now)
            const sinceRepeat = now - (lastRepeatRef.current[idx] ?? now)
            if (heldFor >= INITIAL_REPEAT_DELAY_MS && sinceRepeat >= REPEAT_INTERVAL_MS) {
              lastRepeatRef.current[idx] = now
              fireButton(idx)
            }
          }
        })

        prevPressedRef.current = pressedNow
      }

      rafRef.current = requestAnimationFrame(poll)
    }

    const start = () => {
      if (rafRef.current == null) rafRef.current = requestAnimationFrame(poll)
    }
    const stop = () => {
      if (rafRef.current != null) { cancelAnimationFrame(rafRef.current); rafRef.current = null }
      pressedAtRef.current = {}
      lastRepeatRef.current = {}
      prevPressedRef.current = new Set()
    }

    // A controller already connected before mount won't fire
    // gamepadconnected (browsers only surface that on the actual connect/
    // first-input event) — cheap to also check once on mount.
    if (navigator.getGamepads && Array.from(navigator.getGamepads()).some(Boolean)) start()

    window.addEventListener('gamepadconnected', start)
    window.addEventListener('gamepaddisconnected', stop)
    return () => {
      stop()
      window.removeEventListener('gamepadconnected', start)
      window.removeEventListener('gamepaddisconnected', stop)
    }
  }, [])
}
