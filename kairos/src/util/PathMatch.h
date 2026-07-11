#pragma once
#include <string>

// Folder/path comparison helpers shared by SyncManager (cross-source sync-time
// dedup) and ContentRepository/ContentService (manual merge's folder-mismatch
// check). Callers are expected to have already applied
// ConfStore::applyPathMap() to both sides before calling into here — this
// utility stays ConfStore-free so it's independently testable.
namespace pathutil {

// Parent directory of a file path (everything before the last '/'). Empty
// string if there's no '/' in the path.
std::string parentDir(const std::string& file_path);

// Strips trailing slash(es) and collapses repeated '/'. Deliberately cheap
// and safe only — no case-folding, no symlink resolution.
std::string normalizeCheap(const std::string& path);

// normalizeCheap() + lowercase. Used only for the case-insensitive fallback
// comparison — never sufficient on its own to auto-merge, only to flag a
// pair as an uncertain duplicate candidate for human review.
std::string normalizeCaseInsensitive(const std::string& path);

enum class FolderMatch { kNone, kExactCheap, kCaseInsensitiveOnly };

// Compares two already path-mapped folder paths.
FolderMatch compareFolders(const std::string& mapped_a, const std::string& mapped_b);

} // namespace pathutil
