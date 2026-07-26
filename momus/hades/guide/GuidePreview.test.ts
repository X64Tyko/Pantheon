import {describe, it, expect} from 'vitest'
import {fmtClock, fmtCountdown, computePreviewTiming} from '@/guide/GuidePreview'
import type {EpgProgram} from '@/api/types'

// GuidePreview.tsx's own three pure pieces: two trivial-but-regressable
// formatters, and the isLive/startsInMs boundary logic its own class comment
// (and useGuideSession's) explains — "you can't actually preview a future
// program," so the live video always follows the focused channel while these
// two values only ever drive the hero's TEXT.

function program(overrides: Partial<EpgProgram> = {}): EpgProgram {
    return {
        item_type: 'movie',
        item_id: 'm1',
        block_id: 'b1',
        title: 'Test Movie',
        duration_ms: 3_600_000,
        wall_clock_start_ms: 10_000,
        wall_clock_end_ms: 13_600_000,
        status: 'scheduled',
        ...overrides,
    }
}

describe('fmtClock', () => {
    it('formats a timestamp as a localized hour:minute clock', () => {
        // 14:05 UTC on a fixed date, formatted en-US 'numeric'/'2-digit' — exact
        // wall-clock rendering depends on the host TZ, so only assert on shape,
        // not a hardcoded string.
        const out = fmtClock(Date.UTC(2026, 0, 1, 14, 5))
        expect(out).toMatch(/^\d{1,2}:\d{2}\s?(AM|PM)$/)
    })
})

describe('fmtCountdown', () => {
    it('formats under an hour as plain minutes', () => {
        expect(fmtCountdown(5 * 60_000)).toBe('5m')
    })

    it('rounds to the nearest minute', () => {
        expect(fmtCountdown(90_000)).toBe('2m') // 1.5min rounds up
        expect(fmtCountdown(89_000)).toBe('1m') // 1.483min rounds down
    })

    it('exactly 59 minutes stays in minutes form', () => {
        expect(fmtCountdown(59 * 60_000)).toBe('59m')
    })

    it('exactly 60 minutes rolls over to hours, dropping the minutes part', () => {
        expect(fmtCountdown(60 * 60_000)).toBe('1h')
    })

    it('61 minutes shows hours plus remaining minutes', () => {
        expect(fmtCountdown(61 * 60_000)).toBe('1h 1m')
    })

    it('an exact multiple of an hour never shows "0m"', () => {
        expect(fmtCountdown(2 * 60 * 60_000)).toBe('2h')
    })

    it('zero renders as 0m, not an empty/negative string', () => {
        expect(fmtCountdown(0)).toBe('0m')
    })
})

describe('computePreviewTiming', () => {
    it('is not live and has no countdown when there is no program at all', () => {
        const t = computePreviewTiming(null, 1000)
        expect(t.isLive).toBe(false)
        expect(t.startsInMs).toBeNull()
    })

    it('is live when now is strictly between start and end', () => {
        const p = program({wall_clock_start_ms: 1000, wall_clock_end_ms: 5000})
        const t = computePreviewTiming(p, 3000)
        expect(t.isLive).toBe(true)
        expect(t.startsInMs).toBeNull()
    })

    it('is live at the exact start boundary (inclusive)', () => {
        const p = program({wall_clock_start_ms: 1000, wall_clock_end_ms: 5000})
        const t = computePreviewTiming(p, 1000)
        expect(t.isLive).toBe(true)
    })

    it('is NOT live at the exact end boundary (exclusive)', () => {
        const p = program({wall_clock_start_ms: 1000, wall_clock_end_ms: 5000})
        const t = computePreviewTiming(p, 5000)
        expect(t.isLive).toBe(false)
        // Already over, not upcoming — no countdown either.
        expect(t.startsInMs).toBeNull()
    })

    it('one ms before the end boundary is still live', () => {
        const p = program({wall_clock_start_ms: 1000, wall_clock_end_ms: 5000})
        const t = computePreviewTiming(p, 4999)
        expect(t.isLive).toBe(true)
    })

    it('a future program is not live and has a positive countdown', () => {
        const p = program({wall_clock_start_ms: 10_000, wall_clock_end_ms: 20_000})
        const t = computePreviewTiming(p, 4_000)
        expect(t.isLive).toBe(false)
        expect(t.startsInMs).toBe(6_000)
    })

    it('a past (already-ended) program is not live and has no countdown', () => {
        const p = program({wall_clock_start_ms: 1000, wall_clock_end_ms: 2000})
        const t = computePreviewTiming(p, 9000)
        expect(t.isLive).toBe(false)
        expect(t.startsInMs).toBeNull()
    })

    it('one ms before a future program starts still counts down (not live yet)', () => {
        const p = program({wall_clock_start_ms: 10_000, wall_clock_end_ms: 20_000})
        const t = computePreviewTiming(p, 9_999)
        expect(t.isLive).toBe(false)
        expect(t.startsInMs).toBe(1)
    })
})
