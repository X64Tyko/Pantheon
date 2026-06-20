// Concurrency-controlled image prefetch queue.
// Concurrency defaults to 6; call setConcurrency() once settings are fetched
// so it matches KAIROS_SYNC_THREADS from the backend.
let CONCURRENCY = 6

export function setConcurrency(n: number) { CONCURRENCY = Math.max(1, n) }

type Entry = { url: string; signal: AbortSignal; resolve: () => void; reject: (e: unknown) => void }

class ImageQueue {
  private active  = 0
  private waiting: Entry[] = []

  /** Prefetch `url` and resolve when the browser has it cached. Cancel via AbortController. */
  load(url: string, signal: AbortSignal): Promise<void> {
    return new Promise((resolve, reject) => {
      if (signal.aborted) { reject(new DOMException('', 'AbortError')); return }
      signal.addEventListener('abort', () => reject(new DOMException('', 'AbortError')), { once: true })
      const entry: Entry = { url, signal, resolve, reject }
      if (this.active < CONCURRENCY) this.run(entry)
      else                           this.waiting.push(entry)
    })
  }

  private run(entry: Entry) {
    if (entry.signal.aborted) { this.pump(); return }
    this.active++
    const img = new Image()
    img.onload  = () => { this.active--; this.pump(); if (!entry.signal.aborted) entry.resolve() }
    img.onerror = () => { this.active--; this.pump(); if (!entry.signal.aborted) entry.reject(new Error('img error')) }
    img.src = entry.url
  }

  private pump() {
    // Drain aborted entries, then start the next live one.
    while (this.waiting.length > 0) {
      const next = this.waiting.shift()!
      if (!next.signal.aborted) { this.run(next); return }
    }
  }
}

export const imageQueue = new ImageQueue()
