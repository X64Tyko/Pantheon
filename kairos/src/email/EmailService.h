#pragma once
#include <string>

class Database;

// Minimal SMTP client — STARTTLS on the standard submission port (587) only,
// AUTH LOGIN, plain-text bodies. Covers the common relay setups (Gmail,
// SendGrid, Mailgun, self-hosted Postfix). Implicit-TLS-only relays (port
// 465) aren't supported. Settings are read from app_config (via
// ConfigRepository) on every call rather than cached, so a Settings-page
// change takes effect on the very next send.
class EmailService {
public:
    explicit EmailService(Database& db);

    // Sends the invite-claim link to a newly-imported/invited account.
    // Best-effort: a failure here should never block account creation —
    // callers still have the invite token/link to hand out manually.
    // On failure, *error (if non-null) is set to a human-readable reason.
    bool sendInviteEmail(const std::string& to_address,
                         const std::string& to_display_name,
                         const std::string& invite_link,
                         std::string* error = nullptr) const;

    // Sends a minimal test message — backs the Settings page's "Send Test
    // Email" button so SMTP settings can be verified before relying on them.
    bool testConnection(const std::string& to_address, std::string* error = nullptr) const;

private:
    struct SmtpConfig {
        std::string host, username, password, from_address;
        int         port = 587;
    };
    SmtpConfig loadConfig() const;

    bool send(const SmtpConfig& cfg, const std::string& to_address,
             const std::string& subject, const std::string& body,
             std::string* error) const;

    Database& db_;
};
