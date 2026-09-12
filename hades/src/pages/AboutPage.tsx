import {useEffect, useState} from 'react'
import {Link} from 'react-router-dom'
import ReactMarkdown from 'react-markdown'
import {api} from '../api/client'
import styles from './AboutPage.module.css'

// Public, unauthenticated — reachable without a session (see App.tsx, kept
// outside <ProtectedRoute />) since its audience is prospective contributors
// and supporters, not necessarily existing users. Content is fetched fresh
// from Kairos's own cache of docs/About.md (AboutService.cpp), which in turn
// pulls it from GitHub — the same content backing the published docs site's
// About.html, so the two can never drift out of sync with each other.
export default function AboutPage() {
    const [content, setContent] = useState<string | null>(null)
    const [error, setError] = useState<string | null>(null)

    useEffect(() => {
        api.getAbout()
            .then(r => setContent(r.content))
            .catch(() => setError('Could not load this page right now — try again shortly.'))
    }, [])

    return (
        <div className={styles.page}>
            <div className={styles.card}>
                <Link to="/" className={styles.back}>← Pantheon</Link>
                <div className={styles.brandTitle}>ABOUT PANTHEON</div>
                {error && <div className={styles.error}>{error}</div>}
                {!error && !content && <div className={styles.loading}>Loading…</div>}
                {content && (
                    <div className={styles.markdown}>
                        <ReactMarkdown>{content}</ReactMarkdown>
                    </div>
                )}
            </div>
        </div>
    )
}
