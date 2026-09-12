import { init } from '@noriginmedia/norigin-spatial-navigation'

// Called once at module load (see App.tsx's top-level import) — not a React
// component, this is the library's own service singleton. shouldFocusDOMNode
// makes it call the real element.focus() on every focus change for free
// (screen readers + native :focus-visible), matching what we used to hand-roll.
// preventScroll: nav/useFocusable.ts already does its own explicit
// scrollIntoView — without this, a real <button>'s native focus() call
// (browsers auto-scroll a newly-focused element by default) would compete
// with that instead of the two agreeing on one scroll target.
init({
  shouldFocusDOMNode: true,
  domNodeFocusOptions: { preventScroll: true },
})

// The library binds its own keydown listener to `window` (bubble phase) and,
// for every recognized key — the arrows and Enter — calls preventDefault()/
// stopPropagation() unconditionally, with no awareness of whether the user is
// mid-keystroke in a text field. Left unchecked that hijacks cursor movement
// and Enter inside every <input>/<textarea>/<select>/contentEditable in the
// app, even ones that have nothing to do with spatial nav.
//
// React 18's createRoot delegates all events through a single listener on the
// #root container, which sits BETWEEN any given input and `window` in the
// bubble path. So a bubble-phase listener on `document` (an ancestor of
// #root) is always reached after React has already run the input's own
// onKeyDown, and always reached before the library's `window` listener —
// letting us shield real typing app-wide without touching individual inputs
// or fighting import order.
const TYPING_INPUT_TYPES = new Set([
  'text', 'search', 'email', 'password', 'number', 'tel', 'url',
  'date', 'datetime-local', 'month', 'time', 'week',
])

function isTypingTarget(target: EventTarget | null): boolean {
  if (!(target instanceof HTMLElement)) return false
  if (target.isContentEditable) return true
  if (target.tagName === 'TEXTAREA' || target.tagName === 'SELECT') return true
  return target.tagName === 'INPUT' && TYPING_INPUT_TYPES.has((target as HTMLInputElement).type)
}

document.addEventListener('keydown', event => {
  if (isTypingTarget(event.target)) event.stopPropagation()
})
