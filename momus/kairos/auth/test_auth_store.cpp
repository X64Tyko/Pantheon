#include <gtest/gtest.h>
#include "auth/AuthStore.h"
#include "db/Database.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <algorithm>
#include <ctime>
#include <string>

class AuthStoreTest : public ::testing::Test
{
protected:
	Database db{":memory:"};
	AuthStore auth{db};

	std::optional<AuthUser> findUser(const std::string& user_id)
	{
		for (const auto& u : auth.listUsers()) if (u.user_id == user_id) return u;
		return std::nullopt;
	}

	std::string userIdFor(const std::string& username)
	{
		for (const auto& u : auth.listUsers()) if (u.username == username) return u.user_id;
		return "";
	}
};

// ---------------------------------------------------------------------------
// createUserWithTempPassword
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, TempPassword_CreatesUsableAccount)
{
	auto [user_id, temp_password] = auth.createUserWithTempPassword("alice", "viewer");
	ASSERT_FALSE(user_id.empty());
	ASSERT_FALSE(temp_password.empty());

	// The generated temp password logs in via the normal, unmodified path.
	const std::string token = auth.login("alice", temp_password);
	EXPECT_FALSE(token.empty());
}

TEST_F(AuthStoreTest, TempPassword_SetsMustChangePassword)
{
	auto [user_id, temp_password] = auth.createUserWithTempPassword("bob", "viewer");
	ASSERT_FALSE(user_id.empty());
	auto u = findUser(user_id);
	ASSERT_TRUE(u.has_value());
	EXPECT_TRUE(u->must_change_password);
}

TEST_F(AuthStoreTest, TempPassword_DuplicateUsernameFails)
{
	ASSERT_FALSE(auth.createUserWithTempPassword("carol", "viewer").first.empty());
	auto [user_id, temp_password] = auth.createUserWithTempPassword("carol", "viewer");
	EXPECT_TRUE(user_id.empty());
	EXPECT_TRUE(temp_password.empty());
}

// ---------------------------------------------------------------------------
// createUserWithEmailInvite / claimInvite
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, EmailInvite_AccountUnusableUntilClaimed)
{
	auto [user_id, invite_token] = auth.createUserWithEmailInvite("dave", "viewer");
	ASSERT_FALSE(user_id.empty());
	ASSERT_FALSE(invite_token.empty());

	// Nobody — including this process — knows the placeholder password, so
	// every login attempt must fail until the invite is claimed.
	EXPECT_TRUE(auth.login("dave", "").empty());
	EXPECT_TRUE(auth.login("dave", "password").empty());

	auto u = findUser(user_id);
	ASSERT_TRUE(u.has_value());
	EXPECT_TRUE(u->must_change_password);
}

TEST_F(AuthStoreTest, EmailInvite_GetInviteUsernameReturnsUsernameForValidToken)
{
	auto [user_id, invite_token] = auth.createUserWithEmailInvite("erin", "viewer");
	ASSERT_FALSE(user_id.empty());
	auto username = auth.getInviteUsername(invite_token);
	ASSERT_TRUE(username.has_value());
	EXPECT_EQ(*username, "erin");
}

TEST_F(AuthStoreTest, EmailInvite_GetInviteUsernameNulloptForUnknownToken)
{
	EXPECT_FALSE(auth.getInviteUsername("not-a-real-token").has_value());
}

TEST_F(AuthStoreTest, EmailInvite_GetInviteUsernameNulloptForExpiredToken)
{
	auto [user_id, invite_token] = auth.createUserWithEmailInvite("frank", "viewer", /*invite_ttl_seconds=*/-10);
	ASSERT_FALSE(user_id.empty());
	EXPECT_FALSE(auth.getInviteUsername(invite_token).has_value());
}

TEST_F(AuthStoreTest, ClaimInvite_SetsPasswordClearsFlagAndLogsIn)
{
	auto [user_id, invite_token] = auth.createUserWithEmailInvite("grace", "viewer");
	ASSERT_FALSE(user_id.empty());

	const std::string session = auth.claimInvite(invite_token, "a-real-password");
	ASSERT_FALSE(session.empty());

	auto validated = auth.validate(session);
	ASSERT_TRUE(validated.has_value());
	EXPECT_EQ(validated->username, "grace");
	EXPECT_FALSE(validated->must_change_password);

	// The chosen password now works through the normal login path.
	EXPECT_FALSE(auth.login("grace", "a-real-password").empty());
}

