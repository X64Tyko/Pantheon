#include "AuthStore.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <crypt.h>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

// Session lifetime: 30 days.
static constexpr int64_t SESSION_TTL = 30LL * 24 * 3600;

// bcrypt cost factor.
static constexpr int BCRYPT_COST = 12;

// ---------------------------------------------------------------------------

AuthStore::AuthStore(Database& db) : db_(db) {}

// ---------------------------------------------------------------------------

bool AuthStore::hasAnyUser() const {
	SQLite::Statement q(db_.get(), "SELECT 1 FROM user LIMIT 1");
	return q.executeStep();
}

std::string AuthStore::insertUser(const std::string& username, const std::string& password_hash,
                                  const std::string& role, bool must_change_password) {
	if (username.empty()) return "";
	if (role != "admin" && role != "viewer") return "";

	const std::string user_id = generateToken().substr(0, 16);
	const int64_t     now     = static_cast<int64_t>(std::time(nullptr));

	try {
		SQLite::Statement ins(db_.get(),
			"INSERT INTO user (user_id, username, password_hash, role, created_at, must_change_password)"
			" VALUES (?,?,?,?,?,?)");
		ins.bind(1, user_id);
		ins.bind(2, username);
		ins.bind(3, password_hash);
		ins.bind(4, role);
		ins.bind(5, now);
		ins.bind(6, must_change_password ? 1 : 0);
		ins.exec();
		return user_id;
	} catch (const SQLite::Exception&) {
		return "";   // UNIQUE constraint on username
	}
}

bool AuthStore::createUser(const std::string& username,
                           const std::string& password,
                           const std::string& role) {
	if (password.empty()) return false;
	return !insertUser(username, hashPassword(password), role, /*must_change_password=*/false).empty();
}

std::pair<std::string, std::string> AuthStore::createUserWithTempPassword(
    const std::string& username, const std::string& role) {
	// Random plaintext, hex from the same CSPRNG source as session tokens —
	// plenty of entropy for a one-time credential the admin relays and the
	// user immediately replaces.
	const std::string temp_password = generateToken().substr(0, 20);
	const std::string user_id = insertUser(username, hashPassword(temp_password), role,
	                                        /*must_change_password=*/true);
	if (user_id.empty()) return {"", ""};
	return {user_id, temp_password};
}

std::pair<std::string, std::string> AuthStore::createUserWithEmailInvite(
    const std::string& username, const std::string& role, int64_t invite_ttl_seconds) {
	// Unguessable placeholder — hashed and the plaintext immediately
	// discarded, so nobody (including this process) ever knows a usable
	// password for the account until claimInvite() sets a real one.
	const std::string placeholder_hash = hashPassword(generateToken());
	const std::string user_id = insertUser(username, placeholder_hash, role,
	                                        /*must_change_password=*/true);
	if (user_id.empty()) return {"", ""};

	const std::string invite_token = generateToken();
	const int64_t     now          = static_cast<int64_t>(std::time(nullptr));
	SQLite::Statement ins(db_.get(),
		"INSERT INTO user_invite (invite_id, user_id, created_at, expires_at) VALUES (?,?,?,?)");
	ins.bind(1, invite_token);
	ins.bind(2, user_id);
	ins.bind(3, now);
	ins.bind(4, now + invite_ttl_seconds);
	ins.exec();

	return {user_id, invite_token};
}

std::optional<std::string> AuthStore::getInviteUsername(const std::string& invite_token) const {
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	SQLite::Statement q(db_.get(), R"(
		SELECT u.username FROM user_invite i
		JOIN user u ON u.user_id = i.user_id
		WHERE i.invite_id = ? AND i.used_at IS NULL AND i.expires_at > ?
	)");
	q.bind(1, invite_token);
	q.bind(2, now);
	if (!q.executeStep()) return std::nullopt;
	return q.getColumn(0).getString();
}

