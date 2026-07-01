// Module-level (outside React lifecycle) scroll position store. Route
// navigation unmounts page components, so a plain useRef/useState loses the
// position — this survives remounts for the life of the tab.
const positions = new Map<string, number>()

export function saveScrollPos(key: string, y: number) {
  positions.set(key, y)
}

export function getScrollPos(key: string): number {
  return positions.get(key) ?? 0
}
