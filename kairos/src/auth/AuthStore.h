#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Database;

struct AuthUser {
	std::string user_id;
	std::string username;
	std::string role;   // "admin" | "viewer" — forced to "viewer" for a 'cast'-purpose
	                    // session regardless of the account's real role; see validate().
	// Parental controls — see RatingSeverity.h. restricted=false (the default)
	// means this account sees everything; the ceilings only apply when true.
	bool        restricted         = false;
	std::string max_tv_rating      = "TV-Y";
	std::string max_movie_rating   = "G";
	std::string max_channel_rating = "TV-Y";
	// Set on invite-created accounts until the owner replaces the temp/
	// placeholder password with one of their own choosing (updateUser and
	// claimInvite both clear it on a successful password change).
	bool        must_change_password = false;
};

// One active session, as surfaced to the owning user for review/revocation.
// Deliberately excludes the raw token — session_id is a separate, non-secret
// handle minted alongside it (see AuthStore::mintCastToken).
struct SessionInfo {
	std::string session_id;
	int64_t     created_at = 0;
	int64_t     last_seen  = 0;
};

class AuthStore {
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

private:
	Database& db_;

	// Shared INSERT for all three creation paths (createUser,
	// createUserWithTempPassword, createUserWithEmailInvite) — returns the
	// new user_id, or "" if username is taken or role is invalid.
	std::string insertUser(const std::string& username, const std::string& password_hash,
	                       const std::string& role, bool must_change_password);

	static std::string hashPassword(const std::string& password);
	static bool        checkPassword(const std::string& password,
	                                 const std::string& hash);
	static std::string generateToken();
	static std::string generateBcryptSalt();
	static bool        timingSafeEqual(const std::string& a, const std::string& b);
};
