import {useNavigate} from 'react-router-dom'
import {useNavBack} from '../nav/back'
import {useFocusable} from '../nav/useFocusable'
import {TvGuideSection} from './TvGuideSection'
import homeStyles from './TvHome.module.css'
import styles from './TvGuidePage.module.css'

// Standalone /tv/guide route — TvGuideSection used to be embedded inline at
// the bottom of TvHome (reached by scrolling + a quick-action button that
// scrollIntoView'd to it), the one place native Android's own audit of this
// pass found web `/tv` hadn't caught up to desktop Guide's own standalone-
// route treatment. TvGuideSection itself is unchanged — it already owns its
// own FocusContext/session plumbing; this page just gives it a real route
// and a Home/Library quick-action row, same style as Home's own
// (quickActionRow/quickActionButton, reused directly from TvHome.module.css
// rather than duplicated).
export function TvGuidePage() {
    const navigate = useNavigate()
    useNavBack(() => navigate('/tv'))

    return (
        <div className={styles.page}>
            <div className={homeStyles.quickActionRow}>
                <HomeButton onClick={() => navigate('/tv')}/>
                <LibraryButton onClick={() => navigate('/tv/library')}/>
            </div>
            <div className={styles.guideArea}>
                <TvGuideSection/>
            </div>
        </div>
    )
}

function HomeButton({onClick}: { onClick: () => void }) {
    // forceFocus: this page has no other default-focused element above the
    // quick-action row (TvGuideSection's own preview/grid focuses itself once
    // channels load) — same fallback TvHome's LibraryButton and TvLibrary's
    // back button both use for the same reason.
    const {ref, focused} = useFocusable<object, HTMLButtonElement>({
        focusKey: 'tv-guide-quickaction-home',
        onEnterPress: onClick,
        forceFocus: true
    })
    return (
        <button
            ref={ref} data-tv-focused={focused}
            onClick={onClick}
            className={homeStyles.quickActionButton}
        >
            <svg width="16" height="16" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4"
                 strokeLinecap="round" strokeLinejoin="round">
                <path d="M1.5 6.5L7 2l5.5 4.5M3 5.5V12h8V5.5"/>
            </svg>
            Home
        </button>
    )
}

function LibraryButton({onClick}: { onClick: () => void }) {
    const {ref, focused} = useFocusable<object, HTMLButtonElement>({
        focusKey: 'tv-guide-quickaction-library',
        onEnterPress: onClick
    })
    return (
        <button
            ref={ref} data-tv-focused={focused}
            onClick={onClick}
            className={homeStyles.quickActionButton}
        >
            <svg width="16" height="16" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4">
                <rect x="1.5" y="1.5" width="4.5" height="11" rx="1"/>
                <rect x="8" y="1.5" width="4.5" height="11" rx="1"/>
            </svg>
            Library
        </button>
    )
}
