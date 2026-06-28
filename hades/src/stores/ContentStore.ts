import { makeAutoObservable, runInAction } from 'mobx'
import { api } from '../api/client'
import type { Episode, EpisodeGroup, GroupingCandidate, LibraryWithSource, Movie, MovieDetail, Show, ShowDetail } from '../api/types'

export const PAGE_SIZE = 50

export type DetailItem = ShowDetail | MovieDetail

export class ContentStore {
  libraries:   LibraryWithSource[] = []
  selectedLib: string | null = null
  tab:         'shows' | 'movies' = 'shows'

  shows:   Show[]  = []
  movies:  Movie[] = []
  total:   number  = 0
  page:    number  = 1

  query:          string = ''
  filterGenre:    string = ''
  filterYear:     string = ''
  filterRating:   string = ''

  loading: boolean       = false
  error:   string | null = null

  detailItem:     DetailItem | null = null
  detailLoading:  boolean = false
  detailTab:      string  = 'overview'
  episodes:           Episode[] = []
  groupingCandidates: GroupingCandidate[] = []
  groupingLoading:    boolean = false
  confirmedGroups:    EpisodeGroup[] = []
  groupsLoading:      boolean = false
  groupsSaving:       boolean = false

  editDraft:  Partial<ShowDetail & MovieDetail> = {}
  editSaving: boolean = false
  editError:  string | null = null

  constructor() { makeAutoObservable(this) }

  get pageCount()  { return Math.max(1, Math.ceil(this.total / PAGE_SIZE)) }
  get offset()     { return (this.page - 1) * PAGE_SIZE }

  get selectedLibObj(): LibraryWithSource | null {
    return this.libraries.find(l => l.library_id === this.selectedLib) ?? null
  }

  get visibleTabs(): Array<'shows' | 'movies'> {
    const t = this.selectedLibObj?.library_type
    if (t === 'show')  return ['shows']
    if (t === 'movie') return ['movies']
    return ['shows', 'movies']
  }

  get isShowDetail(): boolean {
    return this.detailItem != null && 'show_id' in this.detailItem
  }

  async loadLibraries() {
    try {
      const libs = await api.getAllLibraries()
      runInAction(() => { this.libraries = libs })
    } catch {}
  }

  selectLib(id: string | null) {
    this.selectedLib = id
    this.page = 1
    const lib = this.libraries.find(l => l.library_id === id)
    if (lib?.library_type === 'show')  this.tab = 'shows'
    if (lib?.library_type === 'movie') this.tab = 'movies'
    this.fetch()
  }

  selectTab(t: 'shows' | 'movies') {
    this.tab  = t
    this.page = 1
    this.fetch()
  }

  goToPage(p: number) {
    this.page = Math.max(1, Math.min(p, this.pageCount))
    this.fetch()
  }

  setQuery(q: string) {
    this.query = q
    this.page  = 1
    this.fetch()
  }

  setFilterGenre(g: string) {
    this.filterGenre = g
    this.page = 1
    this.fetch()
  }

  setFilterYear(y: string) {
    this.filterYear = y
    this.page = 1
    this.fetch()
  }

  setFilterRating(r: string) {
    this.filterRating = r
    this.page = 1
    this.fetch()
  }

  clearFilters() {
    this.query       = ''
    this.filterGenre = ''
    this.filterYear  = ''
    this.filterRating = ''
    this.page = 1
    this.fetch()
  }

  async openDetail(item: Show | Movie) {
    this.detailItem    = null
    this.detailLoading = true
    this.detailTab     = 'overview'
    this.episodes      = []
    this.editDraft     = {}
    this.editError     = null

    try {
      if ('show_id' in item) {
        const [detail, eps] = await Promise.all([
          api.getShow(item.show_id),
          api.getEpisodes(item.show_id),
        ])
        runInAction(() => {
          this.detailItem    = detail
          this.episodes      = eps
          this.editDraft     = { ...detail }
          this.detailLoading = false
        })
      } else {
        const detail = await api.getMovie(item.movie_id)
        runInAction(() => {
          this.detailItem    = detail
          this.editDraft     = { ...detail }
          this.detailLoading = false
        })
      }
    } catch {
      runInAction(() => { this.detailLoading = false })
    }
  }

