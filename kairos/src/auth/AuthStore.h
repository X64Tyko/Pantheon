#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Database;

struct AuthUser
{
	std::string user_id;
	std::string username;
	std::string role; // "admin" | "viewer" — forced to "viewer" for a 'cast'-purpose
	// session regardless of the account's real role; see validate().
	// Parental controls — see RatingSeverity.h. restricted=false (the default)
	// means this account sees everything; the ceilings only apply when true.
	bool restricted                = false;
	std::string max_tv_rating      = "TV-Y";
	std::string max_movie_rating   = "G";
	std::string max_channel_rating = "TV-Y";
	// Set on invite-created accounts until the owner replaces the temp/
	// placeholder password with one of their own choosing (updateUser and
	// claimInvite both clear it on a successful password change).
	bool must_change_password = false;
	// True if a PIN is configured for the profile-switch flow (see
	// switchProfile) — never the PIN itself, just whether the picker should
	// prompt for one.
	bool has_pin = false;
	// Library-wide fallback audio/subtitle language, used by
	// GET /api/playback/:content_type/:id whenever no item-specific
	// preference exists (show_track_preference / movie_track_preference) —
	// see migration v94. Empty means "no preference set," same convention
	// as show/movie's own preferred_language.
	std::string default_audio_lang;
	std::string default_subtitle_lang;
	// Which page this account lands on after login/profile-switch —
	// '' (inherit the global app_config default), 'home', or 'guide'. See
	// migration v96 / ConfigService.cpp's own global "default_landing_page"
	// key for the tier this falls back to when empty.
	std::string default_landing_page;
	// A self-created, passwordless demo-server account (see
	// AuthStore::createGuestUser) — always role="viewer", never has a
	// password. Drives the "Guest" badge/self-service routes in Hades and
	// the guest-only self-service routes in AuthService.cpp.
	bool is_guest = false;
	// Per-account admin grant — lets this non-admin user create/edit their own
	// channel via the channel builder (see channel_auth::canEditChannel).
	// Same shape as `restricted`: an individual account flag, not a
	// server-wide setting, since real named accounts are provisioned
	// individually (unlike guests — see guest_settings::channelBuilderEnabled
	// for their server-wide equivalent). Meaningless for a guest account,
	// which uses that separate guest-wide toggle instead.
	bool channel_builder_enabled = false;
	// Most recent session.last_seen across every session this account has
	// ever held, or created_at if it's never had one — only populated by
	// listUsers() (for the admin Users page's "last active" column), not by
	// validate() (irrelevant to a request's own auth check). 0 if unknown.
	int64_t last_seen = 0;
};

// One active session, as surfaced to the owning user for review/revocation.
// Deliberately excludes the raw token — session_id is a separate, non-secret
// handle minted alongside it (see AuthStore::mintCastToken).
struct SessionInfo
{
	std::string session_id;
	int64_t created_at = 0;
	int64_t last_seen  = 0;
};

class AuthStore
{
public:
	explicit AuthStore(Database& db);

	bool hasAnyUser() const;

	// Returns false if username is already taken.
	bool createUser(const std::string& username,
					const std::string& password,
					const std::string& role);

	// Creates an account with a server-generated temp password instead of one
	// the admin typed — must_change_password is set so the account is gated
	// (see Router/Hades app-shell) until it's replaced. Returns
	// {user_id, temp_password} on success, {"",""} on failure (e.g. username
	// taken). The plaintext temp password is returned exactly once here and
	// never persisted or logged — callers must relay it out-of-band and must
	// not hold onto it longer than needed to display it once.
	std::pair<std::string, std::string> createUserWithTempPassword(
		const std::string& username, const std::string& role);

	// Creates an account nobody can log into yet — password_hash is an
	// unguessable random value discarded immediately after hashing — and
	// mints a single-use, expiring invite token for the email-invite claim
	// flow (see claimInvite). Returns {user_id, invite_token} on success,
	// {"",""} on failure.
	std::pair<std::string, std::string> createUserWithEmailInvite(
		const std::string& username, const std::string& role,
		int64_t invite_ttl_seconds = 7LL * 24 * 3600);

	// Username for a valid (unexpired, unused) invite token, or nullopt.
	std::optional<std::string> getInviteUsername(const std::string& invite_token) const;

