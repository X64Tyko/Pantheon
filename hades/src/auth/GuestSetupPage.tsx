import {useState} from 'react'
import {useNavigate} from 'react-router-dom'
import {useAuth} from './AuthContext'
import {TV_RATINGS, MOVIE_RATINGS} from '../api/ratingScales'
import PinPad from './PinPad'
import styles from './GuestSetupPage.module.css'

// First-run wizard for a freshly-created guest account (see LoginPage's
// "Continue as Guest" flow) — offers the same PIN + parental-control
// restriction fields UsersPage's edit form exposes for any other account,
// minus the admin/viewer role toggle (a guest is always viewer). Entirely
// skippable: a guest who skips everything keeps the defaults
// createGuestUser() set (no PIN, unrestricted) and can always come back via
// AccountPage later — nothing here is a one-time-only action.
export default function GuestSetupPage() {
    const {user, completeGuestSetup, resolveLandingPath} = useAuth()
    const navigate = useNavigate()

    const [step, setStep] = useState<'pin' | 'restriction'>('pin')
    const [pinError, setPinError] = useState('')
    const [pinBusy, setPinBusy] = useState(false)

    const [restricted, setRestricted] = useState(false)
    const [maxTv, setMaxTv] = useState('TV-Y')
    const [maxMovie, setMaxMovie] = useState('G')
    const [maxChannel, setMaxChannel] = useState('TV-Y')
    const [restrictionError, setRestrictionError] = useState('')
    const [restrictionBusy, setRestrictionBusy] = useState(false)

    const finish = async () => {
        const target = user ? await resolveLandingPath(user) : '/'
        navigate(target, {replace: true})
    }

    const setPin = async (pin: string) => {
        setPinError('')
        setPinBusy(true)
        try {
            await completeGuestSetup({pin})
            setStep('restriction')
        } catch (e: any) {
            setPinError(e.message ?? 'Failed to set PIN')
        } finally {
            setPinBusy(false)
        }
    }

    const saveRestriction = async () => {
        setRestrictionError('')
        setRestrictionBusy(true)
        try {
            await completeGuestSetup({
                restricted,
                max_tv_rating: maxTv, max_movie_rating: maxMovie, max_channel_rating: maxChannel,
            })
            await finish()
        } catch (e: any) {
            setRestrictionError(e.message ?? 'Failed to save')
        } finally {
            setRestrictionBusy(false)
        }
    }

    if (!user) return null

    return (
        <div className={styles.page}>
            <div className={styles.card}>
                <div className={styles.brandWrap}>
                    <div className={styles.title}>Welcome, {user.username}</div>
                    <div className={styles.subtitle}>
                        You're browsing as a guest — set these up now, or skip and change them anytime from My Account.
                    </div>
                </div>

                {step === 'pin' && (
                    <>
                        <div className={styles.stepLabel}>SET A PIN (OPTIONAL)</div>
                        <div className={styles.stepHint}>
                            Lets you get back into this exact guest profile from the "Who's watching?" picker on a
                            shared device.
                        </div>
                        <PinPad onComplete={setPin} error={pinError} busy={pinBusy} confirmLabel="SET PIN"/>
                        <button type="button" className={styles.skipBtn} onClick={() => setStep('restriction')}>
                            Skip
                        </button>
                    </>
                )}

                {step === 'restriction' && (
                    <>
                        <div className={styles.stepLabel}>CONTENT RESTRICTIONS (OPTIONAL)</div>
                        <div className={styles.stepHint}>
                            Try out Pantheon's parental controls on your own guest profile — limits what you see by
                            content rating.
                        </div>

                        <label className={styles.checkboxLabel}>
                            <input type="checkbox" checked={restricted}
                                   onChange={e => setRestricted(e.target.checked)}/>
                            Restrict this profile by content rating
                        </label>

                        {restricted && (
                            <div className={styles.ratingGrid}>
                                <div className={styles.fieldCol}>
                                    <label className={styles.fieldLabel}>MAX TV RATING</label>
                                    <select value={maxTv} onChange={e => setMaxTv(e.target.value)}
                                            className={styles.select}>
                                        {TV_RATINGS.map(r => <option key={r} value={r}>{r}</option>)}
                                    </select>
                                </div>
                                <div className={styles.fieldCol}>
                                    <label className={styles.fieldLabel}>MAX MOVIE RATING</label>
                                    <select value={maxMovie} onChange={e => setMaxMovie(e.target.value)}
                                            className={styles.select}>
                                        {MOVIE_RATINGS.map(r => <option key={r} value={r}>{r}</option>)}
                                    </select>
                                </div>
                                <div className={styles.fieldCol}>
                                    <label className={styles.fieldLabel}>MAX CHANNEL RATING</label>
                                    <select value={maxChannel} onChange={e => setMaxChannel(e.target.value)}
                                            className={styles.select}>
                                        {TV_RATINGS.map(r => <option key={r} value={r}>{r}</option>)}
                                    </select>
                                </div>
                            </div>
                        )}

                        {restrictionError && <div className={styles.errorBox}>{restrictionError}</div>}

                        <div className={styles.actionRow}>
                            <button type="button" className={styles.skipBtn} onClick={finish}>
                                Skip
                            </button>
                            <button
                                type="button" disabled={restrictionBusy}
                                onClick={saveRestriction}
                                className={`${styles.submitBtn} ${restrictionBusy ? styles.submitBtnDisabled : styles.submitBtnEnabled}`}
                            >
                                {restrictionBusy ? 'SAVING…' : 'FINISH'}
                            </button>
                        </div>
                    </>
                )}
            </div>
        </div>
    )
}
