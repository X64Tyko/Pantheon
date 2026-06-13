import { observer } from 'mobx-react-lite'
import { useEffect, useState } from 'react'
import { Link } from 'react-router-dom'
import { channelStore } from '../stores'

export default observer(function ChannelsPage() {
  const store = channelStore
  const [showAdd, setShowAdd] = useState(false)
  const [form, setForm]       = useState({ name: '', number: '', timezone: 'UTC' })

  useEffect(() => { store.fetchAll() }, [])

  const add = async () => {
    await store.add({ name: form.name, number: parseInt(form.number), timezone: form.timezone })
    setShowAdd(false)
    setForm({ name: '', number: '', timezone: 'UTC' })
  }

  return (
    <div className="space-y-5">
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold text-zinc-100">Channels</h1>
        <button onClick={() => setShowAdd(v => !v)} className="btn-primary">
          + Add Channel
        </button>
      </div>

      {store.error && (
        <div className="text-red-400 text-sm bg-red-950/30 border border-red-900/40 rounded-lg p-3">
          {store.error}
        </div>
      )}

      {showAdd && (
        <div className="card p-4 space-y-4">
          <h2 className="section-label">New Channel</h2>
          <div className="grid grid-cols-3 gap-3">
            <input
              placeholder="Channel name"
              value={form.name}
              onChange={e => setForm({ ...form, name: e.target.value })}
              className="input"
            />
            <input
              type="number"
              placeholder="Channel number"
              value={form.number}
              onChange={e => setForm({ ...form, number: e.target.value })}
              className="input"
            />
            <input
              placeholder="Timezone  (e.g. America/Chicago)"
              value={form.timezone}
              onChange={e => setForm({ ...form, timezone: e.target.value })}
              className="input"
            />
          </div>
          <div className="flex gap-2">
            <button onClick={add} className="btn-primary">Save</button>
            <button onClick={() => setShowAdd(false)} className="btn-ghost">Cancel</button>
          </div>
        </div>
      )}

      <div className="space-y-2">
        {store.channels.length === 0 && !store.loading && (
          <p className="text-zinc-600 text-sm">No channels configured.</p>
        )}
        {store.channels.map(ch => (
          <div key={ch.channel_id}
               className="flex items-center justify-between card px-4 py-3">
            <div className="flex items-center gap-5">
              <span className="text-amber-400 font-mono font-bold w-8 text-right">
                {ch.number}
              </span>
              <div>
                <div className="font-medium text-sm text-zinc-100">{ch.name}</div>
                <div className="text-[10px] text-zinc-600 mt-0.5">{ch.timezone}</div>
              </div>
            </div>
            <div className="flex gap-2">
              <Link to={`/channels/${ch.channel_id}`} className="btn-secondary">
                Edit Schedule
              </Link>
              <button onClick={() => store.remove(ch.channel_id)} className="btn-danger">
                Remove
              </button>
            </div>
          </div>
        ))}
      </div>
    </div>
  )
})
