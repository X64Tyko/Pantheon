import type { TravelRect } from './useTravelingFocus'
import styles from './TravelingFocusFrame.module.css'

// -4px top offset matches the active card's own translateY(-4px) lift —
// offsetTop is always pre-transform, so this keeps the frame visually glued
// to the lifted card without measuring after the transform applies.
//
// Visibility is driven by `active`, not by whether rect exists — rect holds
// the last known position even while inactive (see useTravelingFocus), so
// hiding never has a position to snap back to.
export function TravelingFocusFrame({ rect, active }: { rect: TravelRect | null; active: boolean }) {
  return (
    <div className={styles.frame} style={{
      width: rect?.width ?? 0, height: rect?.height ?? 0,
      opacity: active && rect ? 1 : 0,
      transform: `translate(${rect?.left ?? 0}px, ${(rect?.top ?? 0) - 4}px)`,
    }} />
  )
}
