import { observer } from 'mobx-react-lite'
import { useEffect, useState } from 'react'
import { useAuth } from '../auth/AuthContext'
import { userStore } from '../stores'
import { TV_RATINGS, MOVIE_RATINGS } from '../api/ratingScales'
import type { InviteUserResult, User } from '../api/types'
import UserOverridesOverlay from './UserOverridesOverlay'
import PinPad from '../auth/PinPad'
import styles from './UsersPage.module.css'

const BTN_VARIANT_CLASS = {
  primary: styles.btnPrimary,
  ghost:   styles.btnGhost,
  danger:  styles.btnDanger,
} as const
const btnClass = (variant: 'primary' | 'ghost' | 'danger') => `${styles.btn} ${BTN_VARIANT_CLASS[variant]}`

// last_seen is epoch seconds — most recent activity across every session
// this account has ever held (see AuthStore::listUsers). Guest-only display;
// coarse buckets are plenty for "is this account still being used" at a
// glance, not a precise clock.
export function formatLastActive(lastSeenSec: number): string {
    if (!lastSeenSec) return 'never'
    const deltaSec = Date.now() / 1000 - lastSeenSec
    if (deltaSec < 60) return 'just now'
    if (deltaSec < 3600) return `${Math.floor(deltaSec / 60)}m ago`
    if (deltaSec < 86400) return `${Math.floor(deltaSec / 3600)}h ago`
    return `${Math.floor(deltaSec / 86400)}d ago`
}

type CreateMode = 'password' | 'temp_password' | 'email'
interface NewUserForm { username: string; password: string; role: 'admin' | 'viewer'; mode: CreateMode; email: string }
interface EditState {
  userId:     string
  password:   string
  role:       'admin' | 'viewer'
  restricted: boolean
  maxTv:      string
  maxMovie:   string
  maxChannel: string
}

const editStateFor = (u: User): EditState => ({
  userId: u.user_id, password: '', role: u.role,
  restricted: u.restricted, maxTv: u.max_tv_rating, maxMovie: u.max_movie_rating, maxChannel: u.max_channel_rating,
})

