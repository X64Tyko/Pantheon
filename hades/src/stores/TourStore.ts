import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'
import type { ScraperConfig } from '../api/types'
import { sourceStore, channelStore } from './index'

const SKIPPED_KEY   = 'hds-tour-skipped'
const DISMISSED_KEY = 'hds-tour-dismissed'

export interface TourStep {
  route:       string
  targetAttr:  string // matches a data-tour="..." attribute on the real button
  title:       string
  body:        string
}

export const TOUR_STEPS: TourStep[] = [
  {
    route:      '/sources',
    targetAttr: 'add-source-btn',
    title:      'Add a media source',
    body:       'Connect a Plex, Jellyfin, or local folder source — this is where your shows and movies come from.',
  },
  {
    route:      '/settings',
    targetAttr: 'tmdb-api-key-input',
    title:      'Configure scraper API keys',
    body:       'Paste in a TMDB key (and TVDB/AniDB if you use them), enable it, then Save Scraper Settings — this is what gets synced content real titles and artwork instead of raw filenames.',
  },
  {
    route:      '/sources',
    targetAttr: 'sync-source-btn',
    title:      'Sync your library',
    body:       'Pull your files in from the source you just added — this is what actually populates your library.',
  },
  {
    route:      '/channels',
    targetAttr: 'add-channel-btn',
    title:      'Build your first channel',
    body:       'Create a channel, then add a block and assign it some content — that’s the minimum needed for a working schedule.',
  },
]

function loadSkipped(): Set<number> {
  try {
    const raw = localStorage.getItem(SKIPPED_KEY)
    return new Set(raw ? (JSON.parse(raw) as number[]) : [])
  } catch { return new Set() }
}

// Pure client onboarding-tour state, same localStorage-backed-preference
// pattern as HelpTipsStore — nothing here is worth syncing across devices.
// `done` is refreshed from real backend state (see refreshCompletion), never
// hand-advanced, so the tour can't drift out of sync with what the admin has
// actually set up.
class TourStore {
  done:      boolean[]  = TOUR_STEPS.map(() => false)
  skipped:   Set<number> = loadSkipped()
  dismissed: boolean     = localStorage.getItem(DISMISSED_KEY) === '1'
  loaded:    boolean     = false // avoids a flash of "step 0" before the first refreshCompletion() resolves

  constructor() { makeAutoObservable(this) }

  get currentStepIndex(): number | null {
    for (let i = 0; i < TOUR_STEPS.length; i++) {
      if (!this.done[i] && !this.skipped.has(i)) return i
    }
    return null
  }

  get currentStep(): TourStep | null {
    const i = this.currentStepIndex
    return i == null ? null : TOUR_STEPS[i]
  }

  get active(): boolean {
    return this.loaded && !this.dismissed && this.currentStepIndex != null
  }

  async refreshCompletion() {
    if (this.dismissed) return
    try {
      const [, , scraperSettings, showsPage, moviesPage] = await Promise.all([
        sourceStore.fetchAll(),
        channelStore.fetchAll(),
        api.getScraperSettings(),
        api.getShows({ limit: 1 }),
        api.getMovies({ limit: 1 }),
      ])
      const hasScraperKey = scraperSettings.configs.some((c: ScraperConfig) => c.enabled && c.api_key.trim().length > 0)
      const hasContent    = showsPage.total > 0 || moviesPage.total > 0
      runInAction(() => {
        this.done = [
          sourceStore.sources.length > 0,
          hasScraperKey,
          hasContent,
          channelStore.channels.length > 0,
        ]
        this.loaded = true
      })
    } catch {
      // Admin-only endpoints 403 for non-admin roles — treat as "nothing to
      // show" rather than surfacing an error for a purely cosmetic feature.
      runInAction(() => { this.loaded = true })
    }
  }

  skipStep(i: number) {
    this.skipped.add(i)
    localStorage.setItem(SKIPPED_KEY, JSON.stringify([...this.skipped]))
  }

  dismiss() {
    this.dismissed = true
    localStorage.setItem(DISMISSED_KEY, '1')
  }
}

export const tourStore = new TourStore()
