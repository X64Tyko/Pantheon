// Whether a profile-picker tile for this account must fall back to the real
// password login instead of attempting AuthStore::switchProfile's PIN path.
// Mirrors AuthStore::switchProfile's own server-side rule exactly (see
// AuthStore.cpp, gated on the same require_password_for_admin flag) — shared
// by auth/ProfileSelectPage.tsx and tv/TvProfileSelect.tsx so an
// admin-with-a-pin tile routes straight to the password form instead of
// showing a PIN prompt the server would reject anyway once
// requireAdminPasswordSwitch is on. A viewer tile is never affected by
// requireAdminPasswordSwitch — that setting only ever tightens the admin
// path, on the theory that a PIN is a meaningfully weaker credential than
// the real password specifically for an account that can do real damage.
export function requiresPasswordForAdminSwitch(
    role: string,
    hasPin: boolean,
    requireAdminPasswordSwitch: boolean,
): boolean {
    return role === 'admin' && (!hasPin || requireAdminPasswordSwitch)
}
