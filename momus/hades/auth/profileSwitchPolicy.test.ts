import {describe, it, expect} from 'vitest'
import {requiresPasswordForAdminSwitch} from '@/auth/profileSwitchPolicy'

describe('requiresPasswordForAdminSwitch', () => {
    it('an admin with no PIN always requires password, regardless of the hardening setting', () => {
        expect(requiresPasswordForAdminSwitch('admin', false, false)).toBe(true)
        expect(requiresPasswordForAdminSwitch('admin', false, true)).toBe(true)
    })

    it('an admin with a PIN requires password only once the hardening setting is on', () => {
        expect(requiresPasswordForAdminSwitch('admin', true, false)).toBe(false)
        expect(requiresPasswordForAdminSwitch('admin', true, true)).toBe(true)
    })

    it('a viewer is never routed to password, with or without a PIN, regardless of the hardening setting', () => {
        expect(requiresPasswordForAdminSwitch('viewer', false, false)).toBe(false)
        expect(requiresPasswordForAdminSwitch('viewer', false, true)).toBe(false)
        expect(requiresPasswordForAdminSwitch('viewer', true, false)).toBe(false)
        expect(requiresPasswordForAdminSwitch('viewer', true, true)).toBe(false)
    })
})
