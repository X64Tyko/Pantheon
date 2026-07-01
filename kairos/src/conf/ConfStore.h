#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Reads and writes a simple INI-style credentials file (kairos.conf).
// Hot-reload: call maybeReload() before any lookup; it re-parses if the file's
// mtime has changed (e.g. after a UI update via the API).
//
// Per-source path_map lines rewrite file paths returned by the /now endpoint
// so that media server paths (e.g. /data on Plex) can be translated to paths
// accessible on the machine running Tunarr (e.g. /media in Docker, or an NFS
// mount path in dev). Multiple path_map lines per section are supported.
//
// Example:
//   [my_plex]
//   token = abc123
//   path_map = /data:/media
class ConfStore {
public:
    explicit ConfStore(std::string path);

    void maybeReload();

    std::string token(const std::string& source_id)  const;
    std::string userId(const std::string& source_id) const;
    bool hasToken(const std::string& source_id)  const;
    bool hasUserId(const std::string& source_id) const;
    std::vector<std::string> allSources() const;

    std::string getDownloadPath() const;
    void        setDownloadPath(const std::string& path);

    int  getImageCacheTtlHours() const;
    void setImageCacheTtlHours(int hours);
	int  getBufferSize() const;
	void setBufferSize(int size);

    // Rewrite a file path by applying the first matching path_map prefix across
    // all configured sources. Returns the path unchanged if no mapping matches.
    std::string applyPathMap(const std::string& path) const;

    std::vector<std::pair<std::string,std::string>> getPathMaps(const std::string& source_id) const;
    void setPathMaps(const std::string& source_id,
                     const std::vector<std::pair<std::string,std::string>>& maps);

    void setCredentials(const std::string& source_id,
                        const std::string& token,
                        const std::string& user_id);
    void removeSource(const std::string& source_id);

private:
    struct Entry {
        std::string token, user_id;
        // Each pair is {from_prefix, to_prefix}.
        std::vector<std::pair<std::string, std::string>> path_maps;
    };

    void loadLocked();
    void parseLocked(const std::string& text);
    void saveLocked() const;

    std::string                              path_;
    std::filesystem::file_time_type          mtime_{};
    std::unordered_map<std::string, Entry>   entries_;
    std::string                              download_path_;
	int buffer_size_ = 1048576; // 1024 KB
    int                                      image_cache_ttl_hours_ = 2;
    mutable std::mutex                       mu_;

    // Throttle maybeReload() to at most once per 30 seconds.
    mutable std::atomic<std::chrono::steady_clock::rep> last_check_ns_{0};
};
