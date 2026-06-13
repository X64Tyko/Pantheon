import { observer } from 'mobx-react-lite'
import { useEffect, useState } from 'react'
import { api } from '../api/client'
import { sourceStore } from '../stores'

type TestState = 'idle' | 'testing' | 'ok' | 'failed'

const NEEDS_TOKEN = ['plex', 'jellyfin', 'emby']

export default observer(function SourcesPage() {
  const store = sourceStore

  // ── Add-source form ────────────────────────────────────────────────────────
  const [showAdd, setShowAdd]   = useState(false)
  const [form, setForm]         = useState({ source_id: '', source_type: 'plex', display_name: '', base_url: '', token: '' })
  const [testState, setTest]    = useState<TestState>('idle')
  const [testError, setTestErr] = useState('')

  const updateForm = (patch: Partial<typeof form>) => {
    setForm(prev => ({ ...prev, ...patch }))
    if ('source_type' in patch || 'base_url' in patch || 'token' in patch) {
      setTest('idle'); setTestErr('')
    }
  }

  const runTest = async () => {
    setTest('testing'); setTestErr('')
    try {
      const r = await api.testSource({ source_type: form.source_type, base_url: form.base_url, token: form.token })
      r.ok ? setTest('ok') : (setTest('failed'), setTestErr(r.error ?? 'Connection failed'))
    } catch (e: any) {
      setTest('failed'); setTestErr(e.message)
    }
  }

  const addSource = async () => {
    await store.addSource({ source_id: form.source_id, source_type: form.source_type as any, display_name: form.display_name, base_url: form.base_url })
    if (form.token) await store.setCredentials(form.source_id, form.token)
    setShowAdd(false)
    setForm({ source_id: '', source_type: 'plex', display_name: '', base_url: '', token: '' })
    setTest('idle'); setTestErr('')
  }

  const cancelAdd = () => { setShowAdd(false); setTest('idle'); setTestErr('') }

  const needsToken = NEEDS_TOKEN.includes(form.source_type)
  const canTest    = needsToken ? (!!form.base_url && !!form.token) : !!form.base_url
  const saveReady  = !!form.source_id && !!form.display_name && (!needsToken || testState === 'ok')

  // ── Add-library form ───────────────────────────────────────────────────────
  const [showAddLib, setShowAddLib] = useState(false)
  const [libForm, setLibForm]       = useState({ external_lib_id: '', display_name: '', library_type: 'show' as 'show' | 'movie' | 'mixed' })

  const addLib = async () => {
    if (!store.selectedId) return
    await store.addLibrary(store.selectedId, libForm.external_lib_id, libForm.display_name, libForm.library_type)
    setShowAddLib(false)
    setLibForm({ external_lib_id: '', display_name: '', library_type: 'show' })
  }

  // ── Credential editor ──────────────────────────────────────────────────────
  const [editingCreds, setEditingCreds] = useState(false)
  const [credToken, setCredToken]       = useState('')

  useEffect(() => { setEditingCreds(false); setCredToken('') }, [store.selectedId])
  useEffect(() => { store.fetchAll() }, [])

  // ── Render ─────────────────────────────────────────────────────────────────
  return (
    <div className="space-y-5">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold text-zinc-100">Media Sources</h1>
        <button onClick={() => setShowAdd(v => !v)} className="btn-primary">
          + Add Source
        </button>
      </div>

      {store.error && (
        <div className="text-red-400 text-sm bg-red-950/30 border border-red-900/40 rounded-lg p-3">
          {store.error}
        </div>
      )}

      {/* ── Add source form ─────────────────────────────────────────────── */}
      {showAdd && (
        <div className="card p-4 space-y-4">
          <h2 className="section-label">New Source</h2>
          <div className="grid grid-cols-2 gap-3">
            <input
              placeholder="source_id  (e.g. plex_home)"
              value={form.source_id}
              onChange={e => updateForm({ source_id: e.target.value })}
              className="input"
            />
            <select
              value={form.source_type}
              onChange={e => updateForm({ source_type: e.target.value })}
              className="input"
            >
              {store.sourceTypes.map(t => (
                <option key={t.type} value={t.type} disabled={!t.supported}>
                  {t.display_name}{!t.supported ? ' (coming soon)' : ''}
                </option>
              ))}
            </select>
            <input
              placeholder="Display name"
              value={form.display_name}
              onChange={e => updateForm({ display_name: e.target.value })}
              className="input"
            />
            <input
              placeholder="Base URL  (e.g. http://plex:32400)"
              value={form.base_url}
              onChange={e => updateForm({ base_url: e.target.value })}
              className="input"
            />
            {needsToken && (
              <input
                placeholder="Auth token"
                type="password"
                value={form.token}
                onChange={e => updateForm({ token: e.target.value })}
                className="input col-span-2"
              />
            )}
          </div>

          {needsToken && (
            <div className="flex items-center gap-3">
              <button
                onClick={runTest}
                disabled={!canTest || testState === 'testing'}
                className="btn-secondary disabled:opacity-40"
              >
                {testState === 'testing' ? 'Testing…' : 'Test Connection'}
              </button>
              {testState === 'ok' && (
                <span className="text-xs text-emerald-400">✓ Connected</span>
              )}
              {testState === 'failed' && (
                <span className="text-xs text-red-400 truncate max-w-xs" title={testError}>
                  ✗ {testError}
                </span>
              )}
            </div>
          )}

          <div className="flex gap-2 pt-1">
            <button
              onClick={addSource}
              disabled={!saveReady}
              title={needsToken && testState !== 'ok' ? 'Run a successful connection test first' : undefined}
              className="btn-primary disabled:opacity-40 disabled:cursor-not-allowed"
            >
              Save
            </button>
            <button onClick={cancelAdd} className="btn-ghost">Cancel</button>
          </div>
        </div>
      )}

      <div className="grid grid-cols-2 gap-5">
        {/* Source list */}
        <div className="space-y-2">
          {store.sources.length === 0 && !store.loading && (
            <p className="text-zinc-600 text-sm">No sources configured.</p>
          )}
          {store.sources.map(src => (
            <div
              key={src.source_id}
              onClick={() => store.select(src.source_id)}
              className={`cursor-pointer rounded-lg border p-3 space-y-2 transition-all duration-150 ${
                store.selectedId === src.source_id
                  ? 'border-amber-500/40 bg-amber-500/5 ring-1 ring-amber-500/10'
                  : 'border-violet-900/30 bg-zinc-900 hover:border-violet-700/50'
              }`}
            >
              <div className="flex items-center justify-between">
                <span className="font-medium text-sm text-zinc-100">{src.display_name}</span>
                <span className="text-[10px] text-violet-500/70 uppercase tracking-widest">
                  {src.source_type}
                </span>
              </div>
              <div className="text-xs text-zinc-600 truncate">{src.base_url}</div>
              <div className="flex gap-2 pt-0.5">
                <button
                  onClick={e => { e.stopPropagation(); store.triggerSync(src.source_id) }}
                  disabled={store.syncing}
                  className="text-xs px-2 py-0.5 bg-violet-900/30 hover:bg-violet-800/40
                             text-violet-300 rounded border border-violet-800/30
                             disabled:opacity-40 transition-colors"
                >
                  {store.syncing ? 'Syncing…' : 'Sync'}
                </button>
                <button
                  onClick={e => { e.stopPropagation(); store.removeSource(src.source_id) }}
                  className="btn-danger"
                >
                  Remove
                </button>
              </div>
            </div>
          ))}
        </div>

        {/* Detail panel */}
        {store.selectedId && (
          <div className="space-y-4">
            {/* Credentials */}
            {NEEDS_TOKEN.includes(store.selected?.source_type ?? '') && (
              <div className="card p-3 space-y-2">
                <div className="flex items-center justify-between">
                  <span className="section-label">Auth Token</span>
                  {!editingCreds && (
                    <button
                      onClick={() => { setEditingCreds(true); setCredToken('') }}
                      className="text-xs px-2 py-0.5 bg-violet-900/30 hover:bg-violet-800/40
                                 text-violet-300 rounded border border-violet-800/30 transition-colors"
                    >
                      {store.credentials[store.selectedId]?.has_token ? 'Update' : 'Set Token'}
                    </button>
                  )}
                </div>

                {!editingCreds ? (
                  <div className="flex items-center gap-2">
                    {store.credentials[store.selectedId]?.has_token ? (
                      <>
                        <span className="text-xs text-emerald-400">● Stored in kairos.conf</span>
                        <button
                          onClick={() => store.deleteCredentials(store.selectedId!)}
                          className="text-xs text-red-500 hover:text-red-400 ml-auto transition-colors"
                        >
                          Remove
                        </button>
                      </>
                    ) : (
                      <span className="text-xs text-zinc-600">○ Not configured</span>
                    )}
                  </div>
                ) : (
                  <div className="space-y-2">
                    <input
                      placeholder="Paste token"
                      type="password"
                      value={credToken}
                      onChange={e => setCredToken(e.target.value)}
                      className="input w-full"
                    />
                    <div className="flex gap-2">
                      <button
                        onClick={async () => {
                          if (credToken) await store.setCredentials(store.selectedId!, credToken)
                          setEditingCreds(false)
                        }}
                        disabled={!credToken}
                        className="btn-primary disabled:opacity-40"
                      >
                        Save
                      </button>
                      <button onClick={() => setEditingCreds(false)} className="btn-ghost">
                        Cancel
                      </button>
                    </div>
                  </div>
                )}
              </div>
            )}

            {/* Libraries */}
            <div className="space-y-3">
              <div className="flex items-center justify-between">
                <h2 className="text-sm font-semibold text-zinc-300">
                  Libraries — {store.selected?.display_name}
                </h2>
                <button
                  onClick={() => { store.fetchAvailable(store.selectedId!); setShowAddLib(v => !v) }}
                  className="text-xs px-2 py-1 bg-violet-900/30 hover:bg-violet-800/40
                             text-violet-300 rounded border border-violet-800/30 transition-colors"
                >
                  + Add
                </button>
              </div>

              {showAddLib && (
                <div className="card p-3 space-y-2">
                  <select
                    value={libForm.external_lib_id}
                    onChange={e => {
                      const lib = store.available.find(l => l.external_lib_id === e.target.value)
                      setLibForm({ external_lib_id: e.target.value, display_name: lib?.name ?? libForm.display_name, library_type: (lib?.type ?? 'show') as any })
                    }}
                    className="input w-full"
                  >
                    <option value="">— select from server —</option>
                    {store.available.map(l => (
                      <option key={l.external_lib_id} value={l.external_lib_id}>
                        {l.name} ({l.type})
                      </option>
                    ))}
                  </select>
                  <input
                    placeholder="Display name"
                    value={libForm.display_name}
                    onChange={e => setLibForm({ ...libForm, display_name: e.target.value })}
                    className="input w-full"
                  />
                  <select
                    value={libForm.library_type}
                    onChange={e => setLibForm({ ...libForm, library_type: e.target.value as any })}
                    className="input w-full"
                  >
                    <option value="show">TV Shows</option>
                    <option value="movie">Movies</option>
                    <option value="mixed">Mixed</option>
                  </select>
                  <div className="flex gap-2">
                    <button onClick={addLib} className="btn-primary">Save</button>
                    <button onClick={() => setShowAddLib(false)} className="btn-ghost">Cancel</button>
                  </div>
                </div>
              )}

              {store.libraries.length === 0 && (
                <p className="text-zinc-600 text-sm">No libraries added yet.</p>
              )}
              {store.libraries.map(lib => (
                <div key={lib.library_id}
                     className="flex items-center justify-between card px-3 py-2.5">
                  <div>
                    <div className="text-sm text-zinc-200">{lib.display_name}</div>
                    <div className="text-xs text-zinc-600 mt-0.5">
                      {lib.library_type} · id: {lib.external_lib_id}
                    </div>
                  </div>
                  <button
                    onClick={() => store.removeLibrary(store.selectedId!, lib.library_id)}
                    className="btn-danger"
                  >
                    Remove
                  </button>
                </div>
              ))}
            </div>
          </div>
        )}
      </div>
    </div>
  )
})
