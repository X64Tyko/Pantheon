import {describe, it, expect} from 'vitest'
import {readFileSync} from 'node:fs'
import {fileURLToPath} from 'node:url'
import path from 'node:path'

// Regression guard for the direct_play -> direct_stream field rename (see
// CHANGELOG). NowPlayingPanel.tsx, PlayHistoryPanel.tsx, playbackApi.ts,
// usePlaybackSession.ts and api/types.ts all switched from reading Kairos/
// Hermes' old `direct_play` field to the renamed `direct_stream` one. None
// of these have component-level test coverage today (no jsdom in this
// suite's environment, and no existing test harness renders them), so
// rather than bolt on a large new component-test surface for narrow ROI,
// this asserts the old field name can't silently creep back into the
// specific files that were touched by the rename — a real regression this
// would have caught, at a fraction of the cost of full component tests.

const HADES_SRC = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../hades/src')

const RENAMED_FILES = [
    'components/activity/NowPlayingPanel.tsx',
    'components/activity/PlayHistoryPanel.tsx',
    'player/playbackApi.ts',
    'player/usePlaybackSession.ts',
    'api/types.ts',
    'api/client.ts',
]

describe('direct_play -> direct_stream rename', () => {
    it.each(RENAMED_FILES)('%s no longer references the old direct_play field name', (relPath) => {
        const source = readFileSync(path.join(HADES_SRC, relPath), 'utf-8')
        expect(source).not.toMatch(/direct_play/)
    })

    it.each([
        'components/activity/NowPlayingPanel.tsx',
        'components/activity/PlayHistoryPanel.tsx',
        'player/playbackApi.ts',
        'player/usePlaybackSession.ts',
    ])('%s does reference the renamed direct_stream field', (relPath) => {
        const source = readFileSync(path.join(HADES_SRC, relPath), 'utf-8')
        expect(source).toMatch(/direct_stream/)
    })
})