	// Validates the token, sets the chosen password, clears
	// must_change_password, marks the token used, and mints a normal login
	// session. Returns the session token on success, "" on failure (invalid,
	// expired, or already-claimed token).
	std::string claimInvite(const std::string& invite_token, const std::string& new_password);

	// Re-issues a fresh invite token for an account that never claimed its
	// first one (the old token is left to expire naturally, not explicitly
	// invalidated). Returns the new token, or "" if the user doesn't exist or
	// no longer needs one (must_change_password already cleared).
	std::string resendInvite(const std::string& user_id,
							 int64_t invite_ttl_seconds = 7LL * 24 * 3600);

	// Returns session token on success, empty string on bad credentials.
	std::string login(const std::string& username, const std::string& password);

	void logout(const std::string& token);

	// Validates token, refreshes last_seen, returns user if valid and unexpired.
	std::optional<AuthUser> validate(const std::string& token);

	std::vector<AuthUser> listUsers() const;

	// Returns false if user not found or would delete last admin.
	bool deleteUser(const std::string& user_id, const std::string& requesting_user_id);

	// Partial update — only fields that are non-empty are applied.
	bool updateUser(const std::string& user_id,
					const std::string& new_password,
					const std::string& new_role);

	// Parental-controls settings for one account — separate from updateUser
	// since it's a distinct concern (restriction tier vs credentials/role) and
	// the Users page can update it without resending password/role each time.
	void updateRestriction(const std::string& user_id, bool restricted,
						   const std::string& max_tv_rating,
						   const std::string& max_movie_rating,
						   const std::string& max_channel_rating);

	// Admin grant of channel-builder access to one named account — separate
	// concern from updateRestriction, same "own PATCH so the Users page can
	// save one thing without resending another" reasoning.
	void updateChannelBuilderEnabled(const std::string& user_id, bool enabled);

	// Self-service, unlike the admin-only settings above — any logged-in
	// user calls this on their own user_id (see AuthService.cpp's
	// PATCH /api/users/me/track-preference). Empty string clears that side,
	// same "absent means don't touch, empty string means clear" convention
	// PUT /api/episodes/:id/track-preference already uses for
	// ShowTrackPreferenceRepository — std::optional here for the same reason.
	void updateTrackPreference(const std::string& user_id,
							   const std::optional<std::string>& audio_lang,
							   const std::optional<std::string>& subtitle_lang);

	// Self-service — same shape as updateTrackPreference, its own route since
	// "landing page" isn't a track-preference concern. Empty string means
	// "inherit the global default" (see default_landing_page's own comment).
	void updateDefaultLandingPage(const std::string& user_id, const std::string& page);

	// Mints a session tagged purpose='cast' — validate() forces its role to
	// "viewer" unconditionally, regardless of the calling account's real
	// role, so a Cast receiver holding this token can never act as admin
	// even if it leaks. Returns {token, session_id}: token is handed to the
	// receiver once and never stored/returned again; session_id is the
	// non-secret handle listSessions()/revokeSession() operate on.
	std::pair<std::string, std::string> mintCastToken(const std::string& user_id);

	std::vector<SessionInfo> listSessions(const std::string& user_id, const std::string& purpose) const;

	// Scoped to the owning user — returns false if no such session_id exists
	// for this user_id (never lets one account revoke another's session).
	bool revokeSession(const std::string& user_id, const std::string& session_id);

	// Profile-switch PIN (Netflix/Plex "Home" style) — a device that already
	// holds a valid session from a real username/password login can hop
	// between profiles via switchProfile() below without re-entering
	// credentials. Rejects pins that aren't 4-6 digits. Setting a new pin
	// clears any existing lockout/fail-count.
	bool setPin(const std::string& user_id, const std::string& pin);
	void clearPin(const std::string& user_id);

