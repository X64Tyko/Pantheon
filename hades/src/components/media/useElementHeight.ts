import { useEffect, useRef, useState } from 'react'

/** Shared by MediaDetailHero and TvLibraryDetail: tracks an element's
 * rendered height, so the backdrop can shrink to exactly match the locked
 * header's actual content height (which varies per item — a long overview
 * needs more room than a short one) instead of a guessed fixed value. */
export function useElementHeight<T extends HTMLElement>() {
  const ref = useRef<T>(null)
  const [height, setHeight] = useState(0)
  useEffect(() => {
    const el = ref.current
    if (!el) return
    const observer = new ResizeObserver(([entry]) => setHeight(entry.contentRect.height))
    observer.observe(el)
    return () => observer.disconnect()
  }, [])
  return { ref, height }
}
