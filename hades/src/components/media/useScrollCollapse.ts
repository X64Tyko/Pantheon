import { useEffect, useRef, useState } from 'react'

/** Shared by MediaDetailHero and TvLibraryDetail: detects whether a
 * `position: sticky` element has actually reached its stuck point, via a
 * zero-height sentinel placed immediately before it in the DOM (same
 * document position as the sticky element's own natural/unstuck top edge).
 * Once the sentinel scrolls out of view, the sticky element has nowhere
 * left to go but stick — this is the standard sentinel/IntersectionObserver
 * technique for "is my sticky element currently stuck," and unlike a
 * guessed scrollTop threshold it stays correct regardless of viewport size
 * or how much scrollable content exists below the sticky element (e.g. a
 * short movie with little metadata/no seasons never falsely reports
 * "collapsed" just because some arbitrary pixel count was scrolled). */
export function useScrollCollapse() {
  const scrollRef   = useRef<HTMLDivElement>(null)
  const sentinelRef = useRef<HTMLDivElement>(null)
  const [collapsed, setCollapsed] = useState(false)
  useEffect(() => {
    const root     = scrollRef.current
    const sentinel = sentinelRef.current
    if (!root || !sentinel) return
    const observer = new IntersectionObserver(
      ([entry]) => setCollapsed(!entry.isIntersecting),
      { root, threshold: 0 },
    )
    observer.observe(sentinel)
    return () => observer.disconnect()
  }, [])
  return { scrollRef, sentinelRef, collapsed }
}