std::string AuthStore::claimInvite(const std::string& invite_token, const std::string& new_password) {
	if (new_password.empty()) return "";
	const int64_t now = static_cast<int64_t>(std::time(nullptr));

	SQLite::Statement q(db_.get(), R"(
		SELECT user_id FROM user_invite WHERE invite_id = ? AND used_at IS NULL AND expires_at > ?
	)");
	q.bind(1, invite_token);
	q.bind(2, now);
	if (!q.executeStep()) return "";
	const std::string user_id = q.getColumn(0).getString();

	SQLite::Statement upd(db_.get(),
		"UPDATE user SET password_hash = ?, must_change_password = 0 WHERE user_id = ?");
	upd.bind(1, hashPassword(new_password));
	upd.bind(2, user_id);
	upd.exec();

	SQLite::Statement mark(db_.get(), "UPDATE user_invite SET used_at = ? WHERE invite_id = ?");
	mark.bind(1, now);
	mark.bind(2, invite_token);
	mark.exec();

	const std::string token = generateToken();
	SQLite::Statement ins(db_.get(),
		"INSERT INTO session (token, user_id, created_at, expires_at, last_seen) VALUES (?,?,?,?,?)");
	ins.bind(1, token);
	ins.bind(2, user_id);
	ins.bind(3, now);
	ins.bind(4, now + SESSION_TTL);
	ins.bind(5, now);
	ins.exec();

	return token;
}

std::string AuthStore::resendInvite(const std::string& user_id, int64_t invite_ttl_seconds) {
	SQLite::Statement q(db_.get(), "SELECT must_change_password FROM user WHERE user_id = ?");
	q.bind(1, user_id);
	if (!q.executeStep() || q.getColumn(0).getInt() == 0) return "";

	const std::string invite_token = generateToken();
	const int64_t     now          = static_cast<int64_t>(std::time(nullptr));
	SQLite::Statement ins(db_.get(),
		"INSERT INTO user_invite (invite_id, user_id, created_at, expires_at) VALUES (?,?,?,?)");
	ins.bind(1, invite_token);
	ins.bind(2, user_id);
	ins.bind(3, now);
	ins.bind(4, now + invite_ttl_seconds);
	ins.exec();
	return invite_token;
}

std::string AuthStore::login(const std::string& username,
                             const std::string& password) {
	SQLite::Statement q(db_.get(),
		"SELECT user_id, password_hash FROM user WHERE username = ?");
	q.bind(1, username);
	if (!q.executeStep()) return "";

	const std::string user_id = q.getColumn(0).getString();
	const std::string stored  = q.getColumn(1).getString();

	if (!checkPassword(password, stored)) return "";

	const std::string token = generateToken();
	const int64_t     now   = static_cast<int64_t>(std::time(nullptr));

	SQLite::Statement ins(db_.get(),
		"INSERT INTO session (token, user_id, created_at, expires_at, last_seen)"
		" VALUES (?,?,?,?,?)");
	ins.bind(1, token);
	ins.bind(2, user_id);
	ins.bind(3, now);
	ins.bind(4, now + SESSION_TTL);
	ins.bind(5, now);
	ins.exec();

	return token;
}

void AuthStore::logout(const std::string& token) {
	SQLite::Statement d(db_.get(), "DELETE FROM session WHERE token = ?");
	d.bind(1, token);
	d.exec();
}

std::optional<AuthUser> AuthStore::validate(const std::string& token) {
	if (token.empty()) return std::nullopt;

	const int64_t now = static_cast<int64_t>(std::time(nullptr));

	SQLite::Statement q(db_.get(), R"(
		SELECT u.user_id, u.username, u.role,
		       u.restricted, u.max_tv_rating, u.max_movie_rating, u.max_channel_rating,
		       s.purpose, u.must_change_password
		FROM session s
		JOIN user u ON u.user_id = s.user_id
		WHERE s.token = ? AND s.expires_at > ?
	)");
	q.bind(1, token);
	q.bind(2, now);
	if (!q.executeStep()) return std::nullopt;

	AuthUser user;
	user.user_id            = q.getColumn(0).getString();
	user.username           = q.getColumn(1).getString();
	user.role                = q.getColumn(2).getString();
	user.restricted          = q.getColumn(3).getInt() != 0;
	user.max_tv_rating       = q.getColumn(4).getString();
	user.max_movie_rating    = q.getColumn(5).getString();
	user.max_channel_rating  = q.getColumn(6).getString();
	user.must_change_password = q.getColumn(8).getInt() != 0;

	// A 'cast'-purpose session (minted for handing off to a Cast receiver,
	// see mintCastToken) is always viewer-capped — this is the actual
	// security boundary, independent of the client-sent
	// "X-Pantheon-Surface: tv" downgrade header, which only works if the
	// holder cooperates with sending it.
	if (q.getColumn(7).getString() == "cast") user.role = "viewer";

	// Best-effort bookkeeping only (session expiry is driven by expires_at,
	// checked above, not last_seen) — every authenticated request runs this in
	// the pre-routing handler, so a write-lock collision with a sync in
	// progress must not fail the whole request with an uncaught SQLITE_BUSY.
	try {
		SQLite::Statement upd(db_.get(),
			"UPDATE session SET last_seen = ? WHERE token = ?");
		upd.bind(1, now);
		upd.bind(2, token);
		upd.exec();
	} catch (const SQLite::Exception&) {}

	return user;
}

