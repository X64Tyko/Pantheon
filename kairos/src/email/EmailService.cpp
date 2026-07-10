#include "EmailService.h"
#include "../db/ConfigRepository.h"
#include "../db/Database.h"
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {

constexpr int kConnectTimeoutSec = 10;
constexpr int kIoTimeoutSec      = 15;

std::string base64Encode(const std::string& in) {
    std::string out;
    out.resize(4 * ((in.size() + 2) / 3) + 1);
    int len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()),
                              reinterpret_cast<const unsigned char*>(in.data()),
                              static_cast<int>(in.size()));
    out.resize(static_cast<size_t>(len));
    return out;
}

// SMTP DATA transparency: any body line starting with '.' must be escaped by
// doubling it, or the server reads it as the end-of-message terminator.
std::string dotStuff(const std::string& body) {
    std::string out;
    bool at_line_start = true;
    for (char c : body) {
        if (at_line_start && c == '.') out += '.';
        out += c;
        at_line_start = (c == '\n');
    }
    return out;
}

// Wraps either a raw socket (pre-STARTTLS) or an SSL* (post-STARTTLS) behind
// one read/write interface so the SMTP conversation code below doesn't need
// to know which phase it's in.
class Conn {
public:
    explicit Conn(int fd) : fd_(fd) {}
    ~Conn() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); }
        if (ctx_) SSL_CTX_free(ctx_);
        if (fd_ >= 0) close(fd_);
    }
    Conn(const Conn&) = delete;
    Conn& operator=(const Conn&) = delete;

    bool upgradeTls(const std::string& host, std::string* error) {
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) { if (error) *error = "SSL_CTX_new failed"; return false; }
        ssl_ = SSL_new(ctx_);
        if (!ssl_) { if (error) *error = "SSL_new failed"; return false; }
        SSL_set_tlsext_host_name(ssl_, host.c_str()); // SNI
        SSL_set_fd(ssl_, fd_);
        if (SSL_connect(ssl_) != 1) {
            if (error) *error = "TLS handshake failed";
            return false;
        }
        return true;
    }

    bool writeLine(const std::string& line) {
        const std::string out = line + "\r\n";
        size_t sent = 0;
        while (sent < out.size()) {
            const int n = ssl_
                ? SSL_write(ssl_, out.data() + sent, static_cast<int>(out.size() - sent))
                : static_cast<int>(::send(fd_, out.data() + sent, out.size() - sent, 0));
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // Reads one full (possibly multi-line) SMTP response and returns its
    // numeric status code, or -1 on read failure/timeout.
    int readResponse() {
        int code = -1;
        while (true) {
            std::string line;
            if (!readLine(line)) return -1;
            if (line.size() < 4) return -1;
            code = std::atoi(line.substr(0, 3).c_str());
            if (line[3] == ' ') break; // final line of a (possibly multi-line) response
            // '-' at position 3 means more lines follow — keep reading.
        }
        return code;
    }

private:
    bool readLine(std::string& out) {
        out.clear();
        char c;
        while (true) {
            const int n = ssl_ ? SSL_read(ssl_, &c, 1) : static_cast<int>(::recv(fd_, &c, 1, 0));
            if (n <= 0) return false;
            if (c == '\n') { if (!out.empty() && out.back() == '\r') out.pop_back(); return true; }
            out += c;
            if (out.size() > 8192) return false; // sanity bound against a misbehaving server
        }
    }

    int      fd_;
    SSL_CTX* ctx_ = nullptr;
    SSL*     ssl_ = nullptr;
};

bool connectSocket(const std::string& host, int port, int* out_fd, std::string* error) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        if (error) *error = "DNS resolution failed for " + host;
        return false;
    }

    int fd = -1;
    for (addrinfo* p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        timeval connect_tv{kConnectTimeoutSec, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &connect_tv, sizeof(connect_tv));
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &connect_tv, sizeof(connect_tv));
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        if (error) *error = "connect() failed to " + host + ":" + std::to_string(port);
        return false;
    }

    timeval io_tv{kIoTimeoutSec, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_tv, sizeof(io_tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_tv, sizeof(io_tv));
    *out_fd = fd;
    return true;
}

} // namespace

EmailService::EmailService(Database& db) : db_(db) {}

