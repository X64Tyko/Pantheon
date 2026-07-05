import { useState } from 'react'

export interface TravelRect { left: number; top: number; width: number; height: number }

// One of these per scrollable row. Cards call activate(el) on focus/hover and
// deactivate() on blur/leave; offsetLeft/Top are relative to the row itself
// (its nearest positioned ancestor), so the frame scrolls natively with the
// row's content — no scroll-position tracking needed.
//
// rect and active are separate state on purpose: blur (old card) and focus
// (new card) aren't guaranteed to land in the same React batch, so if
// deactivate() cleared rect, the frame would render one frame at (0,0) —
// its fallback position — and visibly snap to the start of the row before
// the next card's activate() moved it back. Deactivating only hides it in
// place; the position stays put until something new claims it.
export function useTravelingFocus() {
  const [rect, setRect]     = useState<TravelRect | null>(null)
  const [active, setActive] = useState(false)

  const activate = (el: HTMLElement | null) => {
    if (!el) return
    setRect({ left: el.offsetLeft, top: el.offsetTop, width: el.offsetWidth, height: el.offsetHeight })
    setActive(true)
  }
  const deactivate = () => setActive(false)

  return { rect, active, activate, deactivate }
}