std::vector<AuthUser> AuthStore::listUsers() const {
	SQLite::Statement q(db_.get(),
		"SELECT user_id, username, role, "
		"       restricted, max_tv_rating, max_movie_rating, max_channel_rating, must_change_password "
		"FROM user ORDER BY username");
	std::vector<AuthUser> result;
	while (q.executeStep()) {
		result.push_back({
			q.getColumn(0).getString(),
			q.getColumn(1).getString(),
			q.getColumn(2).getString(),
			q.getColumn(3).getInt() != 0,
			q.getColumn(4).getString(),
			q.getColumn(5).getString(),
			q.getColumn(6).getString(),
			q.getColumn(7).getInt() != 0,
		});
	}
	return result;
}

bool AuthStore::deleteUser(const std::string& user_id,
                           const std::string& requesting_user_id) {
	if (user_id == requesting_user_id) return false;

	// Refuse if this would remove the last admin.
	SQLite::Statement adminCheck(db_.get(),
		"SELECT COUNT(*) FROM user WHERE role = 'admin' AND user_id != ?");
	adminCheck.bind(1, user_id);
	adminCheck.executeStep();
	SQLite::Statement roleCheck(db_.get(),
		"SELECT role FROM user WHERE user_id = ?");
	roleCheck.bind(1, user_id);
	if (!roleCheck.executeStep()) return false;
	if (roleCheck.getColumn(0).getString() == "admin") {
		adminCheck.reset();
		adminCheck.executeStep();
		if (adminCheck.getColumn(0).getInt() == 0) return false;
	}

	SQLite::Statement d(db_.get(), "DELETE FROM user WHERE user_id = ?");
	d.bind(1, user_id);
	d.exec();
	return db_.get().getChanges() > 0;
}

bool AuthStore::updateUser(const std::string& user_id,
                           const std::string& new_password,
                           const std::string& new_role) {
	if (!new_password.empty()) {
		const std::string hash = hashPassword(new_password);
		// Any successful password change — self-service or admin-initiated —
		// satisfies must_change_password, whether it was set (temp-password
		// invite path) or already clear (no-op update in that case).
		SQLite::Statement u(db_.get(),
			"UPDATE user SET password_hash = ?, must_change_password = 0 WHERE user_id = ?");
		u.bind(1, hash);
		u.bind(2, user_id);
		u.exec();
	}
	if (!new_role.empty()) {
		if (new_role != "admin" && new_role != "viewer") return false;
		SQLite::Statement u(db_.get(),
			"UPDATE user SET role = ? WHERE user_id = ?");
		u.bind(1, new_role);
		u.bind(2, user_id);
		u.exec();
	}
	return true;
}

void AuthStore::updateRestriction(const std::string& user_id, bool restricted,
                                  const std::string& max_tv_rating,
                                  const std::string& max_movie_rating,
                                  const std::string& max_channel_rating) {
	SQLite::Statement u(db_.get(), R"(
		UPDATE user SET restricted = ?, max_tv_rating = ?,
		                max_movie_rating = ?, max_channel_rating = ?
		WHERE user_id = ?
	)");
	u.bind(1, restricted ? 1 : 0);
	u.bind(2, max_tv_rating);
	u.bind(3, max_movie_rating);
	u.bind(4, max_channel_rating);
	u.bind(5, user_id);
	u.exec();
}

std::pair<std::string, std::string> AuthStore::mintCastToken(const std::string& user_id) {
	const std::string token      = generateToken();
	const std::string session_id = generateToken();
	const int64_t     now        = static_cast<int64_t>(std::time(nullptr));

	SQLite::Statement ins(db_.get(), R"(
		INSERT INTO session (token, user_id, created_at, expires_at, last_seen, purpose, session_id)
		VALUES (?, ?, ?, ?, ?, 'cast', ?)
	)");
	ins.bind(1, token);
	ins.bind(2, user_id);
	ins.bind(3, now);
	ins.bind(4, now + SESSION_TTL);
	ins.bind(5, now);
	ins.bind(6, session_id);
	ins.exec();

	return {token, session_id};
}