EmailService::SmtpConfig EmailService::loadConfig() const {
    ConfigRepository repo(db_);
    SmtpConfig cfg;
    cfg.host         = repo.getValue("smtp_host");
    cfg.username     = repo.getValue("smtp_username");
    cfg.password     = repo.getValue("smtp_password");
    cfg.from_address = repo.getValue("smtp_from_address");
    const std::string port_str = repo.getValue("smtp_port");
    if (!port_str.empty()) {
        try { cfg.port = std::stoi(port_str); } catch (...) { cfg.port = 587; }
    }
    return cfg;
}

bool EmailService::send(const SmtpConfig& cfg, const std::string& to_address,
                        const std::string& subject, const std::string& body,
                        std::string* error) const {
    if (cfg.host.empty() || cfg.from_address.empty()) {
        if (error) *error = "SMTP is not configured (host/from address missing)";
        return false;
    }

    int fd = -1;
    if (!connectSocket(cfg.host, cfg.port, &fd, error)) return false;
    Conn conn(fd);

    auto fail = [&](const char* msg) { if (error) *error = msg; return false; };

    if (conn.readResponse() != 220)                     return fail("no greeting from server");
    if (!conn.writeLine("EHLO pantheon"))                return fail("write failed (EHLO)");
    if (conn.readResponse() != 250)                      return fail("EHLO rejected");

    if (!conn.writeLine("STARTTLS"))                     return fail("write failed (STARTTLS)");
    if (conn.readResponse() != 220)                      return fail("STARTTLS rejected — server may require implicit TLS on port 465, which isn't supported");
    if (!conn.upgradeTls(cfg.host, error))               return false;

    // RFC 3207: the client must discard prior EHLO capabilities and re-issue
    // EHLO once the TLS session is established.
    if (!conn.writeLine("EHLO pantheon"))                return fail("write failed (EHLO/TLS)");
    if (conn.readResponse() != 250)                      return fail("EHLO rejected after STARTTLS");

    if (!cfg.username.empty()) {
        if (!conn.writeLine("AUTH LOGIN"))               return fail("write failed (AUTH LOGIN)");
        if (conn.readResponse() != 334)                  return fail("AUTH LOGIN not accepted");
        if (!conn.writeLine(base64Encode(cfg.username)))  return fail("write failed (auth username)");
        if (conn.readResponse() != 334)                  return fail("username rejected");
        if (!conn.writeLine(base64Encode(cfg.password)))  return fail("write failed (auth password)");
        if (conn.readResponse() != 235)                  return fail("authentication failed");
    }

    if (!conn.writeLine("MAIL FROM:<" + cfg.from_address + ">")) return fail("write failed (MAIL FROM)");
    if (conn.readResponse() != 250)                      return fail("MAIL FROM rejected");
    if (!conn.writeLine("RCPT TO:<" + to_address + ">")) return fail("write failed (RCPT TO)");
    if (conn.readResponse() != 250)                      return fail("recipient rejected");
    if (!conn.writeLine("DATA"))                          return fail("write failed (DATA)");
    if (conn.readResponse() != 354)                       return fail("DATA rejected");

    std::ostringstream msg;
    msg << "From: Pantheon <" << cfg.from_address << ">\r\n"
        << "To: <" << to_address << ">\r\n"
        << "Subject: " << subject << "\r\n"
        << "Content-Type: text/plain; charset=utf-8\r\n"
        << "\r\n"
        << dotStuff(body) << "\r\n"
        << ".";
    if (!conn.writeLine(msg.str()))                       return fail("write failed (message body)");
    if (conn.readResponse() != 250)                        return fail("message rejected by server");

    conn.writeLine("QUIT");
    return true;
}

bool EmailService::sendInviteEmail(const std::string& to_address, const std::string& to_display_name,
                                   const std::string& invite_link, std::string* error) const {
    const SmtpConfig cfg = loadConfig();
    std::ostringstream body;
    body << "Hi " << (to_display_name.empty() ? to_address : to_display_name) << ",\r\n\r\n"
         << "An admin has set up a Pantheon account for you. Follow the link below to choose your password:\r\n\r\n"
         << invite_link << "\r\n\r\n"
         << "This link expires in 7 days and can only be used once.\r\n";
    return send(cfg, to_address, "You've been invited to Pantheon", body.str(), error);
}

bool EmailService::testConnection(const std::string& to_address, std::string* error) const {
    const SmtpConfig cfg = loadConfig();
    return send(cfg, to_address, "Pantheon SMTP test",
               "This is a test email from Pantheon to confirm your SMTP settings are working.\r\n",
               error);
}
