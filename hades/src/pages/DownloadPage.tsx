import { makeAutoObservable, runInAction } from 'mobx'
import { observer } from 'mobx-react-lite'
import { useEffect, useRef } from 'react'
import { api } from '../api/client'
import type { DownloadJob } from '../api/types'

// ─── Store ───────────────────────────────────────────────────────────────────

class DownloadStore {
  defaultPath: string = ''
  pathDraft:   string = ''
  pathSaving:  boolean = false
  pathError:   string | null = null

  url:          string = ''
  basePath:     string = ''
  showFolder:   string = ''
  seasonFolder: string = 'Season 01'
  submitting:   boolean = false
  submitError:  string | null = null

  pathSuggestions: string[] = []
  libNames:        string[] = []

  jobs:         DownloadJob[] = []
  expandedJobs: Set<string> = new Set()

  private pollTimer: ReturnType<typeof setInterval> | null = null

  constructor() { makeAutoObservable(this) }

  get hasRunning() { return this.jobs.some(j => j.status === 'running' || j.status === 'queued') }

  get assembledPath(): string {
    return [this.basePath, this.showFolder, this.seasonFolder]
      .map(p => p.trim()).filter(Boolean).join('/')
  }

  async loadConfig() {
    try {
      const r = await api.getDownloadConfig()
      runInAction(() => { this.defaultPath = r.path; this.pathDraft = r.path })
    } catch {}
  }

  async saveConfig() {
    this.pathSaving = true
    this.pathError  = null
    try {
      await api.setDownloadConfig(this.pathDraft)
      runInAction(() => { this.defaultPath = this.pathDraft; this.pathSaving = false })
    } catch (e: any) {
      runInAction(() => { this.pathError = e.message; this.pathSaving = false })
    }
  }

  async loadSuggestions() {
    try {
      const libs = await api.getAllLibraries()
      const names = [...new Set(libs.map(l => l.display_name))]
      runInAction(() => {
        this.pathSuggestions = this.defaultPath ? [this.defaultPath] : []
        this.libNames = names
      })
    } catch {}
  }

  async submit() {
    if (!this.url.trim()) return
    this.submitting  = true
    this.submitError = null
    try {
      await api.startDownload(this.url.trim(), this.assembledPath || undefined)
      runInAction(() => {
        this.url          = ''
        this.showFolder   = ''
        this.seasonFolder = 'Season 01'
        this.submitting   = false
      })
      this.loadJobs()
    } catch (e: any) {
      runInAction(() => { this.submitError = e.message; this.submitting = false })
    }
  }

  async loadJobs() {
    try {
      const jobs = await api.getDownloadJobs()
      runInAction(() => { this.jobs = jobs })
    } catch {}
  }

  startPolling() {
    if (this.pollTimer) return
    this.pollTimer = setInterval(() => {
      this.loadJobs().then(() => {
        if (!this.hasRunning) this.stopPolling()
      })
    }, 3000)
  }

  stopPolling() {
    if (this.pollTimer) { clearInterval(this.pollTimer); this.pollTimer = null }
  }

  toggleExpand(id: string) {
    if (this.expandedJobs.has(id)) this.expandedJobs.delete(id)
    else                           this.expandedJobs.add(id)
  }
}

const store = new DownloadStore()

// ─── Component ───────────────────────────────────────────────────────────────

export default observer(function DownloadPage() {
  useEffect(() => {
    store.loadConfig().then(() => store.loadSuggestions())
    store.loadJobs()
    store.startPolling()
    return () => store.stopPolling()
  }, [])

  useEffect(() => {
    if (store.hasRunning) store.startPolling()
  }, [store.hasRunning])

  return (
    <div className="flex flex-col gap-6 max-w-3xl">
      <h1 className="text-xl font-semibold text-zinc-100">Downloads</h1>

      <ConfigPanel />
      <SubmitPanel />
      {store.jobs.length > 0 && <JobList />}
    </div>
  )
})

// ─── Config panel ─────────────────────────────────────────────────────────────

const ConfigPanel = observer(function ConfigPanel() {
  const dirty = store.pathDraft !== store.defaultPath

  return (
    <section className="rounded-lg border border-violet-900/25 bg-zinc-900/50 p-4 space-y-3">
      <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60">
        Default Download Path
      </div>
      <div className="flex gap-2">
        <input
          className="input flex-1 font-mono text-sm"
          placeholder="/path/to/plex/filler"
          value={store.pathDraft}
          onChange={e => runInAction(() => { store.pathDraft = e.target.value })}
          onKeyDown={e => { if (e.key === 'Enter' && dirty) store.saveConfig() }}
        />
        <button
          onClick={() => store.saveConfig()}
          disabled={!dirty || store.pathSaving}
          className="px-3 py-1.5 rounded-lg text-xs font-semibold transition-all
                     bg-amber-500/15 text-amber-400 border border-amber-500/25
                     hover:bg-amber-500/25 disabled:opacity-30 disabled:cursor-not-allowed"
        >
          {store.pathSaving ? 'Saving…' : 'Save'}
        </button>
      </div>
      {store.pathError && (
        <p className="text-xs text-red-400">{store.pathError}</p>
      )}
    </section>
  )
})

// ─── Submit panel ─────────────────────────────────────────────────────────────