std::vector<SessionInfo> AuthStore::listSessions(const std::string& user_id, const std::string& purpose) const {
	SQLite::Statement q(db_.get(), R"(
		SELECT session_id, created_at, last_seen
		FROM session
		WHERE user_id = ? AND purpose = ? AND session_id IS NOT NULL
		ORDER BY created_at DESC
	)");
	q.bind(1, user_id);
	q.bind(2, purpose);

	std::vector<SessionInfo> out;
	while (q.executeStep()) {
		out.push_back({
			q.getColumn(0).getString(),
			q.getColumn(1).getInt64(),
			q.getColumn(2).getInt64(),
		});
	}
	return out;
}

bool AuthStore::revokeSession(const std::string& user_id, const std::string& session_id) {
	SQLite::Statement d(db_.get(), "DELETE FROM session WHERE user_id = ? AND session_id = ?");
	d.bind(1, user_id);
	d.bind(2, session_id);
	d.exec();
	return db_.get().getChanges() > 0;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string AuthStore::generateBcryptSalt() {
	// bcrypt salt: $2b$NN$ followed by 22 chars from bcrypt's base64 alphabet.
	static constexpr char B64[] =
		"./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

	unsigned char raw[16];
	{
		std::ifstream urandom("/dev/urandom", std::ios::binary);
		if (!urandom) throw std::runtime_error("cannot open /dev/urandom");
		urandom.read(reinterpret_cast<char*>(raw), sizeof(raw));
	}

	// Base64-encode 16 bytes → 22 bcrypt chars (bcrypt uses its own alphabet).
	// Each group of 3 bytes → 4 chars; 16 bytes → 21.333 → 22 chars with padding.
	std::string chars;
	chars.reserve(22);
	for (int i = 0; i < 15; i += 3) {
		uint32_t v = (raw[i] << 16) | (raw[i+1] << 8) | raw[i+2];
		chars += B64[(v >> 18) & 0x3f];
		chars += B64[(v >> 12) & 0x3f];
		chars += B64[(v >>  6) & 0x3f];
		chars += B64[(v      ) & 0x3f];
	}
	// Last byte (index 15)
	chars += B64[(raw[15] >> 2) & 0x3f];
	chars += B64[(raw[15] << 4) & 0x3f];
	chars.resize(22);

	std::ostringstream salt;
	salt << "$2b$" << std::setw(2) << std::setfill('0') << BCRYPT_COST << "$" << chars;
	return salt.str();
}

std::string AuthStore::hashPassword(const std::string& password) {
	const std::string salt = generateBcryptSalt();
	struct crypt_data cd{};
	const char* result = crypt_r(password.c_str(), salt.c_str(), &cd);
	if (!result) throw std::runtime_error("crypt_r failed");
	return std::string(result);
}

bool AuthStore::checkPassword(const std::string& password,
                              const std::string& stored_hash) {
	struct crypt_data cd{};
	const char* result = crypt_r(password.c_str(), stored_hash.c_str(), &cd);
	if (!result) return false;
	return timingSafeEqual(std::string(result), stored_hash);
}

std::string AuthStore::generateToken() {
	unsigned char raw[32];
	std::ifstream urandom("/dev/urandom", std::ios::binary);
	if (!urandom) throw std::runtime_error("cannot open /dev/urandom");
	urandom.read(reinterpret_cast<char*>(raw), sizeof(raw));

	std::ostringstream hex;
	hex << std::hex << std::setfill('0');
	for (unsigned char b : raw) hex << std::setw(2) << static_cast<int>(b);
	return hex.str();
}

bool AuthStore::timingSafeEqual(const std::string& a, const std::string& b) {
	// Constant-time comparison — always touches every byte.
	if (a.size() != b.size()) {
		// Still scan b to avoid timing differences on length mismatch.
		volatile char acc = 0;
		for (char c : b) acc |= c;
		(void)acc;
		return false;
	}
	volatile unsigned char diff = 0;
	for (size_t i = 0; i < a.size(); ++i)
		diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
	return diff == 0;
}