	// Mints a new login session for target_user_id, authorized by the
	// caller's own already-valid session rather than target_user_id's
	// password. admin-role targets always require their pin (set one first
	// via setPin — there's no way to switch into an unlocked admin profile).
	// viewer-role targets with no pin configured switch immediately, exactly
	// like an unlocked Netflix profile. Wrong pins count against a per-user
	// lockout (5 failures -> 60s cooldown, see AuthStore.cpp). Returns
	// {"", reason} on failure, {token, ""} on success.
	//
	// require_password_for_admin (see security_settings::
	// requireAdminPasswordSwitchEffective, resolved by the caller — this
	// method has no config access of its own) — when true, an admin-role
	// target is refused here even if it has a PIN configured: a 4-6 digit
	// PIN is a meaningfully weaker credential than the real password, and
	// this closes that gap for servers where the "who's watching?" picker
	// might be reached by a less-trusted device (e.g. any guest profile,
	// once guest profiles exist). Enforced here, not just left to the caller
	// to decide whether to prompt for a PIN or a password, so it can't be
	// bypassed by calling this endpoint directly.
	std::pair<std::string, std::string> switchProfile(const std::string& target_user_id,
													  const std::string& pin,
													  bool require_password_for_admin = false);

	// ── Guest profiles (demo-server "Continue as Guest") ────────────────────

	// Creates a passwordless, viewer-only account (display_name doubles as
	// username — same UNIQUE-constrained taken-name rejection as any other
	// account) and immediately mints a session for it, same shape login()
	// returns. {"", ""} on failure (name taken). Callers are responsible for
	// checking the guest_profiles_enabled setting and countActiveGuests()
	// against the configured cap first — kept as separate, independently
	// testable pieces rather than folded into this one method.
	std::pair<std::string, std::string> createGuestUser(const std::string& display_name);

	int countActiveGuests() const;

	// Guest-only self-service first-run setup — lets a guest configure the
	// same parental-control fields an admin can set for anyone else (see
	// updateRestriction) plus their own PIN, on themselves. Deliberately its
	// own method rather than a loosened version of the admin-only routes
	// above: scoping self-service restriction editing to guests specifically
	// means a *non-guest* viewer can never use it to loosen restrictions an
	// admin set on them. Every field is optional — only supplied ones change.
	void updateGuestSelfSetup(const std::string& user_id,
							  const std::optional<std::string>& pin,
							  const std::optional<bool>& restricted,
							  const std::optional<std::string>& max_tv_rating,
							  const std::optional<std::string>& max_movie_rating,
							  const std::optional<std::string>& max_channel_rating);

	// Deletes user_id's account iff it is actually a guest (is_guest=1 is
	// checked in the DELETE's own WHERE clause — defense in depth so this can
	// never delete a non-guest account even if a route-level gate above it
	// were ever wrong). Returns false if user_id doesn't exist or isn't a
	// guest.
	bool deleteGuestSelf(const std::string& user_id);

	// Deletes every guest account idle for at least idle_seconds — no
	// session touched within that window (see session.last_seen), or one
	// that's never had a session touched at all since created_at. Returns
	// the number of accounts pruned. Meant to be called periodically (see
	// JobScheduler) rather than per-request.
	int pruneIdleGuests(int64_t idle_seconds);

private:
	Database& db_;

	// Shared INSERT for all creation paths (createUser,
	// createUserWithTempPassword, createUserWithEmailInvite, createGuestUser)
	// — returns the new user_id, or "" if username is taken or role is
	// invalid.
	std::string insertUser(const std::string& username, const std::string& password_hash,
						   const std::string& role, bool must_change_password,
						   bool is_guest = false);

	// Shared session-row insert — factored out of login() so createGuestUser
	// (which has no password to check, so can't go through login() itself)
	// can mint a session the same way. Returns the new token.
	std::string insertSession(const std::string& user_id, const std::string& purpose = "login");

	// Deletes user_id's rows from every table that references user_id
	// WITHOUT an ON DELETE CASCADE (playback_history, show_track_preference,
	// movie_track_preference, watch_together_session.host_user_id,
	// watch_together_member — see Database.cpp's migrations v91/92/94/95),
	// then the user row itself (which cascades everything else: session,
	// watch_progress, restriction overrides, playlist-related rows, etc.).
	// Without this, deleting an account that has ever played anything or
	// hosted/joined a Watch Together session throws a foreign-key constraint
	// violation instead of actually deleting it — shared by the admin
	// deleteUser() below and the guest self-delete/prune paths, all of which
	// hit the exact same constraint.
	void deleteUserCascade(const std::string& user_id);

	static std::string hashPassword(const std::string& password);
	static bool checkPassword(const std::string& password,
							  const std::string& hash);
	static std::string generateToken();
	static std::string generateBcryptSalt();
	static bool timingSafeEqual(const std::string& a, const std::string& b);
};