  closeDetail() {
    this.detailItem = null
    this.episodes   = []
  }

  setDetailTab(t: string) {
    this.detailTab = t
    if (t === 'grouping' && this.detailItem && 'show_id' in this.detailItem) {
      this.loadGroupingTab((this.detailItem as ShowDetail).show_id)
    }
  }

  loadGroupingTab(showId: string) {
    this.groupingLoading = true
    this.groupsLoading   = true
    this.groupingCandidates = []
    this.confirmedGroups    = []
    Promise.all([
      api.getGroupingCandidates(showId),
      api.getEpisodeGroups(showId),
    ]).then(([cands, groups]) => runInAction(() => {
      this.groupingCandidates = cands.candidates
      this.confirmedGroups    = groups
      this.groupingLoading    = false
      this.groupsLoading      = false
    })).catch(() => runInAction(() => {
      this.groupingLoading = false
      this.groupsLoading   = false
    }))
  }

  async confirmGroup(showId: string, candidate: GroupingCandidate) {
    this.groupsSaving = true
    try {
      const { group_id } = await api.createEpisodeGroup(showId, { name: candidate.base_title, group_type: 'multipart' })
      await Promise.all(candidate.parts.map(p => api.addGroupMember(showId, group_id, { episode_id: p.episode_id, part_num: p.part_num })))
    } catch {}
    runInAction(() => { this.groupsSaving = false })
    this.loadGroupingTab(showId)
  }

  async deleteGroup(showId: string, groupId: string) {
    this.groupsSaving = true
    try {
      await api.deleteEpisodeGroup(showId, groupId)
    } catch {}
    runInAction(() => { this.groupsSaving = false })
    this.loadGroupingTab(showId)
  }

  patchEdit(field: string, value: unknown) {
    this.editDraft = { ...this.editDraft, [field]: value }
  }

  async saveEdit() {
    if (!this.detailItem) return
    this.editSaving = true
    this.editError  = null
    try {
      if (this.isShowDetail) {
        await api.updateShow((this.detailItem as ShowDetail).show_id, this.editDraft as Partial<ShowDetail>)
        const updated = await api.getShow((this.detailItem as ShowDetail).show_id)
        runInAction(() => {
          this.detailItem = updated
          this.editDraft  = { ...updated }
          this.editSaving = false
        })
      } else {
        await api.updateMovie((this.detailItem as MovieDetail).movie_id, this.editDraft as Partial<MovieDetail>)
        const updated = await api.getMovie((this.detailItem as MovieDetail).movie_id)
        runInAction(() => {
          this.detailItem = updated
          this.editDraft  = { ...updated }
          this.editSaving = false
        })
      }
    } catch (e: any) {
      runInAction(() => { this.editError = e.message; this.editSaving = false })
    }
  }

  async fetch() {
    this.loading = true
    this.error   = null
    const params = {
      limit:          PAGE_SIZE,
      offset:         this.offset,
      library_id:     this.selectedLib ?? undefined,
      q:              this.query       || undefined,
      genre:          this.filterGenre || undefined,
      year:           this.filterYear  ? Number(this.filterYear) : undefined,
      content_rating: this.filterRating || undefined,
    }
    try {
      if (this.tab === 'shows') {
        const r = await api.getShows(params)
        runInAction(() => { this.shows = r.items; this.total = r.total; this.loading = false })
      } else {
        const r = await api.getMovies(params)
        runInAction(() => { this.movies = r.items; this.total = r.total; this.loading = false })
      }
    } catch (e: any) {
      runInAction(() => { this.error = e.message; this.loading = false })
    }
  }
}

export const contentStore = new ContentStore()
