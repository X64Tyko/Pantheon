import { useEffect } from 'react'
import { setFocus } from '@noriginmedia/norigin-spatial-navigation'

// Shared with Layout.tsx's sidebar useFocusable({ focusKey: ... }) — kept
// here since this module owns the "what does Backspace do by default" policy.
export const SIDEBAR_FOCUS_KEY = 'SIDEBAR'

// The library has no concept of "back" — Escape/Backspace closing the
// current view is entirely app-specific, so it stays hand-rolled. Only one
// handler is active at a time (the most recently mounted page/view "owns"
// back — e.g. closing a detail view, or Player's onBack), same contract as
// before. With no handler registered (browsing Home's shelves/Guide, not in
// a modal/detail/player), Backspace instead jumps focus to the sidebar —
// saveLastFocusedChild (see Layout.tsx) restores wherever it was left.
export function triggerBack() {
  if (backHandler) { backHandler(); return }
  setFocus(SIDEBAR_FOCUS_KEY)
}

let backHandler: (() => void) | null = null

function isTextInput(el: Element | null): boolean {
  if (!el) return false
  const tag = el.tagName
  return tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || (el as HTMLElement).isContentEditable
}

let listenerAttached = false
function ensureListener() {
  if (listenerAttached) return
  listenerAttached = true
  window.addEventListener('keydown', e => {
    if (e.key !== 'Escape' && e.key !== 'Backspace') return
    if (isTextInput(document.activeElement)) return
    triggerBack()
  })
}

export function useNavBack(onBack: (() => void) | null) {
  useEffect(() => {
    ensureListener()
    backHandler = onBack ?? null
    return () => { if (backHandler === onBack) backHandler = null }
  }, [onBack])
}

// A Cast/TV remote's hardware Back button isn't a DOM keydown JS can freely
// intercept the way Escape/Backspace are above — on Android/Cast browsers it
// commonly triggers the browser's own history.back() at the platform level,
// bypassing any keydown listener entirely. TvShell is reached via
// window.location.replace() (the Cast handoff — see receiverMode.ts), which
// *replaces* the current history entry rather than pushing a new one, so
// there's nothing for that native back gesture to land on — it falls
// straight through past the app's own root and the whole Cast session
// exits/closes. Reported as "Cast back button closes the app, no intuitive
// way to leave details/library/filters."
//
// Fixed with the standard SPA back-trap pattern: push a sentinel history
// entry once, and on every popstate (real or hardware-triggered back),
// re-run the exact same triggerBack() logic Escape/Backspace already use
// (closing an overlay, or Detail/Library's own registered onBack), then
// immediately re-push the sentinel so the next hardware back press is
// caught the same way instead of the stack ever actually running out.
let tvBackGuardInstalled = false
export function useTvBackGuard() {
  useEffect(() => {
    if (tvBackGuardInstalled) return
    tvBackGuardInstalled = true
    history.pushState({ tvBackGuard: true }, '')
    const onPopState = () => {
      triggerBack()
      history.pushState({ tvBackGuard: true }, '')
    }
    window.addEventListener('popstate', onPopState)
    return () => {
      window.removeEventListener('popstate', onPopState)
      tvBackGuardInstalled = false
    }
  }, [])
}
