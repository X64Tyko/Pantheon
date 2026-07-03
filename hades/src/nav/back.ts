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