TEST_F(AuthStoreTest, ClaimInvite_TokenIsSingleUse)
{
	auto [user_id, invite_token] = auth.createUserWithEmailInvite("heidi", "viewer");
	ASSERT_FALSE(user_id.empty());
	ASSERT_FALSE(auth.claimInvite(invite_token, "first-password").empty());

	// A second claim attempt with the same token must fail — the account
	// already has a real, self-chosen password at this point.
	EXPECT_TRUE(auth.claimInvite(invite_token, "second-password").empty());
}

TEST_F(AuthStoreTest, ClaimInvite_ExpiredTokenFails)
{
	auto [user_id, invite_token] = auth.createUserWithEmailInvite("ivan", "viewer", /*invite_ttl_seconds=*/-10);
	ASSERT_FALSE(user_id.empty());
	EXPECT_TRUE(auth.claimInvite(invite_token, "some-password").empty());
}

TEST_F(AuthStoreTest, ClaimInvite_UnknownTokenFails)
{
	EXPECT_TRUE(auth.claimInvite("not-a-real-token", "some-password").empty());
}

TEST_F(AuthStoreTest, ClaimInvite_EmptyPasswordRejected)
{
	auto [user_id, invite_token] = auth.createUserWithEmailInvite("judy", "viewer");
	ASSERT_FALSE(user_id.empty());
	EXPECT_TRUE(auth.claimInvite(invite_token, "").empty());
}

// ---------------------------------------------------------------------------
// resendInvite
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, ResendInvite_IssuesFreshTokenWhileStillPending)
{
	auto [user_id, first_token] = auth.createUserWithEmailInvite("kim", "viewer");
	ASSERT_FALSE(user_id.empty());

	const std::string second_token = auth.resendInvite(user_id);
	EXPECT_FALSE(second_token.empty());
	EXPECT_NE(second_token, first_token);

	// Both tokens resolve to the same account until one of them is claimed.
	EXPECT_TRUE(auth.getInviteUsername(first_token).has_value());
	EXPECT_TRUE(auth.getInviteUsername(second_token).has_value());
}

TEST_F(AuthStoreTest, ResendInvite_FailsOnceAccountHasARealPassword)
{
	auto [user_id, invite_token] = auth.createUserWithEmailInvite("liam", "viewer");
	ASSERT_FALSE(user_id.empty());
	ASSERT_FALSE(auth.claimInvite(invite_token, "chosen-password").empty());

	EXPECT_TRUE(auth.resendInvite(user_id).empty());
}

TEST_F(AuthStoreTest, ResendInvite_FailsForUnknownUser)
{
	EXPECT_TRUE(auth.resendInvite("no-such-user").empty());
}

// ---------------------------------------------------------------------------
// updateUser clearing must_change_password
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, UpdateUser_PasswordChangeClearsMustChangePassword)
{
	auto [user_id, temp_password] = auth.createUserWithTempPassword("mallory", "viewer");
	ASSERT_FALSE(user_id.empty());
	ASSERT_TRUE(findUser(user_id)->must_change_password);

	ASSERT_TRUE(auth.updateUser(user_id, "a-chosen-password", ""));
	EXPECT_FALSE(findUser(user_id)->must_change_password);

	// The new password works; the old temp one no longer does.
	EXPECT_FALSE(auth.login("mallory", "a-chosen-password").empty());
	EXPECT_TRUE(auth.login("mallory", temp_password).empty());
}

TEST_F(AuthStoreTest, UpdateUser_RoleOnlyChangeDoesNotDisturbFlag)
{
	auto [user_id, temp_password] = auth.createUserWithTempPassword("nolan", "viewer");
	ASSERT_FALSE(user_id.empty());

	ASSERT_TRUE(auth.updateUser(user_id, "", "admin"));
	// No password was supplied — must_change_password is still pending.
	EXPECT_TRUE(findUser(user_id)->must_change_password);
	EXPECT_EQ(findUser(user_id)->role, "admin");
}

