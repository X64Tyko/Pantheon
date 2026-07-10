#include <gtest/gtest.h>
#include "auth/AuthStore.h"
#include "db/Database.h"
#include <algorithm>
#include <string>

class AuthStoreTest : public ::testing::Test {
protected:
    Database  db{ ":memory:" };
    AuthStore auth{ db };

    std::optional<AuthUser> findUser(const std::string& user_id) {
        for (const auto& u : auth.listUsers())
            if (u.user_id == user_id) return u;
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// createUserWithTempPassword
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, TempPassword_CreatesUsableAccount) {
    auto [user_id, temp_password] = auth.createUserWithTempPassword("alice", "viewer");
    ASSERT_FALSE(user_id.empty());
    ASSERT_FALSE(temp_password.empty());

    // The generated temp password logs in via the normal, unmodified path.
    const std::string token = auth.login("alice", temp_password);
    EXPECT_FALSE(token.empty());
}

TEST_F(AuthStoreTest, TempPassword_SetsMustChangePassword) {
    auto [user_id, temp_password] = auth.createUserWithTempPassword("bob", "viewer");
    ASSERT_FALSE(user_id.empty());
    auto u = findUser(user_id);
    ASSERT_TRUE(u.has_value());
    EXPECT_TRUE(u->must_change_password);
}

TEST_F(AuthStoreTest, TempPassword_DuplicateUsernameFails) {
    ASSERT_FALSE(auth.createUserWithTempPassword("carol", "viewer").first.empty());
    auto [user_id, temp_password] = auth.createUserWithTempPassword("carol", "viewer");
    EXPECT_TRUE(user_id.empty());
    EXPECT_TRUE(temp_password.empty());
}

// ---------------------------------------------------------------------------
// createUserWithEmailInvite / claimInvite
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, EmailInvite_AccountUnusableUntilClaimed) {
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

TEST_F(AuthStoreTest, EmailInvite_GetInviteUsernameReturnsUsernameForValidToken) {
    auto [user_id, invite_token] = auth.createUserWithEmailInvite("erin", "viewer");
    ASSERT_FALSE(user_id.empty());
    auto username = auth.getInviteUsername(invite_token);
    ASSERT_TRUE(username.has_value());
    EXPECT_EQ(*username, "erin");
}

TEST_F(AuthStoreTest, EmailInvite_GetInviteUsernameNulloptForUnknownToken) {
    EXPECT_FALSE(auth.getInviteUsername("not-a-real-token").has_value());
}

TEST_F(AuthStoreTest, EmailInvite_GetInviteUsernameNulloptForExpiredToken) {
    auto [user_id, invite_token] = auth.createUserWithEmailInvite("frank", "viewer", /*invite_ttl_seconds=*/-10);
    ASSERT_FALSE(user_id.empty());
    EXPECT_FALSE(auth.getInviteUsername(invite_token).has_value());
}

TEST_F(AuthStoreTest, ClaimInvite_SetsPasswordClearsFlagAndLogsIn) {
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

TEST_F(AuthStoreTest, ClaimInvite_TokenIsSingleUse) {
    auto [user_id, invite_token] = auth.createUserWithEmailInvite("heidi", "viewer");
    ASSERT_FALSE(user_id.empty());
    ASSERT_FALSE(auth.claimInvite(invite_token, "first-password").empty());

    // A second claim attempt with the same token must fail — the account
    // already has a real, self-chosen password at this point.
    EXPECT_TRUE(auth.claimInvite(invite_token, "second-password").empty());
}

TEST_F(AuthStoreTest, ClaimInvite_ExpiredTokenFails) {
    auto [user_id, invite_token] = auth.createUserWithEmailInvite("ivan", "viewer", /*invite_ttl_seconds=*/-10);
    ASSERT_FALSE(user_id.empty());
    EXPECT_TRUE(auth.claimInvite(invite_token, "some-password").empty());
}

TEST_F(AuthStoreTest, ClaimInvite_UnknownTokenFails) {
    EXPECT_TRUE(auth.claimInvite("not-a-real-token", "some-password").empty());
}

TEST_F(AuthStoreTest, ClaimInvite_EmptyPasswordRejected) {
    auto [user_id, invite_token] = auth.createUserWithEmailInvite("judy", "viewer");
    ASSERT_FALSE(user_id.empty());
    EXPECT_TRUE(auth.claimInvite(invite_token, "").empty());
}

// ---------------------------------------------------------------------------
// resendInvite
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, ResendInvite_IssuesFreshTokenWhileStillPending) {
    auto [user_id, first_token] = auth.createUserWithEmailInvite("kim", "viewer");
    ASSERT_FALSE(user_id.empty());

    const std::string second_token = auth.resendInvite(user_id);
    EXPECT_FALSE(second_token.empty());
    EXPECT_NE(second_token, first_token);

    // Both tokens resolve to the same account until one of them is claimed.
    EXPECT_TRUE(auth.getInviteUsername(first_token).has_value());
    EXPECT_TRUE(auth.getInviteUsername(second_token).has_value());
}

TEST_F(AuthStoreTest, ResendInvite_FailsOnceAccountHasARealPassword) {
    auto [user_id, invite_token] = auth.createUserWithEmailInvite("liam", "viewer");
    ASSERT_FALSE(user_id.empty());
    ASSERT_FALSE(auth.claimInvite(invite_token, "chosen-password").empty());

    EXPECT_TRUE(auth.resendInvite(user_id).empty());
}

TEST_F(AuthStoreTest, ResendInvite_FailsForUnknownUser) {
    EXPECT_TRUE(auth.resendInvite("no-such-user").empty());
}

// ---------------------------------------------------------------------------
// updateUser clearing must_change_password
// ---------------------------------------------------------------------------

TEST_F(AuthStoreTest, UpdateUser_PasswordChangeClearsMustChangePassword) {
    auto [user_id, temp_password] = auth.createUserWithTempPassword("mallory", "viewer");
    ASSERT_FALSE(user_id.empty());
    ASSERT_TRUE(findUser(user_id)->must_change_password);

    ASSERT_TRUE(auth.updateUser(user_id, "a-chosen-password", ""));
    EXPECT_FALSE(findUser(user_id)->must_change_password);

    // The new password works; the old temp one no longer does.
    EXPECT_FALSE(auth.login("mallory", "a-chosen-password").empty());
    EXPECT_TRUE(auth.login("mallory", temp_password).empty());
}

TEST_F(AuthStoreTest, UpdateUser_RoleOnlyChangeDoesNotDisturbFlag) {
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

TEST_F(AuthStoreTest, PlainCreateUser_NeverSetsMustChangePassword) {
    ASSERT_TRUE(auth.createUser("oscar", "chosen-password", "viewer"));
    auto users = auth.listUsers();
    auto it = std::find_if(users.begin(), users.end(),
        [](const AuthUser& u) { return u.username == "oscar"; });
    ASSERT_NE(it, users.end());
    EXPECT_FALSE(it->must_change_password);
}
