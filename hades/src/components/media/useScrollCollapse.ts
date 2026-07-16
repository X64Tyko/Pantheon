import { useEffect, useRef, useState } from 'react'

/** Shared by MediaDetailHero and TvLibraryDetail: tracks scroll position of
 * `scrollRef`'s element and flips `collapsed` once past `thresholdPx`, so
 * both surfaces can drive the same shrink-into-sticky-header transition
 * off one consistent rule. */
export function useScrollCollapse(thresholdPx = 120) {
  const scrollRef = useRef<HTMLDivElement>(null)
  const [collapsed, setCollapsed] = useState(false)
  useEffect(() => {
    const el = scrollRef.current
    if (!el) return
    const onScroll = () => setCollapsed(el.scrollTop > thresholdPx)
    el.addEventListener('scroll', onScroll, { passive: true })
    return () => el.removeEventListener('scroll', onScroll)
  }, [])
  return { scrollRef, collapsed }
}
