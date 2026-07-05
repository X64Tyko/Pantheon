import type { TravelRect } from './useTravelingFocus'

// -4px top offset matches the active card's own translateY(-4px) lift —
// offsetTop is always pre-transform, so this keeps the frame visually glued
// to the lifted card without measuring after the transform applies.
export function TravelingFocusFrame({ rect }: { rect: TravelRect | null }) {
  return (
    <div style={{
      position: 'absolute', top: 0, left: 0, zIndex: 1, pointerEvents: 'none',
      width: rect?.width ?? 0, height: rect?.height ?? 0,
      opacity: rect ? 1 : 0,
      transform: `translate(${rect?.left ?? 0}px, ${(rect?.top ?? 0) - 4}px)`,
      borderRadius: 12,
      border: '1.5px solid var(--hds-gold)',
      boxShadow: '0 0 18px 2px oklch(0.83 0.13 84 / 0.4), 0 10px 24px oklch(0 0 0 / 0.45)',
      transition: 'transform .22s cubic-bezier(0.22,1,0.36,1), opacity .18s ease',
    }} />
  )
}
