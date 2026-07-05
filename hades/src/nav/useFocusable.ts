import { useEffect } from 'react'
import {
  useFocusable as useLibraryFocusable,
  type UseFocusableConfig,
  type UseFocusableResult,
} from '@noriginmedia/norigin-spatial-navigation'

// Thin wrapper around the library's own useFocusable — same call signature,
// so every call site just imports from here instead (no other code changes
// needed). Two things the library doesn't do for us, that our old hand-
// rolled hook did:
//
// 1. tabIndex: a plain <div> (every card/tile in this app) has no tabindex
//    by default, so calling .focus() on it (shouldFocusDOMNode: true, set
//    in nav/setup.ts) is a browser no-op — it silently does nothing, no
//    error. That's fine for the library's own internal focus tracking
//    (which is JS-state-based, not real-DOM-focus-based), but it means
//    these elements never actually receive real focus, which in turn means
//    the browser's native "scroll a newly-focused element into view"
//    behavior never engages either.
// 2. scrollIntoView: explicit fallback for the above, and works uniformly
//    for both div-based cards and real buttons alike.
export function useFocusable<P extends object = object, E extends HTMLElement = HTMLElement>(
  config?: UseFocusableConfig<P>,
): UseFocusableResult<E> {
  const result = useLibraryFocusable<P, E>(config)

  useEffect(() => {
    if (result.ref.current) result.ref.current.tabIndex = -1
  })

  useEffect(() => {
    if (!result.focused) return
    // center on both axes, not 'nearest' — 'nearest' settles at a
    // direction-dependent position (clipped shelf title, inconsistent
    // horizontal offset); centering is symmetric and clamps naturally at shelf ends.
    result.ref.current?.scrollIntoView?.({ block: 'center', inline: 'center', behavior: 'smooth' })
  }, [result.focused]) // eslint-disable-line react-hooks/exhaustive-deps

  return result
}
