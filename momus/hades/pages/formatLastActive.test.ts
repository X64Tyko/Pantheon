import {describe, it, expect} from 'vitest'
import {formatLastActive} from '@/pages/UsersPage'

describe('formatLastActive', () => {
    it('returns "never" for a guest with no session at all (last_seen=0)', () => {
        expect(formatLastActive(0)).toBe('never')
    })

    it('returns "just now" under the 60s boundary', () => {
        const now = Date.now() / 1000
        expect(formatLastActive(now - 59)).toBe('just now')
    })

    it('rolls over to minutes exactly at the 60s boundary', () => {
        const now = Date.now() / 1000
        expect(formatLastActive(now - 60)).toBe('1m ago')
    })

    it('stays in minutes just under the 1h boundary', () => {
        const now = Date.now() / 1000
        expect(formatLastActive(now - 3599)).toBe('59m ago')
    })

    it('rolls over to hours exactly at the 1h boundary', () => {
        const now = Date.now() / 1000
        expect(formatLastActive(now - 3600)).toBe('1h ago')
    })

    it('stays in hours just under the 24h boundary', () => {
        const now = Date.now() / 1000
        expect(formatLastActive(now - 86399)).toBe('23h ago')
    })

    it('rolls over to days exactly at the 24h boundary', () => {
        const now = Date.now() / 1000
        expect(formatLastActive(now - 86400)).toBe('1d ago')
    })

    it('handles a multi-day gap', () => {
        const now = Date.now() / 1000
        expect(formatLastActive(now - 86400 * 9)).toBe('9d ago')
    })
})