// ---------------------------------------------------------------------------
// Plain createUser is unaffected by any of the above
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, PlainCreateUser_NeverSetsMustChangePassword)
{
	ASSERT_TRUE(auth.createUser("oscar", "chosen-password", "viewer"));
	auto users = auth.listUsers();
	auto it    = std::find_if(users.begin(), users.end(),
							  [](const AuthUser& u) { return u.username == "oscar"; });
	ASSERT_NE(it, users.end());
	EXPECT_FALSE(it->must_change_password);
}

// ---------------------------------------------------------------------------
// setPin / clearPin / switchProfile — Netflix/Plex-style profile switch
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, SetPin_RejectsNonNumericAndWrongLength)
{
	ASSERT_TRUE(auth.createUser("pat", "password", "viewer"));
	const std::string user_id = userIdFor("pat");

	EXPECT_FALSE(auth.setPin(user_id, "12a4"));    // non-digit
	EXPECT_FALSE(auth.setPin(user_id, "123"));     // too short
	EXPECT_FALSE(auth.setPin(user_id, "1234567")); // too long
	EXPECT_FALSE(findUser(user_id)->has_pin);
}

TEST_F(AuthStoreTest, SetPin_ValidPinMarksHasPin)
{
	ASSERT_TRUE(auth.createUser("quinn", "password", "viewer"));
	const std::string user_id = userIdFor("quinn");

	EXPECT_TRUE(auth.setPin(user_id, "4321"));
	EXPECT_TRUE(findUser(user_id)->has_pin);
}

TEST_F(AuthStoreTest, SwitchProfile_ViewerWithNoPinSwitchesImmediately)
{
	ASSERT_TRUE(auth.createUser("river", "password", "viewer"));
	const std::string user_id = userIdFor("river");

	auto [token, error] = auth.switchProfile(user_id, "");
	EXPECT_FALSE(token.empty());
	EXPECT_TRUE(error.empty());

	auto validated = auth.validate(token);
	ASSERT_TRUE(validated.has_value());
	EXPECT_EQ(validated->user_id, user_id);
}

TEST_F(AuthStoreTest, SwitchProfile_AdminWithoutPinIsDenied)
{
	ASSERT_TRUE(auth.createUser("sage", "password", "admin"));
	const std::string user_id = userIdFor("sage");

	auto [token, error] = auth.switchProfile(user_id, "");
	EXPECT_TRUE(token.empty());
	EXPECT_FALSE(error.empty());
}

TEST_F(AuthStoreTest, SwitchProfile_AdminWithPinSucceeds)
{
	ASSERT_TRUE(auth.createUser("tara", "password", "admin"));
	const std::string user_id = userIdFor("tara");
	ASSERT_TRUE(auth.setPin(user_id, "9999"));

	auto [token, error] = auth.switchProfile(user_id, "9999");
	EXPECT_FALSE(token.empty());
	EXPECT_TRUE(error.empty());
}

TEST_F(AuthStoreTest, SwitchProfile_WrongPinFailsWithoutMintingSession)
{
	ASSERT_TRUE(auth.createUser("uma", "password", "viewer"));
	const std::string user_id = userIdFor("uma");
	ASSERT_TRUE(auth.setPin(user_id, "1111"));

	auto [token, error] = auth.switchProfile(user_id, "0000");
	EXPECT_TRUE(token.empty());
	EXPECT_FALSE(error.empty());

	// The right pin still works — one wrong guess isn't itself a lockout.
	auto [token2, error2] = auth.switchProfile(user_id, "1111");
	EXPECT_FALSE(token2.empty());
}

TEST_F(AuthStoreTest, SwitchProfile_LocksOutAfterFiveWrongAttempts)
{
	ASSERT_TRUE(auth.createUser("vince", "password", "viewer"));
	const std::string user_id = userIdFor("vince");
	ASSERT_TRUE(auth.setPin(user_id, "2222"));

	for (int i = 0; i < 5; ++i)
	{
		auto [token, error] = auth.switchProfile(user_id, "0000");
		EXPECT_TRUE(token.empty());
	}

	// Even the correct pin is refused while locked out.
	auto [token, error] = auth.switchProfile(user_id, "2222");
	EXPECT_TRUE(token.empty());
	EXPECT_FALSE(error.empty());
}

