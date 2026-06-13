#include "ConfStore.h"
#include <fstream>
#include <iostream>
#include <sstream>

// ---------------------------------------------------------------------------
// Format example (kairos.conf):
//
//   # Kairos credentials — do not commit
//
//   [plex_home]
//   token = abc123
//
//   [jellyfin_main]
//   token = xyz789
//   user_id = myuser
// ---------------------------------------------------------------------------

namespace {
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s = s.substr(1);
    return s;
}
} // namespace

// ---------------------------------------------------------------------------

ConfStore::ConfStore(std::string path) : path_(std::move(path)) {
    std::lock_guard lock(mu_);
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) {
        saveLocked(); // write default (empty) conf so the file exists
        std::cout << "[conf] created default " << path_ << '\n';
    } else {
        loadLocked();
    }
    std::cout << "[conf] " << path_ << " — "
              << entries_.size() << " source(s) configured\n";
}

void ConfStore::maybeReload() {
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(path_, ec);
    if (ec) return;

    std::lock_guard lock(mu_);
    if (mtime == mtime_) return;

    loadLocked();
    std::cout << "[conf] reloaded " << path_ << '\n';
}

void ConfStore::loadLocked() {
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(path_, ec);
    if (ec) { entries_.clear(); mtime_ = {}; return; }

    std::ifstream f(path_);
    std::ostringstream ss;
    ss << f.rdbuf();
    entries_.clear();
    parseLocked(ss.str());
    mtime_ = mtime;
}

void ConfStore::parseLocked(const std::string& text) {
    std::istringstream iss(text);
    std::string line, section;
    while (std::getline(iss, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[') {
            auto close = line.rfind(']');
            section = (close != std::string::npos) ? line.substr(1, close - 1) : "";
            continue;
        }
        if (section.empty()) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = trim(line.substr(0, eq));
        auto val = trim(line.substr(eq + 1));
        if (key == "token")   entries_[section].token   = val;
        if (key == "user_id") entries_[section].user_id = val;
    }
}

// ---------------------------------------------------------------------------

std::string ConfStore::token(const std::string& source_id) const {
    std::lock_guard lock(mu_);
    auto it = entries_.find(source_id);
    return (it != entries_.end()) ? it->second.token : "";
}

std::string ConfStore::userId(const std::string& source_id) const {
    std::lock_guard lock(mu_);
    auto it = entries_.find(source_id);
    return (it != entries_.end()) ? it->second.user_id : "";
}

bool ConfStore::hasToken(const std::string& source_id) const {
    std::lock_guard lock(mu_);
    auto it = entries_.find(source_id);
    return it != entries_.end() && !it->second.token.empty();
}

bool ConfStore::hasUserId(const std::string& source_id) const {
    std::lock_guard lock(mu_);
    auto it = entries_.find(source_id);
    return it != entries_.end() && !it->second.user_id.empty();
}

std::vector<std::string> ConfStore::allSources() const {
    std::lock_guard lock(mu_);
    std::vector<std::string> ids;
    ids.reserve(entries_.size());
    for (const auto& [k, _] : entries_) ids.push_back(k);
    return ids;
}

// ---------------------------------------------------------------------------

void ConfStore::setCredentials(const std::string& source_id,
                                const std::string& tkn,
                                const std::string& uid) {
    std::lock_guard lock(mu_);
    entries_[source_id] = {tkn, uid};
    saveLocked();
    std::error_code ec;
    mtime_ = std::filesystem::last_write_time(path_, ec);
}

void ConfStore::removeSource(const std::string& source_id) {
    std::lock_guard lock(mu_);
    entries_.erase(source_id);
    saveLocked();
    std::error_code ec;
    mtime_ = std::filesystem::last_write_time(path_, ec);
}

void ConfStore::saveLocked() const {
    std::ofstream f(path_);
    f << "# Kairos credentials — do not commit\n"
      << "# Managed by Hades UI\n\n";
    for (const auto& [sid, e] : entries_) {
        f << "[" << sid << "]\n";
        if (!e.token.empty())   f << "token = "   << e.token   << "\n";
        if (!e.user_id.empty()) f << "user_id = " << e.user_id << "\n";
        f << "\n";
    }
}
