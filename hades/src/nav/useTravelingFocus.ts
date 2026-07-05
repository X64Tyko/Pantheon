import { useState } from 'react'

export interface TravelRect { left: number; top: number; width: number; height: number }

// One of these per scrollable row. Cards call activate(el) on focus/hover and
// deactivate() on blur/leave; offsetLeft/Top are relative to the row itself
// (its nearest positioned ancestor), so the frame scrolls natively with the
// row's content — no scroll-position tracking needed.
export function useTravelingFocus() {
  const [rect, setRect] = useState<TravelRect | null>(null)

  const activate = (el: HTMLElement | null) => {
    if (!el) { setRect(null); return }
    setRect({ left: el.offsetLeft, top: el.offsetTop, width: el.offsetWidth, height: el.offsetHeight })
  }
  const deactivate = () => setRect(null)

  return { rect, activate, deactivate }
}