TEST_F(AuthStoreTest, SwitchProfile_RecoversOncePinLockoutWindowExpires)
{
	ASSERT_TRUE(auth.createUser("wendy", "password", "viewer"));
	const std::string user_id = userIdFor("wendy");
	ASSERT_TRUE(auth.setPin(user_id, "3333"));

	for (int i = 0; i < 5; ++i) auth.switchProfile(user_id, "0000");
	ASSERT_TRUE(auth.switchProfile(user_id, "3333").first.empty()); // still locked

	// Simulate the lockout window having already elapsed, without a real sleep.
	SQLite::Statement upd(db.get(), "UPDATE user SET pin_locked_until = ? WHERE user_id = ?");
	upd.bind(1, static_cast<int64_t>(std::time(nullptr)) - 1);
	upd.bind(2, user_id);
	upd.exec();

	auto [token, error] = auth.switchProfile(user_id, "3333");
	EXPECT_FALSE(token.empty());
}

TEST_F(AuthStoreTest, ClearPin_RemovesGateAndAllowsImmediateSwitch)
{
	ASSERT_TRUE(auth.createUser("xena", "password", "viewer"));
	const std::string user_id = userIdFor("xena");
	ASSERT_TRUE(auth.setPin(user_id, "4444"));

	auth.clearPin(user_id);
	EXPECT_FALSE(findUser(user_id)->has_pin);

	auto [token, error] = auth.switchProfile(user_id, "");
	EXPECT_FALSE(token.empty());
}

TEST_F(AuthStoreTest, SwitchProfile_UnknownUserFails)
{
	auto [token, error] = auth.switchProfile("no-such-user", "");
	EXPECT_TRUE(token.empty());
	EXPECT_FALSE(error.empty());
}

// ---------------------------------------------------------------------------
// SwitchProfile — require_password_for_admin hardening. A PIN is a
// meaningfully weaker credential than the real password; when this is on,
// admin must never be switchable via PIN alone, even a correctly-configured
// one. Viewer profiles are completely unaffected either way.
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, SwitchProfile_RequirePasswordDeniesAdminEvenWithACorrectPin)
{
	ASSERT_TRUE(auth.createUser("hardened_admin", "password", "admin"));
	const std::string admin_id = userIdFor("hardened_admin");
	ASSERT_TRUE(auth.setPin(admin_id, "1234"));

	auto [token, error] = auth.switchProfile(admin_id, "1234", /*require_password_for_admin=*/true);
	EXPECT_TRUE(token.empty());
	EXPECT_FALSE(error.empty());
	// Distinct from the plain no-pin-configured message — "set a PIN"
	// wouldn't help here, so it shouldn't say that.
	EXPECT_EQ(error.find("set one"), std::string::npos);
}

TEST_F(AuthStoreTest, SwitchProfile_RequirePasswordFalseStillAllowsCorrectAdminPin)
{
	ASSERT_TRUE(auth.createUser("normal_admin", "password", "admin"));
	const std::string admin_id = userIdFor("normal_admin");
	ASSERT_TRUE(auth.setPin(admin_id, "1234"));

	auto [token, error] = auth.switchProfile(admin_id, "1234", /*require_password_for_admin=*/false);
	EXPECT_FALSE(token.empty());
}

TEST_F(AuthStoreTest, SwitchProfile_RequirePasswordDoesNotAffectViewerPinSwitch)
{
	ASSERT_TRUE(auth.createUser("hardened_viewer", "password", "viewer"));
	const std::string viewer_id = userIdFor("hardened_viewer");
	ASSERT_TRUE(auth.setPin(viewer_id, "5678"));

	auto [token, error] = auth.switchProfile(viewer_id, "5678", /*require_password_for_admin=*/true);
	EXPECT_FALSE(token.empty());
}

TEST_F(AuthStoreTest, SwitchProfile_RequirePasswordDoesNotAffectUnlockedViewerSwitch)
{
	ASSERT_TRUE(auth.createUser("open_viewer", "password", "viewer"));
	const std::string viewer_id = userIdFor("open_viewer");
	// No PIN set at all.

	auto [token, error] = auth.switchProfile(viewer_id, "", /*require_password_for_admin=*/true);
	EXPECT_FALSE(token.empty());
}

