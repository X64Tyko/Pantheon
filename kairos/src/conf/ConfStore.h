#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Reads and writes a simple INI-style credentials file (kairos.conf).
// Hot-reload: call maybeReload() before any lookup; it re-parses if the file's
// mtime has changed (e.g. after a UI update via the API).
class ConfStore {
public:
    explicit ConfStore(std::string path);

    void maybeReload();

    std::string token(const std::string& source_id)  const;
    std::string userId(const std::string& source_id) const;
    bool hasToken(const std::string& source_id)  const;
    bool hasUserId(const std::string& source_id) const;
    std::vector<std::string> allSources() const;

    void setCredentials(const std::string& source_id,
                        const std::string& token,
                        const std::string& user_id);
    void removeSource(const std::string& source_id);

private:
    struct Entry { std::string token, user_id; };

    void loadLocked();
    void parseLocked(const std::string& text);
    void saveLocked() const;

    std::string                              path_;
    std::filesystem::file_time_type          mtime_{};
    std::unordered_map<std::string, Entry>   entries_;
    mutable std::mutex                       mu_;

    // Throttle maybeReload() to at most once per 30 seconds.
    mutable std::atomic<std::chrono::steady_clock::rep> last_check_ns_{0};
};
