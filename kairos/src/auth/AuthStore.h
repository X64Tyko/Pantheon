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

	static std::string hashPassword(const std::string& password);
	static bool        checkPassword(const std::string& password,
	                                 const std::string& hash);
	static std::string generateToken();
	static std::string generateBcryptSalt();
	static bool        timingSafeEqual(const std::string& a, const std::string& b);
};
