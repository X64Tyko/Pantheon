#pragma once
#include "../auth/AuthStore.h"
#include <optional>

// Thread-local authenticated user, set by the Router pre-routing handler.
// Valid only during the lifetime of a single request on the calling thread.

const std::optional<AuthUser>& currentUser();
void setCurrentUser(std::optional<AuthUser> user);
void clearCurrentUser();
