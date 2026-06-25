import { useEffect, useState } from 'react'
import { api } from '../api/client'
import { useAuth } from '../auth/AuthContext'
import type { User } from '../api/types'

const inputStyle: React.CSSProperties = {
  padding: '7px 10px', background: 'var(--hds-bg-3)',
  border: '1px solid var(--hds-line)', borderRadius: 7,
  color: 'var(--hds-txt)', fontSize: 12,
  fontFamily: "'JetBrains Mono', monospace", outline: 'none',
  width: '100%', boxSizing: 'border-box',
}

const btnStyle = (variant: 'primary' | 'ghost' | 'danger'): React.CSSProperties => ({
  padding: '7px 14px', borderRadius: 7, fontSize: 11, fontWeight: 600, cursor: 'pointer',
  fontFamily: "'JetBrains Mono', monospace", letterSpacing: '0.06em',
  background: variant === 'primary' ? 'oklch(0.83 0.13 84 / 0.13)'
            : variant === 'danger'  ? 'oklch(0.55 0.22 22 / 0.12)'
            : 'transparent',
  border: variant === 'primary' ? '1px solid oklch(0.83 0.13 84 / 0.4)'
        : variant === 'danger'  ? '1px solid oklch(0.55 0.22 22 / 0.35)'
        : '1px solid var(--hds-line)',
  color: variant === 'primary' ? 'var(--hds-gold)'
       : variant === 'danger'  ? 'oklch(0.72 0.18 22)'
       : 'var(--hds-txt-2)',
})

interface NewUserForm { username: string; password: string; role: 'admin' | 'viewer' }
interface EditState   { userId: string; password: string; role: 'admin' | 'viewer' }