export default observer(function UsersPage() {
  const { user: self } = useAuth()
  const store = userStore

  const [showNew,   setShowNew]   = useState(false)
  const [newForm,   setNewForm]   = useState<NewUserForm>({ username: '', password: '', role: 'viewer', mode: 'password', email: '' })
  const [newError,  setNewError]  = useState('')
  const [newBusy,   setNewBusy]   = useState(false)
  const [newResult, setNewResult] = useState<InviteUserResult | null>(null)

  const [editing,   setEditing]   = useState<EditState | null>(null)
  const [editError, setEditError] = useState('')
  const [editBusy,  setEditBusy]  = useState(false)

  const [deleting,   setDeleting]   = useState<string | null>(null)
  const [overridesFor, setOverridesFor] = useState<User | null>(null)

  const [showPinPad, setShowPinPad] = useState(false)
  const [pinBusy,    setPinBusy]    = useState(false)
  const [pinError,   setPinError]   = useState('')

  useEffect(() => { store.fetchAll() }, [])

  if (self?.role !== 'admin') {
    return (
      <div className={styles.adminRequired}>
        Admin access required.
      </div>
    )
  }

  const submitNew = async (e: React.FormEvent) => {
    e.preventDefault()
    if (!newForm.username) { setNewError('Username required'); return }
    if (newForm.mode === 'password' && !newForm.password) { setNewError('Password required'); return }
    if (newForm.mode === 'email' && !newForm.email) { setNewError('Email required for invite-by-email'); return }
    setNewError(''); setNewBusy(true)
    try {
      if (newForm.mode === 'password') {
        await store.create(newForm.username, newForm.password, newForm.role)
        setShowNew(false)
        setNewForm({ username: '', password: '', role: 'viewer', mode: 'password', email: '' })
      } else {
        const result = await store.invite(newForm.username, newForm.role,
          newForm.mode === 'email' ? { method: 'email', email: newForm.email } : { method: 'temp_password' })
        // Stay open, showing the result — the temp password/invite link is
        // only ever shown here, once, so don't let the form close it away.
        setNewResult(result)
      }
    } catch (err: any) {
      setNewError(err.message ?? 'Failed to create user')
    } finally { setNewBusy(false) }
  }

  const submitEdit = async (e: React.FormEvent) => {
    e.preventDefault()
    if (!editing) return
    setEditError(''); setEditBusy(true)
    try {
      await store.update(editing.userId, {
        password: editing.password || undefined,
        role:     editing.role,
      })
      await store.updateRestriction(editing.userId, {
        restricted:         editing.restricted,
        max_tv_rating:      editing.maxTv,
        max_movie_rating:   editing.maxMovie,
        max_channel_rating: editing.maxChannel,
      })
      setEditing(null)
    } catch (err: any) {
      setEditError(err.message ?? 'Failed to update user')
    } finally { setEditBusy(false) }
  }

  const confirmDelete = async (userId: string) => {
    try {
      await store.remove(userId)
    } catch (err: any) {
      // error surfaced via store.error
    } finally { setDeleting(null) }
  }

  const setPin = async (userId: string, pin: string) => {
    setPinError(''); setPinBusy(true)
    try {
      await store.setPin(userId, pin)
      setShowPinPad(false)
    } catch (err: any) {
      setPinError(err.message ?? 'Failed to set PIN')
    } finally { setPinBusy(false) }
  }

  const clearPin = async (userId: string) => {
    setPinError(''); setPinBusy(true)
    try {
      await store.setPin(userId, '')
    } catch (err: any) {
      setPinError(err.message ?? 'Failed to clear PIN')
    } finally { setPinBusy(false) }
  }

  return (
    <div className={styles.root}>
      <div className={styles.pageHeader}>
        <div>
          <div className={styles.pageTitle}>Users</div>
          <div className={styles.pageSubtitle}>Manage who can access Hades.</div>
        </div>
        <button className={btnClass('primary')} onClick={() => { setShowNew(v => !v); setNewError(''); setNewResult(null) }}>
          {showNew ? 'Cancel' : '+ New User'}
        </button>
      </div>

      {store.error && (
        <div className={styles.storeError}>
          {store.error}
        </div>
      )}

      {/* New user form */}
      {showNew && (
        <form onSubmit={submitNew} className={styles.formPanel}>
          <div className={styles.formPanelHeading}>NEW USER</div>

          <div className={styles.fieldCol}>
            <label className={styles.fieldLabel}>ACCOUNT SETUP</label>
            <select value={newForm.mode}
              onChange={e => { setNewForm(f => ({ ...f, mode: e.target.value as CreateMode })); setNewResult(null) }}
              className={styles.input}>
              <option value="password">I'll set their password</option>
              <option value="temp_password">Invite — I'll relay a one-time password</option>
              <option value="email">Invite — email them a setup link</option>
            </select>
          </div>

          <div className={newForm.mode === 'password' ? styles.formGrid2 : styles.formGrid1}>
            <div className={styles.fieldCol}>
              <label className={styles.fieldLabel}>USERNAME</label>
              <input type="text" required autoFocus value={newForm.username}
                onChange={e => setNewForm(f => ({ ...f, username: e.target.value }))} className={styles.input} />
            </div>
            {newForm.mode === 'password' && (
              <div className={styles.fieldCol}>
                <label className={styles.fieldLabel}>PASSWORD</label>
                <input type="password" required value={newForm.password}
                  onChange={e => setNewForm(f => ({ ...f, password: e.target.value }))} className={styles.input} />
              </div>
            )}
          </div>
          {newForm.mode === 'email' && (
            <div className={styles.fieldCol}>
              <label className={styles.fieldLabel}>EMAIL</label>
              <input type="email" required value={newForm.email}
                onChange={e => setNewForm(f => ({ ...f, email: e.target.value }))} className={styles.input} />
            </div>
          )}
          <div className={styles.fieldCol}>
            <label className={styles.fieldLabel}>ROLE</label>
            <select value={newForm.role}
              onChange={e => setNewForm(f => ({ ...f, role: e.target.value as 'admin' | 'viewer' }))}
              className={`${styles.input} ${styles.inputAuto}`}>
              <option value="viewer">viewer</option>
              <option value="admin">admin</option>
            </select>
          </div>
          {newError && <div className={styles.inlineError}>{newError}</div>}

          {newResult && (
            newResult.ok ? (
              <div className={styles.resultBox}>
                {newForm.mode === 'temp_password' ? (
                  <>Account created. Temp password (share once, then discard):{' '}
                    <code className={styles.resultCode}>{newResult.temp_password}</code>
                  </>
                ) : (
                  <>Account created. Invite email {newResult.invite_sent ? 'sent' : `could not be sent (${newResult.invite_error ?? 'unknown error'})`} — link:{' '}
                    <code className={styles.resultCodeBreak}>{newResult.invite_link}</code>
                  </>
                )}
              </div>
            ) : null
          )}

          <div className={styles.btnRow}>
            <button type="submit" disabled={newBusy} className={btnClass('primary')}>
              {newBusy ? 'Creating…' : newForm.mode === 'password' ? 'Create' : newResult ? 'Send Another' : 'Create & Invite'}
            </button>
            <button type="button" className={btnClass('ghost')} onClick={() => setShowNew(false)}>Cancel</button>
          </div>
        </form>
      )}

      {/* User list */}
      {store.loading ? (
        <div className={styles.loadingText}>Loading…</div>
      ) : (
        <div className={styles.userList}>
          {store.users.map(u => (
            <div key={u.user_id}>
              {editing?.userId !== u.user_id && (
                <div className={styles.userRow}>
                  <div className={styles.userNameCol}>
                    <span className={`${styles.userName} ${u.user_id === self?.user_id ? styles.userNameSelf : styles.userNameNotSelf}`}>
                      {u.username}
                      {u.user_id === self?.user_id && <span className={styles.youTag}>(you)</span>}
                    </span>
                  </div>
                  <span className={`${styles.roleBadge} ${u.role === 'admin' ? styles.roleBadgeAdmin : styles.roleBadgeViewer}`}>
                    {u.role}
                  </span>
                    {u.is_guest && (
                        <span className={styles.guestBadge}>
                      GUEST
                    </span>
                    )}
                  {u.restricted && (
                    <span className={styles.restrictedBadge}>
                      RESTRICTED
                    </span>
                  )}
                  {u.has_pin && (
                    <span title="PIN set" className={styles.pinIcon}>🔒</span>
                  )}
                    {u.is_guest && (
                        <span className={styles.lastActive}>
                      active {formatLastActive(u.last_seen)}
                    </span>
                    )}
                  <div className={styles.rowActions}>
                    {u.restricted && (
                      <button className={btnClass('ghost')} onClick={() => setOverridesFor(u)}>
                        Overrides
                      </button>
                    )}
                    <button className={btnClass('ghost')}
                      onClick={() => { setEditing(editStateFor(u)); setEditError(''); setShowPinPad(false); setPinError('') }}>
                      Edit
                    </button>
                    {u.user_id !== self?.user_id && (
                      deleting === u.user_id ? (
                        <>
                          <button className={btnClass('danger')} onClick={() => confirmDelete(u.user_id)}>Confirm</button>
                          <button className={btnClass('ghost')} onClick={() => setDeleting(null)}>Cancel</button>
                        </>
                      ) : (
                        <button className={btnClass('danger')} onClick={() => setDeleting(u.user_id)}>Delete</button>
                      )
                    )}
                  </div>
                </div>
              )}

              {editing?.userId === u.user_id && (
                <form onSubmit={submitEdit} className={styles.editForm}>
                  <div className={styles.editFormHeading}>
                    EDIT · {u.username}
                  </div>
                  <div className={styles.formGrid2}>
                    <div className={styles.fieldCol}>
                      <label className={styles.fieldLabel}>NEW PASSWORD <span className={styles.fieldLabelHint}>(leave blank to keep)</span></label>
                      <input type="password" autoFocus value={editing.password}
                        onChange={e => setEditing(s => s && ({ ...s, password: e.target.value }))} className={styles.input} />
                    </div>
                    <div className={styles.fieldCol}>
                      <label className={styles.fieldLabel}>ROLE</label>
                      <select value={editing.role}
                        onChange={e => setEditing(s => s && ({ ...s, role: e.target.value as 'admin' | 'viewer' }))}
                        className={`${styles.input} ${styles.inputAuto}`}>
                        <option value="viewer">viewer</option>
                        <option value="admin">admin</option>
                      </select>
                    </div>
                  </div>

                  <div className={styles.editSection}>
                    <label className={styles.checkboxLabel}>
                      <input type="checkbox" checked={editing.restricted}
                        onChange={e => setEditing(s => s && ({ ...s, restricted: e.target.checked }))} />
                      Restricted account <span className={styles.fieldLabelHint}>(limit by content rating; overrides always win)</span>
                    </label>
                    {editing.restricted && (
                      <div className={styles.formGrid3}>
                        <div className={styles.fieldCol}>
                          <label className={styles.fieldLabel}>MAX TV RATING</label>
                          <select value={editing.maxTv}
                            onChange={e => setEditing(s => s && ({ ...s, maxTv: e.target.value }))} className={styles.input}>
                            {TV_RATINGS.map(r => <option key={r} value={r}>{r}</option>)}
                          </select>
                        </div>
                        <div className={styles.fieldCol}>
                          <label className={styles.fieldLabel}>MAX MOVIE RATING</label>
                          <select value={editing.maxMovie}
                            onChange={e => setEditing(s => s && ({ ...s, maxMovie: e.target.value }))} className={styles.input}>
                            {MOVIE_RATINGS.map(r => <option key={r} value={r}>{r}</option>)}
                          </select>
                        </div>
                        <div className={styles.fieldCol}>
                          <label className={styles.fieldLabel}>MAX CHANNEL RATING</label>
                          <select value={editing.maxChannel}
                            onChange={e => setEditing(s => s && ({ ...s, maxChannel: e.target.value }))} className={styles.input}>
                            {TV_RATINGS.map(r => <option key={r} value={r}>{r}</option>)}
                          </select>
                        </div>
                      </div>
                    )}
                  </div>

                  <div className={styles.editSection}>
                    <div className={styles.pinRow}>
                      <div className={styles.pinRowLabel}>
                        Profile-switch PIN <span className={styles.fieldLabelHint}>({u.has_pin ? 'set' : 'not set'}) — used by the "Who\'s watching?" picker</span>
                      </div>
                      <div className={styles.rowActions}>
                        {u.has_pin && (
                          <button type="button" disabled={pinBusy} className={btnClass('ghost')} onClick={() => clearPin(u.user_id)}>
                            Clear PIN
                          </button>
                        )}
                        <button type="button" className={btnClass('ghost')} onClick={() => { setShowPinPad(v => !v); setPinError('') }}>
                          {showPinPad ? 'Cancel' : u.has_pin ? 'Change PIN' : 'Set PIN'}
                        </button>
                      </div>
                    </div>
                    {editing.role === 'admin' && !u.has_pin && (
                      <div className={styles.pinWarning}>
                        Admin profiles need a PIN before they can be used from the profile picker.
                      </div>
                    )}
                    {showPinPad && (
                      <div className={styles.pinPadWrap}>
                        <PinPad busy={pinBusy} error={pinError} confirmLabel="SAVE" onComplete={pin => setPin(u.user_id, pin)} />
                      </div>
                    )}
                  </div>

                  {editError && <div className={styles.inlineError}>{editError}</div>}
                  <div className={styles.btnRow}>
                    <button type="submit" disabled={editBusy} className={btnClass('primary')}>
                      {editBusy ? 'Saving…' : 'Save'}
                    </button>
                    <button type="button" className={btnClass('ghost')} onClick={() => setEditing(null)}>Cancel</button>
                  </div>
                </form>
              )}
            </div>
          ))}
        </div>
      )}

      {overridesFor && (
        <UserOverridesOverlay user={overridesFor} onClose={() => setOverridesFor(null)} />
      )}
    </div>
  )
})