TEST_F(AuthStoreTest, SwitchProfile_RequirePasswordDefaultsToFalseWhenOmitted)
{
	// The overload's own default parameter — every existing call site
	// (before this session's change) doesn't pass a third argument at all;
	// this pins that default staying permissive rather than silently
	// becoming hardened for every caller that hasn't opted in.
	ASSERT_TRUE(auth.createUser("default_admin", "password", "admin"));
	const std::string admin_id = userIdFor("default_admin");
	ASSERT_TRUE(auth.setPin(admin_id, "1234"));

	auto [token, error] = auth.switchProfile(admin_id, "1234");
	EXPECT_FALSE(token.empty());
}

// ---------------------------------------------------------------------------
// deleteUser / deleteUserCascade — accounts referenced by tables with no
// ON DELETE CASCADE (playback_history, show_track_preference,
// movie_track_preference, watch_together_session.host_user_id,
// watch_together_member) must still delete cleanly instead of throwing a
// foreign-key constraint violation. See AuthStore.h's own comment on
// deleteUserCascade for exactly which five tables these are.
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, DeleteUser_SucceedsForAccountWithPlaybackHistory)
{
	ASSERT_TRUE(auth.createUser("admin", "password", "admin"));
	ASSERT_TRUE(auth.createUser("viewer1", "password", "viewer"));
	const std::string admin_id  = userIdFor("admin");
	const std::string viewer_id = userIdFor("viewer1");

	SQLite::Statement ph(db.get(), R"(
        INSERT INTO playback_history
            (event_id, user_id, content_type, content_id, started_at_ms, ended_at_ms)
        VALUES ('e1', ?, 'movie', 'm1', 1000, 2000)
    )");
	ph.bind(1, viewer_id);
	ph.exec();

	EXPECT_TRUE(auth.deleteUser(viewer_id, admin_id));
	EXPECT_FALSE(findUser(viewer_id).has_value());
}

TEST_F(AuthStoreTest, DeleteUser_SucceedsForAccountWithTrackPreferences)
{
	ASSERT_TRUE(auth.createUser("admin", "password", "admin"));
	ASSERT_TRUE(auth.createUser("viewer1", "password", "viewer"));
	const std::string admin_id  = userIdFor("admin");
	const std::string viewer_id = userIdFor("viewer1");

	db.get().exec("INSERT INTO show (show_id, title) VALUES ('s1', 'Show One')");
	db.get().exec("INSERT INTO movie (movie_id, title, file_path, duration_ms) VALUES ('m1', 'Movie One', '/m.mkv', 7200000)");

	SQLite::Statement sp(db.get(),
						 "INSERT INTO show_track_preference (user_id, show_id, updated_at) VALUES (?, 's1', 1000)");
	sp.bind(1, viewer_id);
	sp.exec();

	SQLite::Statement mp(db.get(),
						 "INSERT INTO movie_track_preference (user_id, movie_id, updated_at) VALUES (?, 'm1', 1000)");
	mp.bind(1, viewer_id);
	mp.exec();

	EXPECT_TRUE(auth.deleteUser(viewer_id, admin_id));
	EXPECT_FALSE(findUser(viewer_id).has_value());
}

TEST_F(AuthStoreTest, DeleteUser_SucceedsForHostAndMemberOfAWatchTogetherSession)
{
	ASSERT_TRUE(auth.createUser("admin", "password", "admin"));
	ASSERT_TRUE(auth.createUser("host1", "password", "viewer"));
	ASSERT_TRUE(auth.createUser("member1", "password", "viewer"));
	const std::string admin_id  = userIdFor("admin");
	const std::string host_id   = userIdFor("host1");
	const std::string member_id = userIdFor("member1");

	SQLite::Statement s(db.get(), R"(
        INSERT INTO watch_together_session (session_id, host_user_id, content_type, content_id, created_at)
        VALUES ('wt1', ?, 'movie', 'm1', 1000)
    )");
	s.bind(1, host_id);
	s.exec();

	SQLite::Statement m(db.get(),
						"INSERT INTO watch_together_member (session_id, user_id, joined_at) VALUES ('wt1', ?, 1000)");
	m.bind(1, member_id);
	m.exec();

	// Both the host (host_user_id FK) and a plain member (user_id FK) must
	// delete cleanly.
	EXPECT_TRUE(auth.deleteUser(member_id, admin_id));
	EXPECT_TRUE(auth.deleteUser(host_id, admin_id));
	EXPECT_FALSE(findUser(host_id).has_value());
}

