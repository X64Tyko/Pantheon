#include "AuthContext.h"

static thread_local std::optional<AuthUser> tl_current_user;

const std::optional<AuthUser>& currentUser() { return tl_current_user; }
void setCurrentUser(std::optional<AuthUser> user) { tl_current_user = std::move(user); }
void clearCurrentUser() { tl_current_user = std::nullopt; }
