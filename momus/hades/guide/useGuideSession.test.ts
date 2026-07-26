import {describe, it, expect} from 'vitest'
import {findLiveProgram} from '@/guide/useGuideSession'
import type {EpgProgram} from '@/api/types'

// useGuideSession orchestrates channel/EPG fetching, focus state, and the
// preview-session lifecycle behind a React hook — with no jsdom/RTL in this
// repo (momus/hades/setup.ts), the hook itself can't be rendered. Its two
// genuinely pure pieces are covered directly instead: findLiveProgram here
// (the "now program" derivation), and PreviewSessionController's full
// start/switch/queue/self-stop state machine in
// previewSessionController.test.ts (extracted from this file's old
// beginPreview/stopCurrentPreview refs+closures specifically so it could be
// unit-tested this way). api.getChannels()/getChannelEpg() fetching itself
// is a thin, untested pass-through (see client.test.ts for API-layer
// coverage) and the debounce/visibilitychange wiring is DOM-effect glue with
// no decision logic of its own beyond what's already covered above.

function program(overrides: Partial<EpgProgram> = {}): EpgProgram {
    return {
        item_type: 'movie',
        item_id: 'm1',
        block_id: 'b1',
        title: 'Test Movie',
        duration_ms: 1000,
        wall_clock_start_ms: 0,
        wall_clock_end_ms: 1000,
        status: 'scheduled',
        ...overrides,
    }
}

describe('findLiveProgram', () => {
    it('returns null for an empty schedule', () => {
        expect(findLiveProgram([], 500)).toBeNull()
    })

    it('returns null when nothing is airing at nowMs (gap in schedule)', () => {
        const programs = [
            program({item_id: 'a', wall_clock_start_ms: 0, wall_clock_end_ms: 1000}),
            program({item_id: 'b', wall_clock_start_ms: 2000, wall_clock_end_ms: 3000}),
        ]
        expect(findLiveProgram(programs, 1500)).toBeNull()
    })

    it('finds the single program covering nowMs', () => {
        const programs = [
            program({item_id: 'a', wall_clock_start_ms: 0, wall_clock_end_ms: 1000}),
            program({item_id: 'b', wall_clock_start_ms: 1000, wall_clock_end_ms: 2000}),
        ]
        expect(findLiveProgram(programs, 1500)?.item_id).toBe('b')
    })

    it('start boundary is inclusive', () => {
        const programs = [program({item_id: 'a', wall_clock_start_ms: 1000, wall_clock_end_ms: 2000})]
        expect(findLiveProgram(programs, 1000)?.item_id).toBe('a')
    })

    it('end boundary is exclusive — back-to-back programs never both match', () => {
        const programs = [
            program({item_id: 'a', wall_clock_start_ms: 0, wall_clock_end_ms: 1000}),
            program({item_id: 'b', wall_clock_start_ms: 1000, wall_clock_end_ms: 2000}),
        ]
        // At the exact handoff instant, only the *next* program should claim it.
        expect(findLiveProgram(programs, 1000)?.item_id).toBe('b')
        // And the finishing one must not also match at that same instant.
        const onlyFirst = [programs[0]]
        expect(findLiveProgram(onlyFirst, 1000)).toBeNull()
    })

    it('picks the first match when the schedule has an (invalid) overlap', () => {
        const programs = [
            program({item_id: 'a', wall_clock_start_ms: 0, wall_clock_end_ms: 2000}),
            program({item_id: 'b', wall_clock_start_ms: 500, wall_clock_end_ms: 1500}),
        ]
        expect(findLiveProgram(programs, 1000)?.item_id).toBe('a')
    })
})
