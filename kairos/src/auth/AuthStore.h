#pragma once
#include <optional>
#include <string>
#include <vector>

class Database;

struct AuthUser {
	std::string user_id;
	std::string username;
	std::string role;   // "admin" | "viewer"
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

private:
	Database& db_;

	static std::string hashPassword(const std::string& password);
	static bool        checkPassword(const std::string& password,
	                                 const std::string& hash);
	static std::string generateToken();
	static std::string generateBcryptSalt();
	static bool        timingSafeEqual(const std::string& a, const std::string& b);
};
