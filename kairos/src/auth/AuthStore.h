#pragma once
#include <optional>
#include <string>
#include <vector>

class Database;

struct AuthUser {
	std::string user_id;
	std::string username;
	std::string role;   // "admin" | "viewer"
	// Parental controls — see RatingSeverity.h. restricted=false (the default)
	// means this account sees everything; the ceilings only apply when true.
	bool        restricted         = false;
	std::string max_tv_rating      = "TV-Y";
	std::string max_movie_rating   = "G";
	std::string max_channel_rating = "TV-Y";
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

private:
	Database& db_;

	static std::string hashPassword(const std::string& password);
	static bool        checkPassword(const std::string& password,
	                                 const std::string& hash);
	static std::string generateToken();
	static std::string generateBcryptSalt();
	static bool        timingSafeEqual(const std::string& a, const std::string& b);
};