// ---------------------------------------------------------------------------
// Guest profiles
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, CreateGuestUser_CreatesPasswordlessViewerWithAUsableSession)
{
	auto [user_id, token] = auth.createGuestUser("Curious Visitor");
	ASSERT_FALSE(user_id.empty());
	ASSERT_FALSE(token.empty());

	auto u = findUser(user_id);
	ASSERT_TRUE(u.has_value());
	EXPECT_EQ(u->role, "viewer");
	EXPECT_TRUE(u->is_guest);

	auto validated = auth.validate(token);
	ASSERT_TRUE(validated.has_value());
	EXPECT_EQ(validated->user_id, user_id);
	EXPECT_TRUE(validated->is_guest);
}

TEST_F(AuthStoreTest, CreateGuestUser_NeverLoggableInWithAnyPassword)
{
	auto [user_id, token] = auth.createGuestUser("No Password Here");
	ASSERT_FALSE(user_id.empty());

	// Not even an empty password matches — insertUser stores an empty hash,
	// and bcrypt's checkPassword never treats an empty candidate specially.
	EXPECT_TRUE(auth.login("No Password Here", "").empty());
	EXPECT_TRUE(auth.login("No Password Here", "anything").empty());
}

TEST_F(AuthStoreTest, CreateGuestUser_TakenNameFails)
{
	ASSERT_FALSE(auth.createGuestUser("Taken").first.empty());
	auto [user_id, token] = auth.createGuestUser("Taken");
	EXPECT_TRUE(user_id.empty());
	EXPECT_TRUE(token.empty());
}

TEST_F(AuthStoreTest, CountActiveGuests_CountsOnlyGuestsNotRegularUsers)
{
	EXPECT_EQ(auth.countActiveGuests(), 0);
	ASSERT_TRUE(auth.createUser("regular", "password", "viewer"));
	EXPECT_EQ(auth.countActiveGuests(), 0);
	auth.createGuestUser("guest-a");
	auth.createGuestUser("guest-b");
	EXPECT_EQ(auth.countActiveGuests(), 2);
}

TEST_F(AuthStoreTest, UpdateGuestSelfSetup_SetsPinAndRestriction)
{
	auto [user_id, token] = auth.createGuestUser("Configurable Guest");
	ASSERT_FALSE(user_id.empty());

	auth.updateGuestSelfSetup(user_id, std::string("2468"), true,
							  std::string("TV-PG"), std::string("PG-13"), std::string("TV-14"));

	auto u = findUser(user_id);
	ASSERT_TRUE(u.has_value());
	EXPECT_TRUE(u->has_pin);
	EXPECT_TRUE(u->restricted);
	EXPECT_EQ(u->max_tv_rating, "TV-PG");
	EXPECT_EQ(u->max_movie_rating, "PG-13");
	EXPECT_EQ(u->max_channel_rating, "TV-14");
}

TEST_F(AuthStoreTest, UpdateGuestSelfSetup_OmittedFieldsLeaveExistingValuesAlone)
{
	auto [user_id, token] = auth.createGuestUser("Partial Update Guest");
	ASSERT_FALSE(user_id.empty());
	auth.updateGuestSelfSetup(user_id, std::nullopt, true,
							  std::string("TV-Y7"), std::nullopt, std::nullopt);

	// Only restricted+max_tv_rating supplied this time — max_movie_rating/
	// max_channel_rating (and the PIN) must be untouched from the call above.
	auth.updateGuestSelfSetup(user_id, std::nullopt, std::nullopt,
							  std::nullopt, std::string("R"), std::nullopt);

	auto u = findUser(user_id);
	ASSERT_TRUE(u.has_value());
	EXPECT_FALSE(u->has_pin);
	EXPECT_TRUE(u->restricted);           // still true from the first call
	EXPECT_EQ(u->max_tv_rating, "TV-Y7"); // untouched by the second call
	EXPECT_EQ(u->max_movie_rating, "R");  // set by the second call
}

TEST_F(AuthStoreTest, UpdateGuestSelfSetup_EmptyPinClearsIt)
{
	auto [user_id, token] = auth.createGuestUser("Clears Pin Guest");
	auth.updateGuestSelfSetup(user_id, std::string("1357"), std::nullopt, std::nullopt, std::nullopt, std::nullopt);
	ASSERT_TRUE(findUser(user_id)->has_pin);

	auth.updateGuestSelfSetup(user_id, std::string(""), std::nullopt, std::nullopt, std::nullopt, std::nullopt);
	EXPECT_FALSE(findUser(user_id)->has_pin);
}