export default function UsersPage() {
  const { user: self } = useAuth()
  const [users,   setUsers]   = useState<User[]>([])
  const [loading, setLoading] = useState(true)
  const [error,   setError]   = useState('')

  const [showNew,  setShowNew]  = useState(false)
  const [newForm,  setNewForm]  = useState<NewUserForm>({ username: '', password: '', role: 'viewer' })
  const [newError, setNewError] = useState('')
  const [newBusy,  setNewBusy]  = useState(false)

  const [editing,   setEditing]   = useState<EditState | null>(null)
  const [editError, setEditError] = useState('')
  const [editBusy,  setEditBusy]  = useState(false)

  const [deleting, setDeleting] = useState<string | null>(null)

  const load = () => {
    setLoading(true)
    api.getUsers()
      .then(setUsers)
      .catch(e => setError(e.message))
      .finally(() => setLoading(false))
  }

  useEffect(load, [])

  if (self?.role !== 'admin') {
    return (
      <div style={{ padding: 32, color: 'var(--hds-txt-3)', fontSize: 13 }}>
        Admin access required.
      </div>
    )
  }

  const submitNew = async (e: React.FormEvent) => {
    e.preventDefault()
    if (!newForm.username || !newForm.password) { setNewError('Username and password required'); return }
    setNewError(''); setNewBusy(true)
    try {
      await api.createUser(newForm.username, newForm.password, newForm.role)
      setShowNew(false)
      setNewForm({ username: '', password: '', role: 'viewer' })
      load()
    } catch (err: any) {
      setNewError(err.message ?? 'Failed to create user')
    } finally { setNewBusy(false) }
  }

  const submitEdit = async (e: React.FormEvent) => {
    e.preventDefault()
    if (!editing) return
    setEditError(''); setEditBusy(true)
    try {
      await api.updateUser(editing.userId, {
        password: editing.password || undefined,
        role:     editing.role,
      })
      setEditing(null)
      load()
    } catch (err: any) {
      setEditError(err.message ?? 'Failed to update user')
    } finally { setEditBusy(false) }
  }

  const confirmDelete = async (userId: string) => {
    try {
      await api.deleteUser(userId)
      load()
    } catch (err: any) {
      setError(err.message ?? 'Failed to delete user')
    } finally { setDeleting(null) }
  }

  return (
    <div style={{ maxWidth: 640 }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 22 }}>
        <div>
          <div style={{ fontSize: 15, fontWeight: 600, color: 'var(--hds-txt)', letterSpacing: '0.04em' }}>Users</div>
          <div style={{ fontSize: 11, color: 'var(--hds-txt-3)', marginTop: 3 }}>Manage who can access Hades.</div>
        </div>
        <button style={btnStyle('primary')} onClick={() => { setShowNew(v => !v); setNewError('') }}>
          {showNew ? 'Cancel' : '+ New User'}
        </button>
      </div>

      {error && (
        <div style={{ marginBottom: 14, fontSize: 11, color: 'oklch(0.72 0.18 22)', padding: '8px 10px', background: 'oklch(0.55 0.22 22 / 0.1)', borderRadius: 7, border: '1px solid oklch(0.55 0.22 22 / 0.3)' }}>
          {error}
        </div>
      )}

      {/* New user form */}
      {showNew && (
        <form onSubmit={submitNew} style={{
          marginBottom: 18, padding: 18,
          background: 'var(--hds-bg-2)', border: '1px solid var(--hds-line-s)',
          borderRadius: 10, display: 'flex', flexDirection: 'column', gap: 12,
          animation: 'hds-in 0.15s ease both',
        }}>
          <div style={{ fontSize: 11, color: 'var(--hds-txt-2)', fontWeight: 600, letterSpacing: '0.08em' }}>NEW USER</div>
          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 10 }}>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 5 }}>
              <label style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.06em' }}>USERNAME</label>
              <input type="text" required autoFocus value={newForm.username}
                onChange={e => setNewForm(f => ({ ...f, username: e.target.value }))} style={inputStyle} />
            </div>
            <div style={{ display: 'flex', flexDirection: 'column', gap: 5 }}>
              <label style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.06em' }}>PASSWORD</label>
              <input type="password" required value={newForm.password}
                onChange={e => setNewForm(f => ({ ...f, password: e.target.value }))} style={inputStyle} />
            </div>
          </div>
          <div style={{ display: 'flex', flexDirection: 'column', gap: 5 }}>
            <label style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.06em' }}>ROLE</label>
            <select value={newForm.role}
              onChange={e => setNewForm(f => ({ ...f, role: e.target.value as 'admin' | 'viewer' }))}
              style={{ ...inputStyle, width: 'auto' }}>
              <option value="viewer">viewer</option>
              <option value="admin">admin</option>
            </select>
          </div>
          {newError && <div style={{ fontSize: 11, color: 'oklch(0.72 0.18 22)' }}>{newError}</div>}
          <div style={{ display: 'flex', gap: 8 }}>
            <button type="submit" disabled={newBusy} style={btnStyle('primary')}>
              {newBusy ? 'Creating…' : 'Create'}
            </button>
            <button type="button" style={btnStyle('ghost')} onClick={() => setShowNew(false)}>Cancel</button>
          </div>
        </form>
      )}

      {/* User list */}
      {loading ? (
        <div style={{ fontSize: 12, color: 'var(--hds-txt-3)' }}>Loading…</div>
      ) : (
        <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
          {users.map(u => (
            <div key={u.user_id}>
              {/* Row */}
              {editing?.userId !== u.user_id && (
                <div style={{
                  display: 'flex', alignItems: 'center', gap: 12,
                  padding: '12px 16px',
                  background: 'var(--hds-bg-2)', border: '1px solid var(--hds-line-s)',
                  borderRadius: 9,
                }}>
                  <div style={{ flex: 1, minWidth: 0 }}>
                    <span style={{ fontSize: 13, color: 'var(--hds-txt)', fontWeight: u.user_id === self?.user_id ? 600 : 400 }}>
                      {u.username}
                      {u.user_id === self?.user_id && <span style={{ marginLeft: 6, fontSize: 9, color: 'var(--hds-txt-3)' }}>(you)</span>}
                    </span>
                  </div>
                  <span style={{
                    fontSize: 9.5, letterSpacing: '0.1em', fontWeight: 700, padding: '2px 7px',
                    borderRadius: 4,
                    background: u.role === 'admin' ? 'oklch(0.83 0.13 84 / 0.12)' : 'var(--hds-bg-3)',
                    color:      u.role === 'admin' ? 'var(--hds-gold)'              : 'var(--hds-txt-3)',
                    border:     u.role === 'admin' ? '1px solid oklch(0.83 0.13 84 / 0.3)' : '1px solid var(--hds-line)',
                  }}>
                    {u.role}
                  </span>
                  <div style={{ display: 'flex', gap: 6 }}>
                    <button style={btnStyle('ghost')}
                      onClick={() => { setEditing({ userId: u.user_id, password: '', role: u.role }); setEditError('') }}>
                      Edit
                    </button>
                    {u.user_id !== self?.user_id && (
                      deleting === u.user_id ? (
                        <>
                          <button style={btnStyle('danger')} onClick={() => confirmDelete(u.user_id)}>Confirm</button>
                          <button style={btnStyle('ghost')} onClick={() => setDeleting(null)}>Cancel</button>
                        </>
                      ) : (
                        <button style={btnStyle('danger')} onClick={() => setDeleting(u.user_id)}>Delete</button>
                      )
                    )}
                  </div>
                </div>
              )}

              {/* Inline edit form */}
              {editing?.userId === u.user_id && (
                <form onSubmit={submitEdit} style={{
                  padding: '14px 16px',
                  background: 'var(--hds-bg-2)', border: '1px solid var(--hds-line)',
                  borderRadius: 9, display: 'flex', flexDirection: 'column', gap: 12,
                  animation: 'hds-in 0.15s ease both',
                }}>
                  <div style={{ fontSize: 11, color: 'var(--hds-txt-2)', fontWeight: 600, letterSpacing: '0.08em' }}>
                    EDIT · {u.username}
                  </div>
                  <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 10 }}>
                    <div style={{ display: 'flex', flexDirection: 'column', gap: 5 }}>
                      <label style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.06em' }}>NEW PASSWORD <span style={{ opacity: 0.6 }}>(leave blank to keep)</span></label>
                      <input type="password" autoFocus value={editing.password}
                        onChange={e => setEditing(s => s && ({ ...s, password: e.target.value }))} style={inputStyle} />
                    </div>
                    <div style={{ display: 'flex', flexDirection: 'column', gap: 5 }}>
                      <label style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.06em' }}>ROLE</label>
                      <select value={editing.role}
                        onChange={e => setEditing(s => s && ({ ...s, role: e.target.value as 'admin' | 'viewer' }))}
                        style={{ ...inputStyle, width: 'auto' }}>
                        <option value="viewer">viewer</option>
                        <option value="admin">admin</option>
                      </select>
                    </div>
                  </div>
                  {editError && <div style={{ fontSize: 11, color: 'oklch(0.72 0.18 22)' }}>{editError}</div>}
                  <div style={{ display: 'flex', gap: 8 }}>
                    <button type="submit" disabled={editBusy} style={btnStyle('primary')}>
                      {editBusy ? 'Saving…' : 'Save'}
                    </button>
                    <button type="button" style={btnStyle('ghost')} onClick={() => setEditing(null)}>Cancel</button>
                  </div>
                </form>
              )}
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
