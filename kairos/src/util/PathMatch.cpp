#include "PathMatch.h"
#include <algorithm>
#include <cctype>

namespace pathutil {

std::string parentDir(const std::string& file_path) {
    auto slash = file_path.find_last_of('/');
    return slash == std::string::npos ? std::string() : file_path.substr(0, slash);
}

std::string normalizeCheap(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    char prev = '\0';
    for (char c : path) {
        if (c == '/' && prev == '/') continue; // collapse repeated slashes
        out += c;
        prev = c;
    }
    while (out.size() > 1 && out.back() == '/') out.pop_back();
    return out;
}

std::string normalizeCaseInsensitive(const std::string& path) {
    std::string out = normalizeCheap(path);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

FolderMatch compareFolders(const std::string& mapped_a, const std::string& mapped_b) {
    if (mapped_a.empty() || mapped_b.empty()) return FolderMatch::kNone;
    if (normalizeCheap(mapped_a) == normalizeCheap(mapped_b)) return FolderMatch::kExactCheap;
    if (normalizeCaseInsensitive(mapped_a) == normalizeCaseInsensitive(mapped_b)) return FolderMatch::kCaseInsensitiveOnly;
    return FolderMatch::kNone;
}

} // namespace pathutil