TEST_F(AuthStoreTest, DeleteGuestSelf_DeletesTheAccount)
{
	auto [user_id, token] = auth.createGuestUser("Self Deleting Guest");
	ASSERT_FALSE(user_id.empty());
	EXPECT_TRUE(auth.deleteGuestSelf(user_id));
	EXPECT_FALSE(findUser(user_id).has_value());
}

TEST_F(AuthStoreTest, DeleteGuestSelf_RefusesToDeleteANonGuestAccount)
{
	ASSERT_TRUE(auth.createUser("realuser", "password", "viewer"));
	const std::string user_id = userIdFor("realuser");
	EXPECT_FALSE(auth.deleteGuestSelf(user_id));
	EXPECT_TRUE(findUser(user_id).has_value());
}

TEST_F(AuthStoreTest, DeleteGuestSelf_SucceedsEvenWithPlaybackHistory)
{
	auto [user_id, token] = auth.createGuestUser("Guest Who Watched Something");
	SQLite::Statement ph(db.get(), R"(
        INSERT INTO playback_history
            (event_id, user_id, content_type, content_id, started_at_ms, ended_at_ms)
        VALUES ('e1', ?, 'movie', 'm1', 1000, 2000)
    )");
	ph.bind(1, user_id);
	ph.exec();

	EXPECT_TRUE(auth.deleteGuestSelf(user_id));
	EXPECT_FALSE(findUser(user_id).has_value());
}

TEST_F(AuthStoreTest, DeleteGuestSelf_UnknownUserReturnsFalse)
{
	EXPECT_FALSE(auth.deleteGuestSelf("no-such-user"));
}

TEST_F(AuthStoreTest, PruneIdleGuests_RemovesOnlyGuestsIdleLongerThanTheWindow)
{
	auto [fresh_id, fresh_token] = auth.createGuestUser("Fresh Guest");
	auto [stale_id, stale_token] = auth.createGuestUser("Stale Guest");
	ASSERT_TRUE(auth.createUser("regular", "password", "viewer")); // never pruned regardless of age

	// Back-date the stale guest's session (and its created_at, so the
	// COALESCE fallback for a guest that never got touched again also lands
	// in the past) well outside a 1-hour idle window; leave the fresh
	// guest's session at "now" (set by createGuestUser itself).
	const int64_t long_ago = static_cast<int64_t>(std::time(nullptr)) - 10000;
	SQLite::Statement s(db.get(), "UPDATE session SET last_seen = ? WHERE user_id = ?");
	s.bind(1, long_ago);
	s.bind(2, stale_id);
	s.exec();
	SQLite::Statement u(db.get(), "UPDATE user SET created_at = ? WHERE user_id = ?");
	u.bind(1, long_ago);
	u.bind(2, stale_id);
	u.exec();

	const int pruned = auth.pruneIdleGuests(3600); // 1 hour idle window
	EXPECT_EQ(pruned, 1);
	EXPECT_FALSE(findUser(stale_id).has_value());
	EXPECT_TRUE(findUser(fresh_id).has_value());
	EXPECT_TRUE(userIdFor("regular").size() > 0);
}

TEST_F(AuthStoreTest, PruneIdleGuests_GuestWithNoSessionAtAllFallsBackToCreatedAt)
{
	auto [user_id, token] = auth.createGuestUser("Immediately Abandoned Guest");
	// Delete the session createGuestUser itself minted, simulating a guest
	// who never actually used the account it created for them.
	SQLite::Statement d(db.get(), "DELETE FROM session WHERE user_id = ?");
	d.bind(1, user_id);
	d.exec();

	const int64_t long_ago = static_cast<int64_t>(std::time(nullptr)) - 10000;
	SQLite::Statement u(db.get(), "UPDATE user SET created_at = ? WHERE user_id = ?");
	u.bind(1, long_ago);
	u.bind(2, user_id);
	u.exec();

	EXPECT_EQ(auth.pruneIdleGuests(3600), 1);
	EXPECT_FALSE(findUser(user_id).has_value());
}