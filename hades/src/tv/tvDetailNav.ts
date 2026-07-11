// Plain module state (not React state) so it survives the full unmount that
// happens crossing routes — TvHome/TvLibrary → TvLibraryDetail are sibling
// routes, not parent/child, so nothing here can live in either page's own
// component state. Same singleton-module pattern as nav/back.ts's backHandler.
//
// Set right before navigating into an item's Detail route (by whichever
// shelf/grid card was clicked), so that:
//   1. TvLibraryDetail's Back returns to wherever the user actually came
//      from (Home or Library) instead of a hardcoded page — see peekReturnTo.
//   2. That origin page can restore focus to the exact card that was
//      clicked instead of landing on its own default focus target — see
//      consumeReturnFocusKey.
export interface TvDetailReturn {
  returnTo: string
  focusKey: string
}

let pending: TvDetailReturn | null = null

export function rememberDetailReturn(entry: TvDetailReturn) {
  pending = entry
}

// Read-only — TvLibraryDetail needs this at its own mount to know its Back
// target, without consuming it (the origin page still needs it afterward).
export function peekDetailReturnTo(): string {
  return pending?.returnTo ?? '/tv/library'
}

// Consumed exactly once by the origin page's mount. expectedPath guards
// against a stale entry (e.g. Detail was left via Play → /player instead of
// Back) wrongly restoring focus on some later, unrelated visit to that path.
export function consumeReturnFocusKey(expectedPath: string): string | null {
  if (!pending || pending.returnTo !== expectedPath) return null
  const key = pending.focusKey
  pending = null
  return key
}

// Safety net for any Detail exit that isn't Back (e.g. pressing Play) —
// called from TvLibraryDetail's unmount cleanup. A no-op if Back already
// consumed the entry (the legitimate path always does, before this fires:
// React runs the new route's render, which calls consumeReturnFocusKey,
// before it runs this cleanup effect for the route being left).
export function clearPendingDetailReturn() {
  pending = null
}