const SubmitPanel = observer(function SubmitPanel() {
  const urlRef = useRef<HTMLInputElement>(null)

  const handleSubmit = () => {
    if (!store.url.trim()) return
    if (!store.defaultPath && !store.assembledPath) {
      runInAction(() => { store.submitError = 'Set a download path above or enter a base path.' })
      return
    }
    store.submit()
  }

  return (
    <section className="rounded-lg border border-violet-900/25 bg-zinc-900/50 p-4 space-y-3">
      <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60">
        New Download
      </div>

      <input
        ref={urlRef}
        className="input w-full text-sm"
        placeholder="YouTube URL or playlist URL"
        value={store.url}
        onChange={e => runInAction(() => { store.url = e.target.value; store.submitError = null })}
        onKeyDown={e => { if (e.key === 'Enter') handleSubmit() }}
      />

      <div className="space-y-1.5">
        <div className="text-[9px] uppercase tracking-widest text-violet-500/50">Destination</div>

        {/* Base path */}
        <input
          list="dl-base-paths"
          className="input w-full font-mono text-xs"
          placeholder={store.defaultPath ? `Base path (default: ${store.defaultPath})` : '/media/tvshows'}
          value={store.basePath}
          onChange={e => runInAction(() => { store.basePath = e.target.value })}
          onKeyDown={e => { if (e.key === 'Enter') handleSubmit() }}
        />
        <datalist id="dl-base-paths">
          {store.pathSuggestions.map(p => <option key={p} value={p} />)}
        </datalist>

        {/* Show folder + Season folder */}
        <div className="flex gap-2">
          <input
            list="dl-lib-names"
            className="input flex-1 font-mono text-xs"
            placeholder="Show folder (optional)"
            value={store.showFolder}
            onChange={e => runInAction(() => { store.showFolder = e.target.value })}
            onKeyDown={e => { if (e.key === 'Enter') handleSubmit() }}
          />
          <datalist id="dl-lib-names">
            {store.libNames.map(n => <option key={n} value={n} />)}
          </datalist>
          <input
            className="input w-28 font-mono text-xs"
            placeholder="Season 01"
            value={store.seasonFolder}
            onChange={e => runInAction(() => { store.seasonFolder = e.target.value })}
            onKeyDown={e => { if (e.key === 'Enter') handleSubmit() }}
          />
        </div>

        {/* Assembled path preview + submit */}
        <div className="flex items-center gap-2 pt-0.5">
          <span className="text-[10px] font-mono text-zinc-600 flex-1 truncate">
            {store.assembledPath || store.defaultPath || '—'}
          </span>
          <button
            onClick={handleSubmit}
            disabled={store.submitting || !store.url.trim()}
            className="px-4 py-1.5 rounded-lg text-xs font-semibold transition-all shrink-0
                       bg-violet-600/20 text-violet-300 border border-violet-500/30
                       hover:bg-violet-600/35 disabled:opacity-30 disabled:cursor-not-allowed"
          >
            {store.submitting ? 'Starting…' : '↓ Download'}
          </button>
        </div>
      </div>

      {store.submitError && (
        <p className="text-xs text-red-400">{store.submitError}</p>
      )}
    </section>
  )
})

// ─── Job list ─────────────────────────────────────────────────────────────────

const JobList = observer(function JobList() {
  return (
    <section className="space-y-2">
      <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/60 px-1">
        Jobs
      </div>
      {store.jobs.map(job => <JobRow key={job.id} job={job} />)}
    </section>
  )
})

const JobRow = observer(function JobRow({ job }: { job: DownloadJob }) {
  const expanded = store.expandedJobs.has(job.id)
  const isActive = job.status === 'running' || job.status === 'queued'

  const statusColor = {
    queued:  'text-zinc-400',
    running: 'text-violet-400',
    done:    'text-emerald-400',
    error:   'text-red-400',
  }[job.status]

  const statusDot = {
    queued:  'bg-zinc-500',
    running: 'bg-violet-500 animate-pulse',
    done:    'bg-emerald-500',
    error:   'bg-red-500',
  }[job.status]

  const shortUrl = job.url.replace(/^https?:\/\/(www\.)?/, '')

  return (
    <div className="rounded-lg border border-violet-900/20 bg-zinc-900/50 overflow-hidden">
      {/* Header row */}
      <div
        className="flex items-center gap-3 px-4 py-3 cursor-pointer hover:bg-zinc-800/30 transition-colors"
        onClick={() => store.toggleExpand(job.id)}
      >
        <span className={`w-2 h-2 rounded-full shrink-0 ${statusDot}`} />
        <span className={`text-xs font-mono w-14 shrink-0 ${statusColor}`}>{job.status}</span>
        <span className="text-sm text-zinc-300 flex-1 truncate" title={job.url}>{shortUrl}</span>
        <span className="text-xs text-zinc-600 shrink-0">
          {new Date(job.started_at).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' })}
        </span>
        <span className={`text-zinc-600 text-xs ml-1 transition-transform ${expanded ? 'rotate-180' : ''}`}>▾</span>
      </div>

      {/* Progress bar — shown when running */}
      {isActive && (
        <div className="px-4 pb-2">
          <div className="h-1 rounded-full bg-zinc-800 overflow-hidden">
            <div
              className="h-full rounded-full bg-violet-500 transition-all duration-500"
              style={{ width: `${job.progress}%` }}
            />
          </div>
          <div className="text-[10px] text-zinc-600 mt-1">{job.progress}%</div>
        </div>
      )}

      {/* Log output */}
      {expanded && job.log.length > 0 && (
        <div className="border-t border-zinc-800/60 px-4 py-3">
          <div className="text-[10px] font-semibold uppercase tracking-widest text-violet-500/50 mb-2">
            Output
          </div>
          <pre className="text-[11px] text-zinc-500 font-mono leading-relaxed overflow-x-auto
                          max-h-48 overflow-y-auto scrollbar-dark space-y-0 whitespace-pre-wrap break-all">
            {job.log.join('\n')}
          </pre>
          <div className="text-[10px] text-zinc-700 mt-2 font-mono">→ {job.dest_path}</div>
        </div>
      )}
    </div>
  )
})
